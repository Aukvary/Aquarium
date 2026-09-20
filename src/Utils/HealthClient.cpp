#include "./Handlers.hpp"
#include "userver/ugrpc/client/call_options.hpp"

#include <aquarium.pb.h>
#include <aquarium_client.usrv.pb.hpp>
#include <utility>

namespace Aquarium::Handlers {
userver::ugrpc::client::CallOptions HealthClient::MakeCallOptions() {
    userver::ugrpc::client::CallOptions call_options;
    call_options.SetTimeout(std::chrono::seconds{5});
    return call_options;
}

HealthClient::HealthClient(aquarium::api::AquariumClient& client)
    : _client{client} {}

aquarium::api::HealthCheckResponse HealthClient::CheckHealth() const {
    aquarium::api::HealthCheckRequest request;

    auto call_options = MakeCallOptions();

    return _client.Health(request, std::move(call_options));
}
} // namespace Aquarium::Handlers
