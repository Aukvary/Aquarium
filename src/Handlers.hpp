#pragma once

#include "userver/ugrpc/client/call_options.hpp"

#include <aquarium_client.usrv.pb.hpp>
#include <string>
#include <string_view>
#include <userver/components/component_base.hpp>
#include <userver/components/component_fwd.hpp>
#include <userver/ugrpc/client/fwd.hpp>
#include <userver/ugrpc/client/simple_client_component.hpp>
#include <userver/ugrpc/impl/static_service_metadata.hpp>
#include <vector>

namespace Aquarium::Handlers {
class CheckHealthClient final {
private:
    static userver::ugrpc::client::CallOptions MakeCallOptions();
    aquarium::api::AquariumClient& client;

public:
    explicit CheckHealthClient(aquarium::api::AquariumClient& client);
    std::string CheckHealthRequestStream(
        const std::vector<std::string_view>& requests) const;
    std::vector<std::string_view>
    Stream(const std::vector<std::string_view>& requests) const;
};
} // namespace Aquarium::Handlers
