#include "./Handlers.hpp"
#include "userver/components/component_config.hpp"
#include "userver/ugrpc/server/service_component_base.hpp"

#include <aquarium.pb.h>
#include <string>
#include <userver/yaml_config/merge_schemas.hpp>
#include <utility>

namespace Aquarium::Handlers {

HealthService::HealthService(std::string prefix) : _prefix(std::move(prefix)) {}

aquarium::api::AquariumBase::HealthResult HealthService::Health(
    CallContext& /*context*/, aquarium::api::HealthCheckRequest&& /*request*/
) {
    aquarium::api::HealthCheckResponse responce;

    responce.set_status(aquarium::api::HealthCheckResponse::SERVING);
    responce.set_db_initialized(false);
    responce.set_utilities_initialized(false);

    return responce;
}

HealthServiceComponent::HealthServiceComponent(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : userver::ugrpc::server::ServiceComponentBase{cfg, ctx},
      _service(cfg["health-prefix"].As<std::string>()) {
    RegisterService(_service);
}

userver::yaml_config::Schema HealthServiceComponent::GetStaticConfigSchema() {
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
