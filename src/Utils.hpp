#pragma once

#include <string_view>
#include <userver/components/component_base.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/ugrpc/client/fwd.hpp>
#include <userver/ugrpc/client/simple_client_component.hpp>
#include <userver/ugrpc/impl/static_service_metadata.hpp>
#include <userver/ugrpc/server/call_context.hpp>
#include <userver/ugrpc/server/service_component_base.hpp>
#include <UtilsService.grpc.pb.h>
#include <UtilsService.pb.h>
#include <UtilsService_client.usrv.pb.hpp>
#include <UtilsService_service.usrv.pb.hpp>

namespace Aquarium::Handlers {

inline const userver::dynamic_config::Key<userver::ugrpc::client::ClientQos>
    kHealthQos{
        "health-client-qos",
        userver::dynamic_config::DefaultAsJsonString{
            R"({"methods": {"__default__": {"timeout-ms": 1000, "attempts":
            1}}})"
        }
    };

class UtilsClient final {
private:
    using Client = aquarium::api::UtilsServiceClient;
    using Response = aquarium::api::HealthResponse;
    const Client& _client;

    static userver::ugrpc::client::CallOptions MakeCallOptions();

public:
    explicit UtilsClient(const Client& client);

    [[nodiscard]]
    Response CheckHealth() const;
};

class UtilsClientComponent final
    : public userver::ugrpc::client::SimpleClientComponent<
          aquarium::api::UtilsServiceClient> {
private:
    using Client = aquarium::api::UtilsServiceClient;
    using Request = aquarium::api::HealthRequest;
    using Base = userver::ugrpc::client::SimpleClientComponent<
        aquarium::api::UtilsServiceClient>;

    UtilsClient _clientWrapper;

public:
    static constexpr std::string_view kName = "health-client";

    UtilsClientComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    )
        : Base(cfg, ctx, kHealthQos), _clientWrapper(GetClient()) {}

    using Base::GetClient;

    UtilsClient& GetClientWrapper() noexcept {
        return _clientWrapper;
    }
};

class UtilsService final : public aquarium::api::UtilsServiceBase {
private:
    using HealthRequest = aquarium::api::HealthRequest;
    using HealthResponse = aquarium::api::HealthResponse;
    using CallContext = userver::ugrpc::server::CallContext;
    const std::string _prefix;

public:
    explicit UtilsService(std::string prefix);

    HealthResult Health(CallContext& context, HealthRequest&& request) override;
};

class UtilsServiceComponent final
    : public userver::ugrpc::server::ServiceComponentBase {
private:
    static constexpr std::string_view health_prefix = "health-prefix";
    UtilsService _service;

public:
    static constexpr std::string_view kName = "health-service";

    UtilsServiceComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    static userver::yaml_config::Schema GetStaticConfigSchema();
};
} // namespace Aquarium::Handlers
