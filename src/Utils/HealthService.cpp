#include "./Users.hpp"
#include "./Utils.hpp"
#include "userver/components/component_config.hpp"
#include "userver/ugrpc/client/exceptions.hpp"
#include "userver/ugrpc/server/service_component_base.hpp"

#include <memory>
#include <string>
#include <UserStoreService.grpc.pb.h>
#include <UserStoreService.pb.h>
#include <UserStoreService_client.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <userver/ugrpc/client/call_options.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <utility>
#include <UtilsService.pb.h>

namespace Aquarium::Utils {

UtilsService::UtilsService(
    UserStoreClient& userStore,
    std::string_view internalKey
)
    : _userStore(userStore), _internalKey(internalKey) {}

UtilsService::HealthResult UtilsService::Health(
    CallContext& /*context*/,
    aquarium::api::HealthRequest&& /*request*/
) {
    aquarium::api::HealthResponse response;

    try {
        userver::ugrpc::client::CallOptions call_options;

        call_options.AddMetadata("authorization", "Bearer " + _internalKey);

        const auto users = _userStore.GetAll(
            aquarium::api::GetAllUsersRequest{}, std::move(call_options)
        );
        response.set_status(aquarium::api::HealthResponse::SERVING);
        response.set_db_initialized(
            fmt::format("db has been initialized: {} users", users.users_size())
        );
    } catch (const userver::ugrpc::client::BaseError& ex) {
        response.set_status(aquarium::api::HealthResponse::NOT_SERVING);
        response.set_db_initialized("db check failed");
    } catch (const std::exception& ex) {
        response.set_status(aquarium::api::HealthResponse::NOT_SERVING);
        response.set_db_initialized("unknown error");
    }

    response.set_utilities_initialized(false);
    return response;
}

UtilsServiceComponent::UtilsServiceComponent(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : userver::ugrpc::server::ServiceComponentBase{cfg, ctx},
      _service{
          ctx.FindComponent<Aquarium::Users::UserStoreClientComponent>()
              .GetClient(),
          cfg["internal-key"].As<std::string>()
      } {
    RegisterService(_service);
}

userver::yaml_config::Schema UtilsServiceComponent::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<
        userver::ugrpc::server::ServiceComponentBase>(R"(
type: object
description: Health service component
additionalProperties: false
properties:
    health-prefix:
        type: string
        description: prefix for health checks
    internal-key:
        type: string
        description: Secret invite key
)");
}

} // namespace Aquarium::Utils
