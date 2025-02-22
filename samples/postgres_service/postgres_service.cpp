#include <userver/clients/dns/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>

#include <userver/utest/using_namespace_userver.hpp>

/// [Postgres service sample - component]
#include <userver/components/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/server/handlers/http_handler_base.hpp>
#include <userver/utils/daemon_run.hpp>

#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>

namespace samples::pg {

class KeyValue final : public server::handlers::HttpHandlerBase {
 public:
  static constexpr std::string_view kName = "handler-key-value";

  KeyValue(const components::ComponentConfig& config,
           const components::ComponentContext& context);

  std::string HandleRequestThrow(
      const server::http::HttpRequest& request,
      server::request::RequestContext&) const override;

 private:
  std::string GetValue(std::string_view key,
                       const server::http::HttpRequest& request) const;
  std::string PostValue(std::string_view key,
                        const server::http::HttpRequest& request) const;
  std::string DeleteValue(std::string_view key) const;

  storages::postgres::ClusterPtr pg_cluster_;
};

}  // namespace samples::pg
/// [Postgres service sample - component]

namespace samples::pg {

/// [Postgres service sample - component constructor]
KeyValue::KeyValue(const components::ComponentConfig& config,
                   const components::ComponentContext& context)
    : HttpHandlerBase(config, context),
      pg_cluster_(
          context.FindComponent<components::Postgres>("key-value-database")
              .GetCluster()) {
  constexpr auto kCreateTable = R"~(
      CREATE TABLE IF NOT EXISTS key_value_table (
        key VARCHAR PRIMARY KEY,
        value VARCHAR
      )
    )~";

  using storages::postgres::ClusterHostType;
  pg_cluster_->Execute(ClusterHostType::kMaster, kCreateTable);
}
/// [Postgres service sample - component constructor]

/// [Postgres service sample - HandleRequestThrow]
std::string KeyValue::HandleRequestThrow(
    const server::http::HttpRequest& request,
    server::request::RequestContext&) const {
  const auto& key = request.GetArg("key");
  if (key.empty()) {
    throw server::handlers::ClientError(
        server::handlers::ExternalBody{"No 'key' query argument"});
  }

  request.GetHttpResponse().SetContentType(http::content_type::kTextPlain);
  switch (request.GetMethod()) {
    case server::http::HttpMethod::kGet:
      return GetValue(key, request);
    case server::http::HttpMethod::kPost:
      return PostValue(key, request);
    case server::http::HttpMethod::kDelete:
      return DeleteValue(key);
    default:
      throw server::handlers::ClientError(server::handlers::ExternalBody{
          fmt::format("Unsupported method {}", request.GetMethod())});
  }
}
/// [Postgres service sample - HandleRequestThrow]

/// [Postgres service sample - GetValue]
const storages::postgres::Query kSelectValue{
    "SELECT value FROM key_value_table WHERE key=$1",
    storages::postgres::Query::Name{"sample_select_value"},
};

std::string KeyValue::GetValue(std::string_view key,
                               const server::http::HttpRequest& request) const {
  storages::postgres::ResultSet res = pg_cluster_->Execute(
      storages::postgres::ClusterHostType::kSlave, kSelectValue, key);
  if (res.IsEmpty()) {
    request.SetResponseStatus(server::http::HttpStatus::kNotFound);
    return {};
  }

  return res.AsSingleRow<std::string>();
}
/// [Postgres service sample - GetValue]

/// [Postgres service sample - PostValue]
const storages::postgres::Query kInsertValue{
    "INSERT INTO key_value_table (key, value) "
    "VALUES ($1, $2) "
    "ON CONFLICT DO NOTHING",
    storages::postgres::Query::Name{"sample_insert_value"},
};

std::string KeyValue::PostValue(
    std::string_view key, const server::http::HttpRequest& request) const {
  const auto& value = request.GetArg("value");

  storages::postgres::Transaction transaction =
      pg_cluster_->Begin("sample_transaction_insert_key_value",
                         storages::postgres::ClusterHostType::kMaster, {});

  auto res = transaction.Execute(kInsertValue, key, value);
  if (res.RowsAffected()) {
    transaction.Commit();
    request.SetResponseStatus(server::http::HttpStatus::kCreated);
    return std::string{value};
  }

  res = transaction.Execute(kSelectValue, key);
  transaction.Rollback();

  auto result = res.AsSingleRow<std::string>();
  if (result != value) {
    request.SetResponseStatus(server::http::HttpStatus::kConflict);
  }

  return res.AsSingleRow<std::string>();
}
/// [Postgres service sample - PostValue]

/// [Postgres service sample - DeleteValue]
std::string KeyValue::DeleteValue(std::string_view key) const {
  auto res =
      pg_cluster_->Execute(storages::postgres::ClusterHostType::kMaster,
                           "DELETE FROM key_value_table WHERE key=$1", key);
  return std::to_string(res.RowsAffected());
}
/// [Postgres service sample - DeleteValue]

}  // namespace samples::pg

/// [Postgres service sample - main]
int main(int argc, char* argv[]) {
  const auto component_list =
      components::MinimalServerComponentList()
          .Append<samples::pg::KeyValue>()
          .Append<components::Postgres>("key-value-database")
          .Append<components::TestsuiteSupport>()
          .Append<clients::dns::Component>();
  return utils::DaemonMain(argc, argv, component_list);
}
/// [Postgres service sample - main]
