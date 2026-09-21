#include "userver/components/component_config.hpp"
#include "userver/ugrpc/server/service_component_base.hpp"
#include "Utils.hpp"

#include <string>
#include <userver/yaml_config/merge_schemas.hpp>
#include <utility>
#include <UtilsService.pb.h>
#include <UtilsService_service.usrv.pb.hpp>

namespace Aquarium::Handlers {

UtilsService ::UtilsService(std::string prefix) : _prefix(std::move(prefix)) {}

UtilsService::HealthResult UtilsService::Health(
    CallContext& /*context*/, aquarium::api::HealthRequest&& /*request*/
) {
    aquarium::api::HealthResponse responce;

    responce.set_status(aquarium::api::HealthResponse::SERVING);
    responce.set_db_initialized(false);
    responce.set_utilities_initialized(false);

    return responce;
}

UtilsServiceComponent::UtilsServiceComponent(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : userver::ugrpc::server::ServiceComponentBase{cfg, ctx},
      _service(cfg["health-prefix"].As<std::string>()) {
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
)");
}

} // namespace Aquarium::Handlers
