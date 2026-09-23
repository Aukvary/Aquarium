#include "./Users.hpp"

#include <string>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/io/row_types.hpp>

namespace Aquarium::Users {
UserStore::UserStore(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : UserStore::Component{cfg, ctx},
      _pgCluster(ctx.FindComponent<userver::components::Postgres>("postgres-db")
                     .GetCluster()),
      _inviteKey(cfg["invite_key"].As<std::string>()) {}

UserStore::GetAllResult
UserStore::GetAll(CallContext& /*ctx*/, GetAllUsersRequest&& /*request*/) {
    static const std::string query = "SELECT id, name, key FROM users;";

    auto result = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster, query
    );

    auto users = result.AsContainer<std::vector<User>>(
        userver::storages::postgres::kRowTag
    );

    // Создаем напрямую сгенерированный Protobuf-класс ответа
    aquarium::api::GetAllUsersResponse response;

    for (const auto& u : users) {
        auto* proto_user = response.add_users();
        proto_user->set_id(u.id);
        proto_user->set_name(u.name);
        proto_user->set_key(u.key);
    }

    // Возвращаем сам ответ, фреймворк обернет его в Result под капотом
    return response;
}

UserStore::GetUserResult
UserStore::GetUser(CallContext& ctx, GetUserRequest&& request) {}

UserStore::AddUserResult
UserStore::AddUser(CallContext& ctx, AddUserRequest&& request) {}

UserStore::UpdateUserResult
UserStore::UpdateUser(CallContext& ctx, UpdateUserRequest&& request) {}

UserStore::DeleteUserResult
UserStore::DeleteUser(CallContext& ctx, DeleteUserRequest&& request) {}

}; // namespace Aquarium::Users
