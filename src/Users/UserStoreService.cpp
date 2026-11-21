#include "./Users.hpp"
#include "../AccessControl.hpp"
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
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/exceptions.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/storages/postgres/postgres.hpp>
#include <userver/storages/postgres/query.hpp>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/utils/uuid4.hpp>
#include <utility>

namespace Aquarium::Users {
bool IsBlank(std::string_view str) noexcept {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) {
        return std::isspace(c);
    });
}

void ValidateUserName(std::string_view name) {
    if (IsBlank(name)) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                "User name cannot be empty"
            )
        );
    }
    if (name.length() > kMaxUserNameLength) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                "User name cannot exceed 100 characters"
            )
        );
    }
}

const UserDbInfo* FindUserById(
    const std::unordered_map<
        std::string,
        UserDbInfo,
        std::hash<std::string>,
        userver::crypto::algorithm::StringsEqualConstTimeComparator>& users,
    std::int64_t id
) {
    auto it = std::find_if(
        users.begin(), users.end(), [id](const auto& pair) {
            return pair.second.id == id;
        }
    );
    if (it == users.end()) {
        return nullptr;
    }
    return &it->second;
}

userver::ugrpc::server::ErrorWithStatus HandleDatabaseException(
    const std::exception& ex,
    std::string_view operation,
    std::string_view contextInfo
) {
    if (const auto* e =
            dynamic_cast<const userver::ugrpc::server::ErrorWithStatus*>(&ex)) {
        return *e;
    }

    if (const auto* e =
            dynamic_cast<const userver::storages::postgres::UniqueViolation*>(
                &ex
            )) {
        LOG_WARNING() << operation << " unique constraint violation ("
                      << contextInfo << "): " << e->what();
        return userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::ALREADY_EXISTS,
                "Resource already exists"
            )
        );
    }

    if (const auto* e = dynamic_cast<
            const userver::storages::postgres::ForeignKeyViolation*>(&ex)) {
        LOG_WARNING() << operation << " foreign key constraint violation ("
                      << contextInfo << "): " << e->what();
        return userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::FAILED_PRECONDITION,
                "Resource is referenced by other entities"
            )
        );
    }

    if (const auto* e =
            dynamic_cast<const userver::storages::postgres::ConnectionError*>(
                &ex
            )) {
        LOG_ERROR() << operation << " database connection failed ("
                    << contextInfo << "): " << e->what();
        return userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::UNAVAILABLE,
                "Database service unavailable"
            )
        );
    }

    LOG_ERROR() << operation << " unexpected error (" << contextInfo
                << "): " << ex.what();
    return userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error")
    );
}

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
            if (!user_info.active) {
                continue;
            }
            auto* user = response.add_users();
            user->set_id(user_info.id);
            user->set_name(user_info.name);
        }

        return response;
    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& ex) {
        throw HandleDatabaseException(ex, "GetAll users");
    }
}

UserStoreService::GetUserResult UserStoreService::GetUser(
    CallContext& /*ctx*/,
    GetUserRequest&& request
) {
    try {
        auto snapshot = _authCache.Get();

        const auto* user_info = FindUserById(*snapshot, request.id());

        if (!user_info || !user_info->active) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "Undefined user")
            );
        }

        aquarium::api::GetUserResponse response;
        auto* user = response.mutable_user();
        user->set_id(user_info->id);
        user->set_name(user_info->name);

        return response;

    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& e) {
        throw HandleDatabaseException(
            e, "GetUser", "id=" + std::to_string(request.id())
        );
    }
}

UserStoreService::AddUserResult UserStoreService::AddUser(
    CallContext& /*ctx*/,
    AddUserRequest&& request
) {
    ValidateUserName(request.name());

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
    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& ex) {
        throw HandleDatabaseException(ex, "AddUser", "name=" + request.name());
    }
}

UserStoreService::UpdateUserResult UserStoreService::UpdateUser(
    CallContext& ctx,
    UpdateUserRequest&& request
) {
    ValidateUserName(request.newname());

    const auto* user = ctx.GetStorageContext().GetOptional(kAuthUser);
    if (!AccessControl::CanUpdateUser(user, request.id())) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::PERMISSION_DENIED,
                "Only the user itself or an admin can call this method"
            )
        );
    }

    static const userver::storages::postgres::Query kQuery{
        "UPDATE user_schema.tokens SET name = $2, updated = CURRENT_TIMESTAMP "
        "WHERE id = $1 RETURNING id, name, key"
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
        auto* resp_user = response.mutable_user();

        resp_user->set_id(id);
        resp_user->set_name(std::move(name));

        return response;
    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& ex) {
        throw HandleDatabaseException(
            ex, "UpdateUser", "id=" + std::to_string(request.id())
        );
    }
}

UserStoreService::DeleteUserResult UserStoreService::DeleteUser(
    CallContext& ctx,
    DeleteUserRequest&& request
) {
    const auto* user = ctx.GetStorageContext().GetOptional(kAuthUser);
    if (!AccessControl::CanDeleteUser(user, request.id())) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::PERMISSION_DENIED,
                "Only the user itself or an admin can call this method"
            )
        );
    }

    static const userver::storages::postgres::Query kQuery{
        "UPDATE user_schema.tokens SET active = FALSE, updated = CURRENT_TIMESTAMP "
        "WHERE id = $1 AND active = TRUE"
    };

    try {
        auto result = _pgCluster->Execute(
            userver::storages::postgres::ClusterHostType::kMaster, kQuery,
            static_cast<int64_t>(request.id())
        );

        bool success = result.RowsAffected() > 0;
        if (success) {
            _authCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);
        }

        aquarium::api::DeleteUserResponse response;
        response.set_success(success);

        return response;
    } catch (const userver::ugrpc::server::ErrorWithStatus&) {
        throw;
    } catch (const std::exception& ex) {
        throw HandleDatabaseException(
            ex, "DeleteUser", "id=" + std::to_string(request.id())
        );
    }
}
}; // namespace Aquarium::Users
