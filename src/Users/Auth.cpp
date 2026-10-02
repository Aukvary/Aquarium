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
    std::string_view callName = context.GetCallName();
    const auto& metadata = context.GetServerContext().client_metadata();

    if (callName.ends_with("AddUser")) {
        auto it = metadata.find("invite-key");
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
        if (!authHeader.starts_with("Invite ")) {
            context.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED,
                    "Invalid authorization format"
                }
            );
            return;
        }

        std::string token(authHeader.substr(7));

        if (token != _inviteKey) {
            context.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED,
                    "Invalid or expired token"
                }
            );
            return;
        }

        return;
    }

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
