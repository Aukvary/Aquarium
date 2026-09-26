#pragma once

#include <string>
#include <string_view>
#include <UserStoreService_client.usrv.pb.hpp>
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

namespace Aquarium::Utils {
class UtilsService final : public aquarium::api::UtilsServiceBase {
private:
    using HealthRequest = aquarium::api::HealthRequest;
    using HealthResponse = aquarium::api::HealthResponse;
    using CallContext = userver::ugrpc::server::CallContext;
    using UserStoreClient = ::aquarium::api::UserStoreServiceClient;

    UserStoreClient& _userStore;

public:
    explicit UtilsService(UserStoreClient& userStore);

    HealthResult Health(CallContext& context, HealthRequest&& request) override;
};

class UtilsServiceComponent final
    : public userver::ugrpc::server::ServiceComponentBase {

private:
    UtilsService _service;

public:
    static constexpr std::string_view kName = "health-service";

    UtilsServiceComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    static userver::yaml_config::Schema GetStaticConfigSchema();
};
} // namespace Aquarium::Utils
