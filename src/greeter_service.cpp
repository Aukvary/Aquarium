#include <greeter_service.hpp>

#include <utility>

#include <fmt/format.h>

#include <userver/components/component.hpp>
#include <userver/utest/using_namespace_userver.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <userver/ugrpc/server/service_component_base.hpp>

namespace aquarium {

GreeterService::GreeterService(std::string prefix)
    : prefix_(std::move(prefix))
{}

GreeterService::SayHelloResult GreeterService::SayHello(CallContext& /*context*/, api::GreetingRequest&& request) {
    api::GreetingResponse response;
    response.set_greeting(fmt::format("{}, {}!", prefix_, request.name()));
    return response;
}

GreeterServiceComponent::GreeterServiceComponent(
    const components::ComponentConfig& config,
    const components::ComponentContext& context
)
    : ugrpc::server::ServiceComponentBase(config, context),
      service_(config["greeting-prefix"].As<std::string>())
{
    RegisterService(service_);
}

yaml_config::Schema GreeterServiceComponent::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<ugrpc::server::ServiceComponentBase>(R"(
type: object
description: gRPC greeter service component
additionalProperties: false
properties:
    greeting-prefix:
        type: string
        description: greeting prefix
)");
}

}  // namespace aquarium
