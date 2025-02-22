#include <userver/server/request/response_base.hpp>

#include <userver/utils/assert.hpp>

USERVER_NAMESPACE_BEGIN

namespace server::request {

namespace {
const auto kStartTime = std::chrono::steady_clock::now();

std::chrono::milliseconds ToMsFromStart(
    std::chrono::steady_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(tp - kStartTime);
}

}  // namespace

void ResponseDataAccounter::StartRequest(
    size_t size, std::chrono::steady_clock::time_point create_time) {
  count_.Add(1);
  current_ += size;
  auto ms = ToMsFromStart(create_time);
  time_sum_.Add(ms.count());
}

void ResponseDataAccounter::StopRequest(
    size_t size, std::chrono::steady_clock::time_point create_time) {
  current_ -= size;
  auto ms = ToMsFromStart(create_time);
  time_sum_.Subtract(ms.count());
  count_.Subtract(1);
}

std::chrono::milliseconds ResponseDataAccounter::GetAvgRequestTime() const {
  // TODO: race
  auto count = count_.NonNegativeRead();
  auto time_sum = std::chrono::milliseconds(time_sum_.NonNegativeRead());

  auto now_ms = ToMsFromStart(std::chrono::steady_clock::now());
  auto delta = (now_ms * count) - time_sum;
  return delta / (count ? count : 1);
}

ResponseBase::ResponseBase(ResponseDataAccounter& data_accounter)
    : ResponseBase{data_accounter, std::chrono::steady_clock::now()} {}

ResponseBase::ResponseBase(ResponseDataAccounter& data_account,
                           std::chrono::steady_clock::time_point now)
    : accounter_{data_account}, create_time_{now} {
  guard_.emplace(accounter_, create_time_, data_.size());
}

ResponseBase::~ResponseBase() noexcept {
  UASSERT_MSG(!is_sent_ || !guard_,
              "SetDone must be called explicitly within Server's lifetime, "
              "otherwise guard_ may violate Server's lifetime");
}

void ResponseBase::SetData(std::string data) {
  create_time_ = std::chrono::steady_clock::now();
  data_ = std::move(data);
  guard_.emplace(accounter_, create_time_, data_.size());
}

void ResponseBase::SetReady() { SetReady(std::chrono::steady_clock::now()); }

void ResponseBase::SetReady(std::chrono::steady_clock::time_point now) {
  ready_time_ = now;
  is_ready_ = true;
}

bool ResponseBase::IsLimitReached() const {
  return accounter_.GetCurrentLevel() >= accounter_.GetMaxLevel();
}

void ResponseBase::SetSendFailed(
    std::chrono::steady_clock::time_point failure_time) {
  SetSent(0, failure_time);
}

void ResponseBase::SetSent(std::size_t bytes_sent,
                           std::chrono::steady_clock::time_point sent_time) {
  UASSERT(!is_sent_);
  UASSERT(guard_);
  is_sent_ = true;
  bytes_sent_ = bytes_sent;
  sent_time_ = sent_time;
  guard_.reset();
}

void ResponseBase::SetStreamId(std::uint32_t stream_id) {
  UASSERT(!stream_id_.has_value());
  stream_id_.emplace(stream_id);
}

}  // namespace server::request

USERVER_NAMESPACE_END
