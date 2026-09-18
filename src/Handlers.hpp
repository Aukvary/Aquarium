#pragma once

#include <aquarium.grpc.pb.h>
#include <userver/components/component_base.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/ugrpc/client/fwd.hpp>
#include <userver/ugrpc/client/simple_client_component.hpp>
#include <userver/ugrpc/impl/static_service_metadata.hpp>

namespace Aquarium::Handlers {
class CheckHealthClient final {
  public:
    explicit CheckHealthClient(aquarium::api::HealthCheckRequest);
};
} // namespace Aquarium::Handlers
