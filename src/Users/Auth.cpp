#include "./Users.hpp"

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <cstdint>
#include <curl/curl.h>
#include <string>
#include <string_view>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/utils/assert.hpp>

namespace Aquarium::Users {
void AuthServiceMiddleware::OnCallStart(
    userver::ugrpc::server::MiddlewareCallContext& ctx
) const {
    std::string_view callName = ctx.GetCallName();
    const auto& metadata = ctx.GetServerContext().client_metadata();

    if (!CheckAddUser(ctx, callName, metadata))
        return;

    if (CheckUserAuth(ctx, metadata))
        return;
}

bool AuthServiceMiddleware::CheckAddUser(
    userver::ugrpc::server::MiddlewareCallContext& ctx,
    std::string_view callName,
    const auto& metadata
) const {
    if (callName.ends_with("AddUser")) {
        auto it = metadata.find("invite-key");
        if (it == metadata.cend()) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED,
                    "Missing authorization metadata"
                }
            );
            return false;
        }

        std::string_view authHeader(it->second.data(), it->second.size());
        if (!authHeader.starts_with("Invite ")) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED,
                    "Invalid authorization format"
                }
            );
            return false;
        }

        std::string token(authHeader.substr(7));

        if (token != _inviteKey) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::UNAUTHENTICATED,
                    "Invalid or expired token"
                }
            );
            return false;
        }

        return false;
    }

    return true;
}
bool AuthServiceMiddleware::CheckUserAuth(
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
        return false;
    }

    std::string_view authHeader(it->second.data(), it->second.size());
    if (!authHeader.starts_with("Bearer ")) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED,
                "Invalid authorization format"
            }
        );
        return false;
    }

    std::string token(authHeader.substr(7));
    if (token == _internalKey)
        return false;

    auto snapShot = _authCache.Get();

    const AuthCachePolicy::CacheContainer& users = *snapShot.Get();

    auto user_it = users.find(token);
    if (user_it == users.end()) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED, "Invalid or expired token"
            }
        );
        return false;
    }

    const UserDbInfo& user = user_it->second;

    if (!user.active) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::UNAUTHENTICATED, "User is not active"
            }
        );
        return false;
    }

    ctx.GetStorageContext().Emplace(kAuthUser, user);

    return false;
}

void AuthServiceMiddleware::PostRecvMessage(
    userver::ugrpc::server::MiddlewareCallContext& ctx,
    google::protobuf::Message& request
) const {
    const auto* user = ctx.GetStorageContext().GetOptional(kAuthUser);
    if (!user) {
        return;
    }

    const google::protobuf::FieldDescriptor* id_field =
        request.GetDescriptor()->FindFieldByName("id");
    if (!id_field) {
        return;
    }

    UINVARIANT(
        id_field->type() == google::protobuf::FieldDescriptor::TYPE_INT64,
        "field 'id' must be int64"
    );

    const std::int64_t id =
        request.GetReflection()->GetInt64(request, id_field);

    if (!CheckSelfOrAdmin(ctx.GetCallName(), *user, id)) {
        ctx.SetError(
            ::grpc::Status{
                ::grpc::StatusCode::PERMISSION_DENIED,
                "Only the user itself or an admin can call this method"
            }
        );
    }
}

bool AuthServiceMiddleware::CheckSelfOrAdmin(
    std::string_view callName,
    const UserDbInfo& user,
    std::int64_t id
) const {
    if (!callName.ends_with("UpdateUser") && !callName.ends_with("DeleteUser"))
        return true;

    return user.admin || user.id == id;
}
} // namespace Aquarium::Users
