#include <hello_handler.hpp>

#include <userver/components/component.hpp>
#include <userver/utest/using_namespace_userver.hpp>

namespace aquarium {

HelloHandler::HelloHandler(const components::ComponentConfig& config, const components::ComponentContext& context)
    : server::handlers::HttpHandlerBase(config, context)
{}

std::string HelloHandler::HandleRequestThrow(
    const server::http::HttpRequest& request,
    server::request::RequestContext&
) const {
    const auto& name = request.GetArg("name");
    if (name.empty()) {
        return "Hello world";
    }
    return "Hello, " + name + "!";
}

}  // namespace aquarium
