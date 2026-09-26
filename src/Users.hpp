#pragma once

#include "userver/components/component_config.hpp"
#include "userver/storages/postgres/io/chrono.hpp"
#include "userver/ugrpc/client/simple_client_component.hpp"
#include "userver/ugrpc/server/middlewares/base.hpp"
#include "userver/ugrpc/server/middlewares/fwd.hpp"
#include "userver/yaml_config/yaml_config.hpp"

#include <cstdint>
#include <google/protobuf/stubs/port.h>
#include <memory>
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
#include <userver/ugrpc/server/service_component_base.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <vector>

namespace Aquarium::Users {
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
    std::string _inviteKey;

public:
    static constexpr std::string_view kName = "user-store-service";

    UserStoreService(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    static userver::yaml_config::Schema GetStaticConfigSchema() {
        return userver::yaml_config::MergeSchemas<Base>(R"(
type: object
description: UserStore component
additionalProperties: false
properties:
    invite_key:
        type: string
        description: Secret invite key
)");
    }

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

struct UserDbInfo {
    std::int64_t id;
    std::string key;
    std::vector<std::string> scopes;
    std::string name;
};

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
        "SELECT id, key, scopes, name FROM users.user_schema.tokens";
    static constexpr const char* kUpdatedField = "update";
};

using AuthCache = userver::components::PostgreCache<AuthCachePolicy>;

class AuthServiceMiddleware final
    : public userver::ugrpc::server::MiddlewareBase {
private:
    const AuthCache& _authCache;

public:
    explicit AuthServiceMiddleware(const AuthCache& authCache)
        : _authCache(authCache) {}

    void OnCallStart(
        userver::ugrpc::server::MiddlewareCallContext& ctx
    ) const override;
};

class AuthServiceMiddlewareComponent
    : public userver::ugrpc::server::MiddlewareFactoryComponentBase {

    using MiddlewareBase = userver::ugrpc::server::MiddlewareBase;
    using Base = userver::ugrpc::server::MiddlewareFactoryComponentBase;

private:
    const AuthCache& _authCache;

public:
    static constexpr std::string_view kName = "auth-service-middleware";

    AuthServiceMiddlewareComponent(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    )
        : Base{cfg, ctx}, _authCache(ctx.FindComponent<AuthCache>()) {}

    [[nodiscard]]
    std::shared_ptr<const MiddlewareBase> CreateMiddleware(
        const userver::ugrpc::server::ServiceInfo&,
        const userver::yaml_config::YamlConfig&
    ) const override {
        return std::make_shared<AuthServiceMiddleware>(_authCache);
    }
};

} // namespace Aquarium::Users
