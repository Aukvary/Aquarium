#include "./Users.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <userver/ugrpc/server/exceptions.hpp>

namespace Aquarium::Users {

void AuthenticationMiddleware::OnCallStart(
    userver::ugrpc::server::MiddlewareCallContext& ctx
) const {
    std::string_view callName = ctx.GetCallName();
    const auto& metadata = ctx.GetServerContext().client_metadata();

    if (callName.ends_with("AddUser")) {
        CheckAddUser(ctx, metadata);
        return;
    }

    CheckUserAuth(ctx, metadata);
}

void AuthenticationMiddleware::CheckAddUser(
    userver::ugrpc::server::MiddlewareCallContext& ctx,
    const auto& metadata
) const {
    auto it = metadata.find("invite-key");
    if (it == metadata.cend()) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Missing authorization metadata"
            }
        );
        return;
    }

    std::string_view authHeader(it->second.data(), it->second.size());
    if (!authHeader.starts_with("Invite ")) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Invalid authorization format"
            }
        );
        return;
    }

    std::string token(authHeader.substr(7));

    if (token != _inviteKey) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Invalid or expired token"
            }
        );
        return;
    }
}

void AuthenticationMiddleware::CheckUserAuth(
    userver::ugrpc::server::MiddlewareCallContext& ctx,
    const auto& metadata
) const {
    auto it = metadata.find("authorization");
    if (it == metadata.cend()) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Missing authorization metadata"
            }
        );
        return;
    }

    std::string_view authHeader(it->second.data(), it->second.size());
    if (!authHeader.starts_with("Bearer ")) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Invalid authorization format"
            }
        );
        return;
    }

    std::string token(authHeader.substr(7));
    if (token == _internalKey) {
        UserDbInfo internalUser{
            .id = 0,
            .key = std::string(_internalKey),
            .name = "internal_service",
            .scopes = {"*"},
            .active = true,
            .admin = true
        };
        ctx.GetStorageContext().Emplace(kAuthUser, std::move(internalUser));
        return;
    }

    UserDbInfo user;
    if (_userLookup) {
        auto optUser = _userLookup(token);
        if (!optUser) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED, "Invalid or expired token"
                }
            );
            return;
        }
        user = std::move(*optUser);
    } else if (_authCache) {
        auto snapShot = _authCache->Get();
        const AuthCachePolicy::CacheContainer& users = *snapShot.Get();
        auto user_it = users.find(token);
        if (user_it == users.end()) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED, "Invalid or expired token"
                }
            );
            return;
        }
        user = user_it->second;
    } else {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::INTERNAL,
                "Auth cache not initialized"
            }
        );
        return;
    }

    if (!user.active) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "User is not active"
            }
        );
        return;
    }

    ctx.GetStorageContext().Emplace(kAuthUser, std::move(user));
}

} // namespace Aquarium::Users
