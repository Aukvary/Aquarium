#pragma once
#include "userver/components/component_config.hpp"
#include "userver/storages/postgres/io/chrono.hpp"
#include "userver/ugrpc/client/simple_client_component.hpp"
#include "userver/ugrpc/server/middlewares/base.hpp"
#include "userver/ugrpc/server/middlewares/fwd.hpp"
#include "userver/ugrpc/server/storage_context.hpp"
#include "userver/utils/any_storage.hpp"
#include "userver/yaml_config/yaml_config.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_hash.hpp>
#include <cstdint>
#include <functional>
#include <google/protobuf/stubs/port.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <UserStoreService.grpc.pb.h>
#include <UserStoreService.pb.h>
#include <UserStoreService_client.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <userver/cache/base_postgres_cache.hpp>
#include <userver/components/component_context.hpp>
#include <userver/crypto/algorithm.hpp>
#include <userver/server/auth/user_auth_info.hpp>
#include <userver/server/handlers/auth/auth_checker_base.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/io/array_types.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/ugrpc/server/service_component_base.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <vector>

namespace Aquarium::Users {
struct UserDbInfo {
    std::int64_t id;
    std::string key;
    std::string name;
    std::vector<std::string> scopes;
    bool active;
    bool admin;
};

const UserDbInfo* FindUserById(
    const std::unordered_map<
        std::string,
        UserDbInfo,
        std::hash<std::string>,
        userver::crypto::algorithm::StringsEqualConstTimeComparator>& users,
    std::int64_t id
);

bool IsLoopbackPeer(std::string_view peer) noexcept;

bool ConstantTimeCompare(
    std::string_view str1,
    std::string_view str2
) noexcept;

void ValidateStartupKeys(
    std::string_view internalKey,
    std::string_view inviteKey,
    bool isTestMode
);

constexpr std::size_t kMaxUserNameLength = 100;

bool IsBlank(std::string_view str) noexcept;
void ValidateUserName(std::string_view name);

userver::ugrpc::server::ErrorWithStatus HandleDatabaseException(
    const std::exception& ex,
    std::string_view operation,
    std::string_view contextInfo = {}
);

inline const userver::utils::
    AnyStorageDataTag<userver::ugrpc::server::StorageContext, UserDbInfo>
        kAuthUser{};

struct AuthCachePolicy {
    static constexpr std::string_view kName = "auth-pg-cache";

    using ValueType = UserDbInfo;
    using UpdatedFieldType = userver::storages::postgres::TimePointTz;
    using CacheContainer = std::unordered_map<
        std::string,
        UserDbInfo,
        std::hash<std::string>,
        userver::crypto::algorithm::StringsEqualConstTimeComparator>;

