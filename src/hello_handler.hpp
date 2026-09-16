#pragma once

#include <string_view>
#include <userver/utest/using_namespace_userver.hpp>

#include <userver/server/handlers/http_handler_base.hpp>

namespace aquarium {

class HelloHandler final : public server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "handler-hello";

    HelloHandler(const components::ComponentConfig& config, const components::ComponentContext& context);

    std::string HandleRequestThrow(const server::http::HttpRequest& request, server::request::RequestContext&)
        const override;
};

}  // namespace aquarium
