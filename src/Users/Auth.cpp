#include "./Users.hpp"

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <string>
#include <string_view>
#include <userver/ugrpc/server/exceptions.hpp>

namespace Aquarium::Users {

void AuthServiceMiddleware::OnCallStart(
    userver::ugrpc::server::MiddlewareCallContext& context
) const {
    const auto& metadata = context.GetServerContext().client_metadata();

    auto it = metadata.find("authorization");
    if (it == metadata.cend()) {
        context.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Missing authorization metadata"
            }
        );
        return;
    }

    std::string_view authHeader(it->second.data(), it->second.size());
    if (!authHeader.starts_with("Bearer ")) {
        context.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Invalid authorization format"
            }
        );
        return;
    }

    std::string token(authHeader.substr(7));

    auto snapShot = _authCache.Get();

    if (!snapShot->contains(token)) {
        context.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED, "Invalid or expired token"
            }
        );
        return;
    }
}

} // namespace Aquarium::Users
