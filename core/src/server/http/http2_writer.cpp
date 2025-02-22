#include <server/http/http2_writer.hpp>

#include <fmt/format.h>

#include <server/http/http2_session.hpp>
#include <server/http/http_cached_date.hpp>
#include <server/http/http_request_parser.hpp>

#include <userver/http/common_headers.hpp>
#include <userver/http/predefined_header.hpp>
#include <userver/logging/log.hpp>
#include <userver/server/http/http_response.hpp>
#include <userver/utils/impl/projecting_view.hpp>
#include <userver/utils/small_string.hpp>

USERVER_NAMESPACE_BEGIN

namespace server::http {

namespace {

bool IsBodyForbiddenForStatus(HttpStatus status) {
  return status == HttpStatus::kNoContent ||
         status == HttpStatus::kNotModified ||
         (static_cast<int>(status) >= 100 && static_cast<int>(status) < 200);
}

}  // namespace

DataBufferSender::DataBufferSender(std::string&& data) : data(std::move(data)) {
  nghttp2_provider.read_callback = NgHttp2ReadCallback;
  nghttp2_provider.source.ptr = this;
}

DataBufferSender::DataBufferSender(DataBufferSender&& o) noexcept
    : data(std::move(o.data)), sended_bytes(o.sended_bytes) {
  nghttp2_provider.source.ptr = this;
  nghttp2_provider.read_callback = NgHttp2ReadCallback;
}

// implements
// https://nghttp2.org/documentation/types.html#c.nghttp2_data_source_read_callback
ssize_t NgHttp2ReadCallback(nghttp2_session*, int32_t, uint8_t*,
                            std::size_t max_len, uint32_t* flags,
                            nghttp2_data_source* source, void*) {
  UASSERT(source);
  UASSERT(source->ptr);
  auto& sender = *static_cast<DataBufferSender*>(source->ptr);

  std::size_t remaining = sender.data.size() - sender.sended_bytes;

  remaining = std::min(remaining, max_len);

  *flags = NGHTTP2_DATA_FLAG_NONE;
  UASSERT(sender.sended_bytes + remaining <= sender.data.size());
  if (sender.sended_bytes + remaining == sender.data.size()) {
    *flags |= NGHTTP2_DATA_FLAG_EOF;
  }
  *flags |= NGHTTP2_DATA_FLAG_NO_COPY;
  return remaining;
}

nghttp2_nv UnsafeHeaderToNGHeader(std::string_view name, std::string_view value,
                                  const bool sensitive) {
  nghttp2_nv result;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  result.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name.data()));

  result.namelen = name.size();

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast),
  result.value = reinterpret_cast<uint8_t*>(const_cast<char*>(value.data()));
  result.valuelen = value.size();
  // no_copy_name -- we must to lower case all headers
  // no_copy_value -- we must to store all values until
  // nghttp2_on_frame_send_callback or nghttp2_on_frame_not_send_callback not
  // called result.flags = NGHTTP2_NV_FLAG_NO_COPY_VALUE |
  // NGHTTP2_NV_FLAG_NO_COPY_NAME;
  result.flags = NGHTTP2_NV_FLAG_NONE;
  if (sensitive) {
    result.flags |= NGHTTP2_NV_FLAG_NO_INDEX;
  }

  return result;
}

class Http2HeaderWriter final {
 public:
  // We must borrow key-value pairs until the `WriteToSocket` is called
  explicit Http2HeaderWriter(std::size_t nheaders) {
    values_.reserve(nheaders);
  }

  void AddKeyValue(std::string_view key, std::string&& value) {
    const auto* ptr = values_.data();
    values_.push_back(std::move(value));
    UASSERT(ptr == values_.data());
    ng_headers_.push_back(UnsafeHeaderToNGHeader(key, values_.back(), false));
    bytes_ += (key.size() + value.size());
  }

  void AddKeyValue(std::string_view key, std::string_view value) {
    ng_headers_.push_back(UnsafeHeaderToNGHeader(key, value, false));
    bytes_ += (key.size() + value.size());
  }

  void AddCookie(const Cookie& cookie) {
    USERVER_NAMESPACE::http::headers::HeadersString val;
    cookie.AppendToString(val);
    const auto* ptr = values_.data();
    // TODO: avoid copy
    values_.push_back(std::string{val.data(), val.size()});
    UASSERT(ptr == values_.data());
    ng_headers_.push_back(UnsafeHeaderToNGHeader(
        USERVER_NAMESPACE::http::headers::kSetCookie, values_.back(), false));
    const std::string_view key = USERVER_NAMESPACE::http::headers::kSetCookie;
    bytes_ += (key.size() + val.size());
  }