    static constexpr auto kKeyMember = &UserDbInfo::key;
    static constexpr const char* kQuery =
        "SELECT id, key, name, scopes, active, admin FROM user_schema.tokens";
    static constexpr const char* kUpdatedField = "updated";
};

using AuthCache = userver::components::PostgreCache<AuthCachePolicy>;

class UserStoreService final
    : public aquarium::api::UserStoreServiceBase::Component {

    using Base = aquarium::api::UserStoreServiceBase::Component;
    using GetUserRequest = aquarium::api::GetUserRequest;
    using GetAllUsersRequest = aquarium::api::GetAllUsersRequest;
    using AddUserRequest = aquarium::api::AddUserRequest;
    using UpdateUserRequest = aquarium::api::UpdateUserRequest;
    using DeleteUserRequest = aquarium::api::DeleteUserRequest;

private:
    userver::storages::postgres::ClusterPtr _pgCluster;
    AuthCache& _authCache;

public:
    static constexpr std::string_view kName = "user-store-service";

    UserStoreService(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    GetAllResult GetAll(
        CallContext& ctx,
        GetAllUsersRequest&& request
    ) override;

    GetUserResult GetUser(CallContext& ctx, GetUserRequest&& request) override;

    AddUserResult AddUser(CallContext& ctx, AddUserRequest&& request) override;

    UpdateUserResult UpdateUser(
        CallContext& ctx,
        UpdateUserRequest&& request
    ) override;

    DeleteUserResult DeleteUser(
        CallContext& ctx,
        DeleteUserRequest&& request
    ) override;
};

class UserStoreClientComponent final
    : public userver::ugrpc::client::SimpleClientComponent<
          aquarium::api::UserStoreServiceClient> {
public:
    using Base = userver::ugrpc::client::SimpleClientComponent<
        aquarium::api::UserStoreServiceClient>;

    static constexpr std::string_view kName = "user-store-client";

    UserStoreClientComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    )
        : Base{cfg, ctx} {};

    using Base::GetClient;
};

class AuthenticationMiddleware final
    : public userver::ugrpc::server::MiddlewareBase {
public:
    using UserLookup =
        std::function<std::optional<UserDbInfo>(std::string_view)>;
    using PeerValidator = std::function<bool(std::string_view)>;

private:
    const std::string _inviteKey;
    const AuthCache* _authCache{nullptr};
    const std::string _internalKey;
    UserLookup _userLookup{};
    PeerValidator _peerValidator{IsLoopbackPeer};

public:
    static inline const auto kDependency =
        userver::middlewares::MiddlewareDependencyBuilder()
            .InGroup<userver::middlewares::groups::Auth>();

    explicit AuthenticationMiddleware(
        const std::string_view inviteKey,
        const AuthCache& authCache,
        const std::string_view internalKey,
        PeerValidator peerValidator = IsLoopbackPeer
    )
        : _inviteKey(inviteKey), _authCache(&authCache),
          _internalKey(internalKey),
          _peerValidator(std::move(peerValidator)) {}

    explicit AuthenticationMiddleware(
        const std::string_view inviteKey,
        UserLookup userLookup,
        const std::string_view internalKey,
        PeerValidator peerValidator = IsLoopbackPeer
    )
        : _inviteKey(inviteKey), _authCache(nullptr),
          _internalKey(internalKey), _userLookup(std::move(userLookup)),
          _peerValidator(std::move(peerValidator)) {}

    void OnCallStart(
        userver::ugrpc::server::MiddlewareCallContext& ctx
    ) const override;

    void CheckAddUser(
        userver::ugrpc::server::MiddlewareCallContext& ctx,
        const auto& metadata
    ) const;
    void CheckUserAuth(
        userver::ugrpc::server::MiddlewareCallContext& ctx,
        const auto& metadata
    ) const;
};

using AuthServiceMiddleware = AuthenticationMiddleware;

class AuthenticationMiddlewareComponent
    : public userver::ugrpc::server::MiddlewareFactoryComponentBase {

    using MiddlewareBase = userver::ugrpc::server::MiddlewareBase;
    using Base = userver::ugrpc::server::MiddlewareFactoryComponentBase;

private:
    std::shared_ptr<const AuthenticationMiddleware> _middleware;

public:
    static constexpr std::string_view kName = "authentication-middleware";
    static userver::yaml_config::Schema GetStaticConfigSchema() {
        return userver::yaml_config::MergeSchemas<Base>(R"(
type: object
description: Authentication middleware component
additionalProperties: false
properties:
    invite-key:
        type: string
        description: Secret invite key
    internal-key:
        type: string
        description: Secret internal key
    test-mode:
        type: boolean
        description: Allow development/test fallback keys
        defaultDescription: false
)");
    }

    AuthenticationMiddlewareComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    )
        : Base{cfg, ctx} {
        const auto inviteKey = cfg["invite-key"].As<std::string>();
        const auto internalKey = cfg["internal-key"].As<std::string>();

        bool isTestMode = cfg["test-mode"].As<bool>(false);
        const char* envTestMode = std::getenv("AQUARIUM_TEST_MODE");
        if (envTestMode && (std::string_view(envTestMode) == "1" ||
                            std::string_view(envTestMode) == "true")) {
            isTestMode = true;
        }

        ValidateStartupKeys(internalKey, inviteKey, isTestMode);

        _middleware = std::make_shared<const AuthenticationMiddleware>(
            inviteKey,
            ctx.FindComponent<AuthCache>(),
            internalKey
        );
    }

    [[nodiscard]]
    std::shared_ptr<const MiddlewareBase> CreateMiddleware(
        const userver::ugrpc::server::ServiceInfo&,
        const userver::yaml_config::YamlConfig&
    ) const override {
        return _middleware;
    }
};

using AuthServiceMiddlewareComponent = AuthenticationMiddlewareComponent;

} // namespace Aquarium::Users
