#pragma once

#pragma once

#include <string_view>
#include <userver/components/component_base.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/ugrpc/client/fwd.hpp>
#include <userver/ugrpc/client/simple_client_component.hpp>
#include <userver/ugrpc/impl/static_service_metadata.hpp>
#include <userver/utest/using_namespace_userver.hpp>

namespace Aquarium::Handlers {

inline const userver::dynamic_config::Key<userver::ugrpc::client::ClientQos>
    kHealthQos{
        "health-client-qos",
        userver::dynamic_config::DefaultAsJsonString{
            R"({"methods": {"__default__": {"timeout-ms": 1000, "attempts": 1}}})"
        }
    };

class UtilsClient final {
private:
    using Client = aquarium::api::;
    using Request = aquarium::api::HealthRequest;
    static userver::ugrpc::client::CallOptions MakeCallOptions();
    Client& _client;

public:
    explicit UtilsClient(Client& client);

    [[nodiscard]]
    Request CheckHealth() const;
};

class HealthClientComponent final
    : public userver::ugrpc::client::SimpleClientComponent<UtilsClient> {
private:
    using Client = aquarium::api::UtilsClient;
    using Request = aquarium::api::HealthRequest;
    using Base = userver::ugrpc::client::SimpleClientComponent<UtilsClient>;

    UtilsClient _clientWrapper;

public:
    static constexpr std::string_view kName = "health-client";

    HealthClientComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    )
        : Base(cfg, ctx, kHealthQos), _clientWrapper(GetClient()) {}

    using Base::GetClient;

    UtilsClient& GetClientWrapper() noexcept {
        return _clientWrapper;
    }
};

class HealthService final
    : public userver::ugrpc::client::SimpleClientComponent<
          aquarium::api::UtilsClient> {
private:
    const std::string _prefix;

public:
    explicit HealthService(std::string prefix);

    grpc::health::v1::HealthCheckResponse Health(
        CallContext& context, aquarium::api::HealthCheckRequest&& request
    ) override;
};

class HealthServiceComponent final
    : public userver::ugrpc::server::ServiceComponentBase {
private:
    static constexpr std::string_view health_prefix = "health-prefix";
    HealthService _service;

public:
    static constexpr std::string_view kName = "health-service";

    HealthServiceComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    static userver::yaml_config::Schema GetStaticConfigSchema();
};
} // namespace Aquarium::Handlers
