#pragma once

#include <string_view>
#include <userver/utest/using_namespace_userver.hpp>

#include <userver/components/component_fwd.hpp>
#include <userver/ugrpc/server/service_component_base.hpp>

#include <greeter_service.usrv.pb.hpp>

namespace aquarium {

class GreeterService final : public api::GreeterServiceBase {
public:
  explicit GreeterService(std::string prefix);

  SayHelloResult SayHello(CallContext &context,
                          api::GreetingRequest &&request) override;

private:
  const std::string prefix_;
};

class GreeterServiceComponent final
    : public ugrpc::server::ServiceComponentBase {
public:
  static constexpr std::string_view kName = "greeter-service";

  GreeterServiceComponent(const components::ComponentConfig &config,
                          const components::ComponentContext &context);

  static yaml_config::Schema GetStaticConfigSchema();

private:
  GreeterService service_;
};

} // namespace aquarium
