#include "./Users.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <userver/ugrpc/server/exceptions.hpp>

namespace Aquarium::Users {

bool IsLoopbackPeer(std::string_view peer) noexcept {
    if (peer.empty()) {
        return false;
    }

    if (peer.starts_with("unix:") || peer.starts_with("unix-abstract:")) {
        return true;
    }

    if (peer.starts_with("inproc")) {
        return true;
    }

    if (peer.starts_with("ipv4:")) {
        peer.remove_prefix(5);
    } else if (peer.starts_with("ipv6:")) {
        peer.remove_prefix(5);
    }

    if (peer.starts_with("127.")) {
        return true;
    }

    if (peer.starts_with("[::1]") || peer.starts_with("%5B::1%5D") ||
        peer.starts_with("%5b::1%5d") || peer.starts_with("::1")) {
        return true;
    }

    if (peer.starts_with("[::ffff:127.") ||
        peer.starts_with("%5B::ffff:127.") ||
        peer.starts_with("%5b::ffff:127.") ||
        peer.starts_with("::ffff:127.")) {
        return true;
    }

    return false;
}

bool ConstantTimeCompare(
    std::string_view str1,
    std::string_view str2
) noexcept {
    if (str1.size() == str2.size()) {
        return userver::crypto::algorithm::AreStringsEqualConstTime(
            str1, str2
        );
    }
    (void)userver::crypto::algorithm::AreStringsEqualConstTime(
        str2, str2
    );
    return false;
}

namespace {

void ValidateSingleKey(
    std::string_view key,
    std::string_view keyName,
    bool isTestMode
) {
    if (key.empty()) {
        throw std::invalid_argument(
            std::string(keyName) + " must not be empty"
        );
    }

    if (isTestMode) {
        return;
    }

    constexpr std::size_t kMinKeyLength = 32;
    if (key.length() < kMinKeyLength) {
        throw std::invalid_argument(
            std::string(keyName) + " is too short (" +
            std::to_string(key.length()) +
            " chars). Minimum length is 32 characters in production mode."
        );
    }

    static const std::unordered_set<std::string_view> kBannedWeakKeys = {
        "zxc",
        "secret_invite_key_value",
        "secret",
        "password",
        "admin",
        "12345678",
        "default",
        "internal_key",
        "invite_key",
        "test"
    };

    if (kBannedWeakKeys.contains(key)) {
        throw std::invalid_argument(
            std::string(keyName) + " uses insecure default/weak secret: " +
            std::string(key)
        );
    }

    std::unordered_set<char> uniqueChars(key.begin(), key.end());
    if (uniqueChars.size() < 4) {
        throw std::invalid_argument(
            std::string(keyName) +
            " does not meet minimum entropy requirements (too few unique characters)."
        );
    }
}

} // namespace

void ValidateStartupKeys(
    std::string_view internalKey,
    std::string_view inviteKey,
    bool isTestMode
) {
    ValidateSingleKey(internalKey, "internal-key", isTestMode);
    ValidateSingleKey(inviteKey, "invite-key", isTestMode);
}

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

    if (!ConstantTimeCompare(token, _inviteKey)) {
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
    if (!_internalKey.empty() && ConstantTimeCompare(token, _internalKey)) {
        const std::string peer = ctx.GetServerContext().peer();
        const bool isLocal =
            _peerValidator ? _peerValidator(peer) : IsLoopbackPeer(peer);
        if (!isLocal) {
            ctx.SetError(
                ::grpc::Status{
                    ::grpc::StatusCode::PERMISSION_DENIED,
                    "Internal service access is restricted to loopback/local callers"
                }
            );
            return;
        }

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
