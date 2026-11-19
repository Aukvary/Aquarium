#include <gmock/gmock.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <AccessControl.hpp>
#include <Users.hpp>
#include <UserStoreService.pb.h>
#include <UserStoreService_client.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <userver/ugrpc/client/call_options.hpp>
#include <userver/ugrpc/client/exceptions.hpp>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/ugrpc/tests/service_fixtures.hpp>
#include <userver/utest/utest.hpp>
#include <utility>

namespace {

class TestUserStoreService final : public aquarium::api::UserStoreServiceBase {
public:
    void AddUserDirect(const Aquarium::Users::UserDbInfo& user) {
        users_[user.id] = user;
    }

    GetAllResult GetAll(
        CallContext& /*context*/,
        aquarium::api::GetAllUsersRequest&& /*request*/
    ) override {
        aquarium::api::GetAllUsersResponse response;
        for (const auto& [id, user] : users_) {
            auto* proto_user = response.add_users();
            proto_user->set_id(user.id);
            proto_user->set_name(user.name);
        }
        return response;
    }

    GetUserResult GetUser(
        CallContext& /*context*/,
        aquarium::api::GetUserRequest&& request
    ) override {
        auto it = users_.find(request.id());
        if (it == users_.end()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "Undefined user")
            );
        }
        aquarium::api::GetUserResponse response;
        auto* proto_user = response.mutable_user();
        proto_user->set_id(it->second.id);
        proto_user->set_name(it->second.name);
        return response;
    }

    AddUserResult AddUser(
        CallContext& /*context*/,
        aquarium::api::AddUserRequest&& request
    ) override {
        std::int64_t next_id = users_.empty() ? 1 : users_.rbegin()->first + 1;
        Aquarium::Users::UserDbInfo user_info{
            next_id,        "generated-key-" + std::to_string(next_id),
            request.name(), {"read"},
            true,           false
        };
        users_[next_id] = user_info;

        aquarium::api::AddUserResponse response;
        auto* proto_user = response.mutable_user();
        proto_user->set_id(user_info.id);
        proto_user->set_name(user_info.name);
        proto_user->set_key(user_info.key);
        return response;
    }

    UpdateUserResult UpdateUser(
        CallContext& context,
        aquarium::api::UpdateUserRequest&& request
    ) override {
        const auto* user =
            context.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
        if (!Aquarium::AccessControl::CanUpdateUser(user, request.id())) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(
                    grpc::StatusCode::PERMISSION_DENIED,
                    "Only the user itself or an admin can call this method"
                )
            );
        }

        auto it = users_.find(request.id());
        if (it == users_.end()) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(grpc::StatusCode::NOT_FOUND, "User not found")
            );
        }
        it->second.name = request.newname();

        aquarium::api::UpdateUserResponse response;
        auto* proto_user = response.mutable_user();
        proto_user->set_id(it->second.id);
        proto_user->set_name(it->second.name);
        return response;
    }

    DeleteUserResult DeleteUser(
        CallContext& context,
        aquarium::api::DeleteUserRequest&& request
    ) override {
        const auto* user =
            context.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
        if (!Aquarium::AccessControl::CanDeleteUser(user, request.id())) {
            throw userver::ugrpc::server::ErrorWithStatus(
                grpc::Status(
                    grpc::StatusCode::PERMISSION_DENIED,
                    "Only the user itself or an admin can call this method"
                )
            );
        }

        auto it = users_.find(request.id());
        bool success = (it != users_.end());
        if (success) {
            users_.erase(it);
        }
        aquarium::api::DeleteUserResponse response;
        response.set_success(success);
        return response;
    }

private:
    std::map<std::int64_t, Aquarium::Users::UserDbInfo> users_;
};

