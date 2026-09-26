#pragma once

#include "userver/ugrpc/client/simple_client_component.hpp"

#include <cstdint>
#include <google/protobuf/stubs/port.h>
#include <string>
#include <string_view>
#include <UserStoreService.grpc.pb.h>
#include <UserStoreService.pb.h>
#include <UserStoreService_client.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <userver/components/component_context.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>
#include <userver/ugrpc/server/service_component_base.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

namespace Aquarium::Users {

struct User {
    using IdType = int64_t;

    IdType id;
    std::string name;
    std::string key;
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

} // namespace Aquarium::Users
