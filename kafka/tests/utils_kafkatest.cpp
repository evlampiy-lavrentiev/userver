#include "utils_kafkatest.hpp"

#include <algorithm>
#include <tuple>

#include <fmt/format.h>

#include <userver/engine/subprocess/environment_variables.hpp>
#include <userver/engine/wait_all_checked.hpp>
#include <userver/utils/lazy_prvalue.hpp>

USERVER_NAMESPACE_BEGIN

namespace {

constexpr const char* kTestsuiteKafkaServerHost{"TESTSUITE_KAFKA_SERVER_HOST"};
constexpr const char* kDefaultKafkaServerHost{"localhost"};
constexpr const char* kTestsuiteKafkaServerPort{"TESTSUITE_KAFKA_SERVER_PORT"};
constexpr const char* kDefaultKafkaServerPort{"9099"};
constexpr const char* kRecipeKafkaBrokersList{"KAFKA_RECIPE_BROKER_LIST"};

std::string FetchBrokerList() {
  const auto env = engine::subprocess::GetCurrentEnvironmentVariablesPtr();

  if (const auto* brokers_list =
          env->GetValueOptional(kRecipeKafkaBrokersList)) {
    return *brokers_list;
  }

  std::string server_host{kDefaultKafkaServerHost};
  std::string server_port{kDefaultKafkaServerPort};

  const auto* host = env->GetValueOptional(kTestsuiteKafkaServerHost);
  if (host) {
    server_port = *host;
  }
  const auto* port = env->GetValueOptional(kTestsuiteKafkaServerPort);
  if (port) {
    server_port = *port;
  }
  return fmt::format("{}:{}", server_host, server_port);
}

kafka::impl::Secret MakeSecrets(std::string_view bootstrap_servers) {
  kafka::impl::Secret secrets{};
  secrets.brokers = bootstrap_servers;

  return secrets;
}

}  // namespace

bool operator==(const Message& lhs, const Message& rhs) {
  return std::tie(lhs.topic, lhs.key, lhs.payload, lhs.partition) ==
         std::tie(rhs.topic, rhs.key, rhs.payload, rhs.partition);
}

KafkaCluster::KafkaCluster() : bootstrap_servers_(FetchBrokerList()) {}

std::string KafkaCluster::GenerateTopic() {
  return fmt::format("tt-{}", topics_count_.fetch_add(1));
}

std::vector<std::string> KafkaCluster::GenerateTopics(std::size_t count) {
  std::vector<std::string> topics{count};
  std::generate_n(topics.begin(), count, [this] { return GenerateTopic(); });

  return topics;
}

kafka::impl::Configuration KafkaCluster::MakeProducerConfiguration(
    const std::string& name, kafka::impl::ProducerConfiguration configuration,
    kafka::impl::Secret secrets) {
  return kafka::impl::Configuration{name, configuration,
                                    AddBootstrapServers(secrets)};
}

kafka::impl::Configuration KafkaCluster::MakeConsumerConfiguration(
    const std::string& name, kafka::impl::ConsumerConfiguration configuration,
    kafka::impl::Secret secrets) {
  if (configuration.group_id.empty()) {
    configuration.group_id = "test-group";
  }
  return kafka::impl::Configuration{name, configuration,
                                    AddBootstrapServers(secrets)};
}

kafka::Producer KafkaCluster::MakeProducer(
    const std::string& name, kafka::impl::ProducerConfiguration configuration) {
  return kafka::Producer{name, engine::current_task::GetTaskProcessor(),
                         configuration, MakeSecrets(bootstrap_servers_)};
};

std::deque<kafka::Producer> KafkaCluster::MakeProducers(
    std::size_t count, std::function<std::string(std::size_t)> nameGenerator,
    kafka::impl::ProducerConfiguration configuration) {
  std::deque<kafka::Producer> producers;
  for (std::size_t i{0}; i < count; ++i) {
    producers.emplace_back(utils::LazyPrvalue(
        [&] { return MakeProducer(nameGenerator(i), configuration); }));
  }

  return producers;
}

void KafkaCluster::SendMessages(utils::span<const Message> messages) {
  kafka::Producer producer = MakeProducer("kafka-producer");

  std::vector<engine::TaskWithResult<void>> results;
  results.reserve(messages.size());
  for (const auto& message : messages) {
    results.emplace_back(producer.SendAsync(
        message.topic, message.key, message.payload, message.partition));
  }

  engine::WaitAllChecked(results);
}

kafka::impl::Consumer KafkaCluster::MakeConsumer(
    const std::string& name, const std::vector<std::string>& topics,
    kafka::impl::ConsumerConfiguration configuration,
    kafka::impl::ConsumerExecutionParams params) {
  if (configuration.group_id.empty()) {
    configuration.group_id = "test-group";
  }
  return kafka::impl::Consumer{name,
                               topics,
                               engine::current_task::GetTaskProcessor(),
                               engine::current_task::GetTaskProcessor(),
                               configuration,
                               MakeSecrets(bootstrap_servers_),
                               std::move(params)};
};

kafka::impl::Secret KafkaCluster::AddBootstrapServers(
    kafka::impl::Secret secrets) const {
  secrets.brokers = bootstrap_servers_;

  return secrets;
}

USERVER_NAMESPACE_END