class UserServiceGrpcFixture
    : public userver::ugrpc::tests::ServiceFixtureBase {
protected:
    UserServiceGrpcFixture() {
        Aquarium::Users::UserDbInfo admin_user{1,       "admin-token",
                                               "Admin", {"read", "write"},
                                               true,    true};
        Aquarium::Users::UserDbInfo alice{2,        "alice-token", "Alice",
                                          {"read"}, true,          false};
        Aquarium::Users::UserDbInfo bob{3,        "bob-token", "Bob",
                                        {"read"}, true,        false};
        Aquarium::Users::UserDbInfo charlie_inactive{4,         "charlie-token",
                                                     "Charlie", {"read"},
                                                     false,     false};

        token_to_user_[admin_user.key] = admin_user;
        token_to_user_[alice.key] = alice;
        token_to_user_[bob.key] = bob;
        token_to_user_[charlie_inactive.key] = charlie_inactive;

        service_.AddUserDirect(admin_user);
        service_.AddUserDirect(alice);
        service_.AddUserDirect(bob);
        service_.AddUserDirect(charlie_inactive);

        auto middleware =
            std::make_shared<Aquarium::Users::AuthenticationMiddleware>(
                "secret_invite_key",
                [this](std::string_view token)
                    -> std::optional<Aquarium::Users::UserDbInfo> {
                    auto it = token_to_user_.find(std::string(token));
                    if (it != token_to_user_.end()) {
                        return it->second;
                    }
                    return std::nullopt;
                },
                "internal_secret_key"
            );

        SetServerMiddlewares({std::move(middleware)});
        RegisterService(service_);
        StartServer();
        client_ = MakeClient<aquarium::api::UserStoreServiceClient>();
    }

    ~UserServiceGrpcFixture() override {
        client_.reset();
        StopServer();
    }

    aquarium::api::UserStoreServiceClient& Client() {
        return *client_;
    }

    static userver::ugrpc::client::CallOptions MakeAuthOptions(
        const std::string& token
    ) {
        userver::ugrpc::client::CallOptions opts;
        opts.AddMetadata("authorization", "Bearer " + token);
        return opts;
    }

    static userver::ugrpc::client::CallOptions MakeInviteOptions(
        const std::string& invite_key
    ) {
        userver::ugrpc::client::CallOptions opts;
        opts.AddMetadata("invite-key", "Invite " + invite_key);
        return opts;
    }

protected:
    std::unordered_map<std::string, Aquarium::Users::UserDbInfo> token_to_user_;
    TestUserStoreService service_;
    std::optional<aquarium::api::UserStoreServiceClient> client_;
};

} // namespace

UTEST(UserStoreUnitTests, FindUserById_EmptyContainer) {
    Aquarium::Users::AuthCachePolicy::CacheContainer users;
    const auto* result = Aquarium::Users::FindUserById(users, 1);
    EXPECT_EQ(result, nullptr);
}

UTEST(UserStoreUnitTests, FindUserById_FoundAndNotFound) {
    Aquarium::Users::AuthCachePolicy::CacheContainer users;
    users["key-1"] =
        Aquarium::Users::UserDbInfo{10, "key-1", "User10", {}, true, false};
    users["key-2"] =
        Aquarium::Users::UserDbInfo{20, "key-2", "User20", {}, true, true};

    const auto* user10 = Aquarium::Users::FindUserById(users, 10);
    ASSERT_NE(user10, nullptr);
    EXPECT_EQ(user10->id, 10);
    EXPECT_EQ(user10->name, "User10");

    const auto* user20 = Aquarium::Users::FindUserById(users, 20);
    ASSERT_NE(user20, nullptr);
    EXPECT_EQ(user20->id, 20);
    EXPECT_TRUE(user20->admin);

    const auto* user99 = Aquarium::Users::FindUserById(users, 99);
    EXPECT_EQ(user99, nullptr);
}