  // A dirty size before HPACK
  std::size_t GetSize() const { return bytes_; }

  std::vector<nghttp2_nv>& GetNgHeaders() { return ng_headers_; }

 private:
  std::vector<std::string> values_;
  std::vector<nghttp2_nv> ng_headers_;
  std::size_t bytes_{0};
};

class Http2ResponseWriter final {
 public:
  Http2ResponseWriter(HttpResponse& response, Http2Session& session)
      : http2_session_(session), response_(response) {}

  void WriteHttpResponse() {
    auto headers = WriteHeaders();
    if (response_.IsBodyStreamed() && response_.GetData().empty()) {
      WriteHttp2BodyStreamed(headers);
    } else {
      // e.g. a CustomHandlerException
      WriteHttp2BodyNotstreamed(headers);
    }
  }

 private:
  Http2HeaderWriter WriteHeaders() {
    // Preallocate space for all headers
    Http2HeaderWriter header_writer{response_.headers_.size() +
                                    response_.cookies_.size() + 3};

    header_writer.AddKeyValue(
        USERVER_NAMESPACE::http::headers::k2::kStatus,
        fmt::to_string(static_cast<std::uint16_t>(response_.status_)));

    const auto& headers = response_.headers_;
    const auto end = headers.cend();
    if (headers.find(USERVER_NAMESPACE::http::headers::kDate) == end) {
      header_writer.AddKeyValue(USERVER_NAMESPACE::http::headers::kDate,
                                std::string{impl::GetCachedDate()});
    }
    if (headers.find(USERVER_NAMESPACE::http::headers::kContentType) == end) {
      header_writer.AddKeyValue(USERVER_NAMESPACE::http::headers::kContentType,
                                kDefaultContentType);
    }
    for (const auto& [key, value] : headers) {
      if (key ==
          std::string{USERVER_NAMESPACE::http::headers::kContentLength}) {
        continue;
      }
      header_writer.AddKeyValue(key, value);
    }
    for (const auto& value :
         USERVER_NAMESPACE::utils::impl::MakeValuesView(response_.cookies_)) {
      header_writer.AddCookie(value);
    }
    return header_writer;
  }

  void WriteHttp2BodyNotstreamed(Http2HeaderWriter& header_writer) {
    const bool is_body_forbidden = IsBodyForbiddenForStatus(response_.status_);
    const bool is_head_request =
        response_.request_.GetMethod() == HttpMethod::kHead;
    auto data = response_.MoveData();

    if (!is_body_forbidden) {
      header_writer.AddKeyValue(
          USERVER_NAMESPACE::http::headers::kContentLength,
          fmt::to_string(data.size()));
    }

    if (is_body_forbidden && !data.empty()) {
      LOG_LIMITED_WARNING()
          << "Non-empty body provided for response with HTTP2 code "
          << static_cast<int>(response_.status_)
          << " which does not allow one, it will be dropped";
    }

    const std::uint32_t stream_id = response_.GetStreamId().value();
    auto& stream = http2_session_.GetStreamChecked(stream_id);
    stream.data_buffer_sender.emplace(std::move(data));
    std::size_t bytes = header_writer.GetSize();

    nghttp2_data_provider* provider{nullptr};
    if (!is_head_request && !is_body_forbidden) {
      provider = &stream.data_buffer_sender.value().nghttp2_provider;
      bytes += stream.data_buffer_sender->data.size();
    }
    const auto& nva = header_writer.GetNgHeaders();
    const int rv =
        nghttp2_submit_response(http2_session_.GetNghttp2SessionPtr(),
                                stream_id, nva.data(), nva.size(), provider);
    if (rv != 0) {
      throw std::runtime_error{
          fmt::format("Fail to submit the response with err id = {}. Err: {}",
                      rv, nghttp2_strerror(rv))};
    }

    nghttp2_session_send(http2_session_.GetNghttp2SessionPtr());
    response_.SetSent(bytes, std::chrono::steady_clock::now());
  }

  void WriteHttp2BodyStreamed(Http2HeaderWriter&) {
    UINVARIANT(false, "Not implemented for HTTP2.0");
  }

  Http2Session& http2_session_;
  HttpResponse& response_;
};

void WriteHttp2ResponseToSocket(HttpResponse& response, Http2Session& session) {
  Http2ResponseWriter w{response, session};
  w.WriteHttpResponse();
}

}  // namespace server::http

USERVER_NAMESPACE_END
