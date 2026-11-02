#include "./Users.hpp"
#include "userver/cache/update_type.hpp"
#include "userver/storages/postgres/cluster_types.hpp"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstdint>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/config.h>
#include <grpcpp/support/status.h>
#include <string>
#include <string_view>
#include <tuple>
#include <UserStoreService.pb.h>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/postgres.hpp>
#include <userver/storages/postgres/query.hpp>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/utils/uuid4.hpp>
#include <utility>

namespace Aquarium::Users {
UserStoreService::UserStoreService(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : UserStoreService::Component{cfg, ctx},
      _pgCluster(ctx.FindComponent<userver::components::Postgres>("postgres-db")
                     .GetCluster()),
      _authCache(ctx.FindComponent<AuthCache>()) {}

UserStoreService::GetAllResult UserStoreService::GetAll(
    CallContext& /*ctx*/,
    GetAllUsersRequest&& /*request*/
) {
    try {
        auto snapshot = _authCache.Get();

        aquarium::api::GetAllUsersResponse response;

        for (const auto& [key, user_info] : *snapshot) {
            auto* user = response.add_users();
            user->set_id(user_info.id);
            user->set_name(user_info.name);
            user->set_key(user_info.key);
        }

        return response;
    } catch (const std::exception& ex) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(grpc::StatusCode::INTERNAL, ex.what())
        );
    }
}

UserStoreService::GetUserResult UserStoreService::GetUser(
    CallContext& /*ctx*/,
    GetUserRequest&& request
) {
    try {
        auto snapshot = _authCache.Get();

        const auto target_id = request.id();
        auto it = std::find_if(
            snapshot->begin(), snapshot->end(), [target_id](const auto& pair) {
                return pair.second.id == target_id;
            }
        );

        if (it == snapshot->end()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "Undefined user")
            );
        }

        const auto& user_info = it->second;

        aquarium::api::GetUserResponse response;
        auto* user = response.mutable_user();
        user->set_id(user_info.id);
        user->set_name(user_info.name);
        user->set_key(user_info.key);

        return response;

    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& e) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INTERNAL,
                std::string("Internal error: ") + e.what()
            )
        );
    }
}

UserStoreService::AddUserResult UserStoreService::AddUser(
    CallContext& /*ctx*/,
    AddUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "INSERT INTO user_schema.tokens (name) VALUES ($1) RETURNING id, key"
    };

    try {
        std::string name = std::move(*request.mutable_name());

        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery, name
        );

        auto [id, key] = result.AsSingleRow<std::tuple<int64_t, std::string>>(
            userver::storages::postgres::kRowTag
        );

        _authCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

        aquarium::api::AddUserResponse response;
        auto* user = response.mutable_user();

        user->set_id(id);
        user->set_name(std::move(name));
        user->set_key(std::move(key));

        return response;
    } catch (const std::exception& ex) {
        LOG_ERROR() << "AddUser error: " << ex.what();
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INTERNAL,
                std::string("DB/Internal error: ") + ex.what()
            )
        );
    }
}

UserStoreService::UpdateUserResult UserStoreService::UpdateUser(
    CallContext& /*ctx*/,
    UpdateUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "UPDATE user_schema.tokens SET name = $2 WHERE id = $1 RETURNING id, "
        "name, key"
    };

    try {
        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery,
            request.id(), request.newname()
        );

        if (result.IsEmpty()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "User not found")
            );
        }

        using UserRow = std::tuple<int64_t, std::string, std::string>;
        auto [id, name, key] =
            result.AsSingleRow<UserRow>(userver::storages::postgres::kRowTag);

        _authCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

        aquarium::api::UpdateUserResponse response;
        auto* user = response.mutable_user();

        user->set_id(id);
        user->set_name(std::move(name));
        user->set_key(std::move(key));

        return response;
    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& ex) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(grpc::StatusCode::INTERNAL, ex.what())
        );
    }
}

UserStoreService::DeleteUserResult UserStoreService::DeleteUser(
    CallContext& /*ctx*/,
    DeleteUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "DELETE FROM user_schema.tokens WHERE id = $1"
    };

    try {
        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery,
            static_cast<int64_t>(request.id())
        );

        bool success = result.RowsAffected() > 0;
        if (success) {
            _authCache.InvalidateAsync(
                userver::cache::UpdateType::kIncremental
            );
        }

        aquarium::api::DeleteUserResponse response;
        response.set_success(success);

        return response;
    } catch (const std::exception& ex) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(grpc::StatusCode::INTERNAL, ex.what())
        );
    }
}
}; // namespace Aquarium::Users