UTEST(UserStoreUnitTests, AccessControl_UpdateUser) {
    Aquarium::Users::UserDbInfo regular_user{2,  "key", "Alice",
                                             {}, true,  false};
    Aquarium::Users::UserDbInfo admin_user{1, "key", "Admin", {}, true, true};

    // Self update allowed
    EXPECT_TRUE(Aquarium::AccessControl::CanUpdateUser(&regular_user, 2));

    // Other update denied for non-admin
    EXPECT_FALSE(Aquarium::AccessControl::CanUpdateUser(&regular_user, 3));

    // Admin can update self and others
    EXPECT_TRUE(Aquarium::AccessControl::CanUpdateUser(&admin_user, 1));
    EXPECT_TRUE(Aquarium::AccessControl::CanUpdateUser(&admin_user, 2));
    EXPECT_TRUE(Aquarium::AccessControl::CanUpdateUser(&admin_user, 3));

    // Null user denied
    EXPECT_FALSE(Aquarium::AccessControl::CanUpdateUser(nullptr, 2));
}

UTEST(UserStoreUnitTests, AccessControl_DeleteUser) {
    Aquarium::Users::UserDbInfo regular_user{2,  "key", "Alice",
                                             {}, true,  false};
    Aquarium::Users::UserDbInfo admin_user{1, "key", "Admin", {}, true, true};

    // Self delete allowed
    EXPECT_TRUE(Aquarium::AccessControl::CanDeleteUser(&regular_user, 2));

    // Other delete denied for non-admin
    EXPECT_FALSE(Aquarium::AccessControl::CanDeleteUser(&regular_user, 3));

    // Admin can delete anyone
    EXPECT_TRUE(Aquarium::AccessControl::CanDeleteUser(&admin_user, 2));
    EXPECT_TRUE(Aquarium::AccessControl::CanDeleteUser(&admin_user, 3));

    // Null user denied
    EXPECT_FALSE(Aquarium::AccessControl::CanDeleteUser(nullptr, 2));
}

// ============================================================================
// gRPC Middleware Authentication & Authorization Tests
// ============================================================================

