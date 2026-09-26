#include "./Users.hpp"

#include <string>
#include <string_view>
#include <userver/ugrpc/server/exceptions.hpp>

namespace Aquarium::Users {

void AuthServiceMiddleware::OnCallStart(
    userver::ugrpc::server::MiddlewareCallContext& ctx
) const {
    const auto& metaData = ctx.GetServerContext().client_metadata();

    auto it = metaData.find("authorization");
    if (it == metaData.end()) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::UNAUTHENTICATED,
                "Missing authorization metadata"
            )
        );
    }

    std::string_view authHeader(it->second.data(), it->second.size());
    if (!authHeader.starts_with("Bearer ")) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::UNAUTHENTICATED,
                "Invalid authorization format"
            )
        );
    }

    std::string token(authHeader.substr(7));
    auto snapShot = _authCache.Get();

    if (!snapShot->contains(token)) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::UNAUTHENTICATED, "Invalid or expired token"
            )
        );
    }
}

} // namespace Aquarium::Users
