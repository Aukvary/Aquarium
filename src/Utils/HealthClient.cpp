#include "userver/ugrpc/client/call_options.hpp"
#include "Utils.hpp"

#include <chrono>
#include <utility>
#include <UtilsService.pb.h>

namespace Aquarium::Handlers {
userver::ugrpc::client::CallOptions UtilsClient::MakeCallOptions() {
    userver::ugrpc::client::CallOptions call_options;
    call_options.SetTimeout(std::chrono::seconds{5});
    return call_options;
}

UtilsClient::UtilsClient(const Client& client) : _client{client} {}

UtilsClient::Response UtilsClient::CheckHealth() const {
    ::aquarium::api::HealthRequest request;

    auto call_options = MakeCallOptions();

    return _client.Health(request, std::move(call_options));
}
} // namespace Aquarium::Handlers