UTEST_F(UserServiceGrpcFixture, GetAll_WithoutAuth_ThrowsUnauthenticated) {
    aquarium::api::GetAllUsersRequest req;
    UEXPECT_THROW(
        Client().GetAll(req, {}), userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(
    UserServiceGrpcFixture,
    GetAll_InvalidAuthFormat_ThrowsUnauthenticated
) {
    aquarium::api::GetAllUsersRequest req;
    userver::ugrpc::client::CallOptions opts;
    opts.AddMetadata("authorization", "Basic some_basic_token");
    UEXPECT_THROW(
        Client().GetAll(req, std::move(opts)),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(UserServiceGrpcFixture, GetAll_UnknownToken_ThrowsUnauthenticated) {
    aquarium::api::GetAllUsersRequest req;
    UEXPECT_THROW(
        Client().GetAll(req, MakeAuthOptions("non-existent-token")),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(UserServiceGrpcFixture, GetAll_InactiveUser_ThrowsUnauthenticated) {
    aquarium::api::GetAllUsersRequest req;
    UEXPECT_THROW(
        Client().GetAll(req, MakeAuthOptions("charlie-token")),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(UserServiceGrpcFixture, GetAll_ValidUser_Succeeds) {
    aquarium::api::GetAllUsersRequest req;
    auto resp = Client().GetAll(req, MakeAuthOptions("alice-token"));
    EXPECT_GE(resp.users_size(), 3);
}

UTEST_F(UserServiceGrpcFixture, GetAll_InternalKey_Succeeds) {
    aquarium::api::GetAllUsersRequest req;
    userver::ugrpc::client::CallOptions opts;
    opts.AddMetadata("authorization", "Bearer internal_secret_key");
    auto resp = Client().GetAll(req, std::move(opts));
    EXPECT_GE(resp.users_size(), 3);
}

UTEST_F(UserServiceGrpcFixture, GetUser_ExistingUser_Succeeds) {
    aquarium::api::GetUserRequest req;
    req.set_id(2);
    auto resp = Client().GetUser(req, MakeAuthOptions("alice-token"));
    EXPECT_EQ(resp.user().id(), 2);
    EXPECT_EQ(resp.user().name(), "Alice");
}

UTEST_F(UserServiceGrpcFixture, GetUser_NonExistentUser_ThrowsNotFound) {
    aquarium::api::GetUserRequest req;
    req.set_id(9999);
    UEXPECT_THROW(
        Client().GetUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::NotFoundError
    );
}

UTEST_F(
    UserServiceGrpcFixture,
    AddUser_WithoutInviteKey_ThrowsUnauthenticated
) {
    aquarium::api::AddUserRequest req;
    req.set_name("NewUser");
    UEXPECT_THROW(
        Client().AddUser(req, {}), userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(
    UserServiceGrpcFixture,
    AddUser_InvalidInviteFormat_ThrowsUnauthenticated
) {
    aquarium::api::AddUserRequest req;
    req.set_name("NewUser");
    userver::ugrpc::client::CallOptions opts;
    opts.AddMetadata("invite-key", "Bearer secret_invite_key");
    UEXPECT_THROW(
        Client().AddUser(req, std::move(opts)),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(UserServiceGrpcFixture, AddUser_WrongInviteKey_ThrowsUnauthenticated) {
    aquarium::api::AddUserRequest req;
    req.set_name("NewUser");
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("wrong_key")),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(UserServiceGrpcFixture, AddUser_ValidInviteKey_Succeeds) {
    aquarium::api::AddUserRequest req;
    req.set_name("NewUser");
    auto resp = Client().AddUser(req, MakeInviteOptions("secret_invite_key"));
    EXPECT_EQ(resp.user().name(), "NewUser");
    EXPECT_GT(resp.user().id(), 0);
}

UTEST_F(UserServiceGrpcFixture, UpdateUser_Self_Succeeds) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(2);
    req.set_newname("Alice_Updated");
    auto resp = Client().UpdateUser(req, MakeAuthOptions("alice-token"));
    EXPECT_EQ(resp.user().name(), "Alice_Updated");
}

UTEST_F(
    UserServiceGrpcFixture,
    UpdateUser_Other_ByNonAdmin_ThrowsPermissionDenied
) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(3); // Alice (id=2) attempts to update Bob (id=3)
    req.set_newname("Bob_Hacked");
    UEXPECT_THROW(
        Client().UpdateUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::PermissionDeniedError
    );
}

UTEST_F(UserServiceGrpcFixture, UpdateUser_Other_ByAdmin_Succeeds) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(3); // Admin (id=1) updates Bob (id=3)
    req.set_newname("Bob_RenamedByAdmin");
    auto resp = Client().UpdateUser(req, MakeAuthOptions("admin-token"));
    EXPECT_EQ(resp.user().name(), "Bob_RenamedByAdmin");
}

UTEST_F(
    UserServiceGrpcFixture,
    DeleteUser_Other_ByNonAdmin_ThrowsPermissionDenied
) {
    aquarium::api::DeleteUserRequest req;
    req.set_id(3); // Alice (id=2) attempts to delete Bob (id=3)
    UEXPECT_THROW(
        Client().DeleteUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::PermissionDeniedError
    );
}

UTEST_F(UserServiceGrpcFixture, DeleteUser_Self_Succeeds) {
    aquarium::api::DeleteUserRequest req;
    req.set_id(2); // Alice deletes self
    auto resp = Client().DeleteUser(req, MakeAuthOptions("alice-token"));
    EXPECT_TRUE(resp.success());
}

UTEST_F(UserServiceGrpcFixture, DeleteUser_Other_ByAdmin_Succeeds) {
    aquarium::api::DeleteUserRequest req;
    req.set_id(3); // Admin deletes Bob
    auto resp = Client().DeleteUser(req, MakeAuthOptions("admin-token"));
    EXPECT_TRUE(resp.success());
}
