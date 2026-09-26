#include "./Users.hpp"
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
      _inviteKey(cfg["invite_key"].As<std::string>()) {}

UserStoreService::GetAllResult UserStoreService::GetAll(
    CallContext& /*ctx*/,
    GetAllUsersRequest&& /*request*/
) {
    static const userver::storages::postgres::Query kQuery{
        "SELECT id, name, key FROM users"
    };

    try {
        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery
        );

        aquarium::api::GetAllUsersResponse response;

        auto rows =
            result.AsSetOf<std::tuple<int64_t, std::string, std::string>>(
                userver::storages::postgres::kRowTag
            );

        for (auto [id, name, key] : rows) {
            auto* user = response.add_users();
            user->set_id(id);
            user->set_name(std::move(name));
            user->set_key(std::move(key));
        }

        return response;
    } catch (const std::exception& ex) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(grpc::StatusCode::INTERNAL, ex.what())
        );
    }
}

UserStoreService::GetUserResult UserStoreService::GetUser(
    CallContext& ctx,
    GetUserRequest&& request
) {
    try {
        static const userver::storages::postgres::Query kQuery{
            "SELECT id, name, key FROM users WHERE id = $1",
        };

        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery,
            request.id()
        );

        if (result.IsEmpty()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "Undefined user")
            );
        }

        auto [id, name, key] =
            result.AsSingleRow<std::tuple<int64_t, std::string, std::string>>(
                userver::storages::postgres::kRowTag
            );

        aquarium::api::GetUserResponse response;
        auto* user = response.mutable_user();
        user->set_id(id);
        user->set_name(std::move(name));
        user->set_key(std::move(key));

        return response;

    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& e) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INTERNAL,
                std::string("DB/Internal error: ") + e.what()
            )
        );
    }
}

UserStoreService::AddUserResult UserStoreService::AddUser(
    CallContext& ctx,
    AddUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "INSERT INTO users (name, key) VALUES ($1, $2) RETURNING id"
    };

    std::string name = std::move(*request.mutable_name());
    std::string key = userver::utils::generators::GenerateUuid();

    auto result = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster, kQuery, name, key
    );

    int64_t id = result.AsSingleRow<int64_t>();

    aquarium::api::AddUserResponse response;
    auto* user = response.mutable_user();

    user->set_id(id);
    user->set_name(std::move(name));
    user->set_key(std::move(key));

    return response;
}

UserStoreService::UpdateUserResult UserStoreService::UpdateUser(
    CallContext& ctx,
    UpdateUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "UPDATE users SET name = $2 WHERE key = $1 RETURNING id, name, key"
    };

    try {
        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery,
            request.key(), request.newname()
        );

        if (result.IsEmpty()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "User not found")
            );
        }

        using UserRow = std::tuple<int64_t, std::string, std::string>;
        auto [id, name, key] =
            result.AsSingleRow<UserRow>(userver::storages::postgres::kRowTag);

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
    CallContext& ctx,
    DeleteUserRequest&& request
) {
    static const userver::storages::postgres::Query kQuery{
        "DELETE FROM users WHERE id = $1"
    };

    auto result = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster, kQuery,
        static_cast<int64_t>(request.id())
    );

    aquarium::api::DeleteUserResponse response;
    response.set_success(result.RowsAffected() > 0);

    return response;
}
}; // namespace Aquarium::Users
