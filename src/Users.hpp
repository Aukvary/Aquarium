#pragma once

#include "userver/components/component_context.hpp"
#include "userver/storages/postgres/postgres_fwd.hpp"

#include <cstdint>
#include <google/protobuf/stubs/port.h>
#include <string>
#include <UserStoreService.grpc.pb.h>
#include <UserStoreService.pb.h>
#include <UserStoreService_client.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/ugrpc/server/service_component_base.hpp>

namespace Aquarium::Users {
struct User {
    using IdType = int64_t;

    IdType id;
    std::string name;
    std::string key;
};

class UserStore final : aquarium::api::UserStoreBase::Component {
private:
    using Base = aquarium::api::UserStoreBase::Component;
    using GetUserRequest = aquarium::api::GetUserRequest;
    using GetAllUsersRequest = aquarium::api::GetAllUsersRequest;
    using AddUserRequest = aquarium::api::AddUserRequest;
    using UpdateUserRequest = aquarium::api::UpdateUserRequest;
    using DeleteUserRequest = aquarium::api::DeleteUserRequest;

    userver::storages::postgres::ClusterPtr _pgCluster;
    std::string _inviteKey;

public:
    static constexpr std::string_view kName = "user-store-service";

    UserStore(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    GetAllResult
    GetAll(CallContext& ctx, GetAllUsersRequest&& request) override;

    GetUserResult GetUser(CallContext& ctx, GetUserRequest&& request) override;

    AddUserResult AddUser(CallContext& ctx, AddUserRequest&& request) override;

    UpdateUserResult
    UpdateUser(CallContext& ctx, UpdateUserRequest&& request) override;

    DeleteUserResult
    DeleteUser(CallContext& ctx, DeleteUserRequest&& request) override;
};
} // namespace Aquarium::Users
