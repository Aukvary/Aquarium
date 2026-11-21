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
#include <userver/storages/postgres/exceptions.hpp>
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
            if (!user.active) {
                continue;
            }
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
        if (it == users_.end() || !it->second.active) {
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
        Aquarium::Users::ValidateUserName(request.name());
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
        Aquarium::Users::ValidateUserName(request.newname());
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
        bool success = (it != users_.end() && it->second.active);
        if (success) {
            it->second.active = false;
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

class UserServiceRemotePeerFixture
    : public userver::ugrpc::tests::ServiceFixtureBase {
public:
    UserServiceRemotePeerFixture() {
        auto middleware =
            std::make_shared<Aquarium::Users::AuthenticationMiddleware>(
                "secret_invite_key",
                [this](std::string_view /*token*/)
                    -> std::optional<Aquarium::Users::UserDbInfo> {
                    return std::nullopt;
                },
                "internal_secret_key",
                [](std::string_view /*peer*/) {
                    return false; // Mock remote peer
                }
            );

        SetServerMiddlewares({std::move(middleware)});
        RegisterService(service_);
        StartServer();
        client_ = MakeClient<aquarium::api::UserStoreServiceClient>();
    }

    ~UserServiceRemotePeerFixture() override {
        client_.reset();
        StopServer();
    }

    aquarium::api::UserStoreServiceClient& Client() {
        return *client_;
    }

protected:
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

UTEST(UserStoreUnitTests, HandleDatabaseException_SanitizationAndMapping) {
    // 1. Generic DB exception -> sanitized INTERNAL ("Internal server error")
    std::runtime_error sql_leak_error(
        "relation \"user_schema.tokens\" does not exist at character 15, "
        "table column name leak"
    );
    auto err1 = Aquarium::Users::HandleDatabaseException(
        sql_leak_error, "GetUser", "id=42"
    );
    EXPECT_EQ(err1.GetStatus().error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_EQ(err1.GetStatus().error_message(), "Internal server error");
    EXPECT_EQ(
        err1.GetStatus().error_message().find("relation"), std::string::npos
    );
    EXPECT_EQ(
        err1.GetStatus().error_message().find("tokens"), std::string::npos
    );

    // 2. Existing ErrorWithStatus preserved
    userver::ugrpc::server::ErrorWithStatus not_found_err(
        grpc::Status(grpc::StatusCode::NOT_FOUND, "Custom entity not found")
    );
    auto err2 = Aquarium::Users::HandleDatabaseException(
        not_found_err, "GetUser", "id=99"
    );
    EXPECT_EQ(err2.GetStatus().error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_EQ(err2.GetStatus().error_message(), "Custom entity not found");

    // 3. ConnectionError -> UNAVAILABLE ("Database service unavailable")
    userver::storages::postgres::ConnectionError conn_err(
        "Could not connect to server on socket /tmp/.s.PGSQL.5432"
    );
    auto err3 = Aquarium::Users::HandleDatabaseException(
        conn_err, "AddUser", "name=Bob"
    );
    EXPECT_EQ(err3.GetStatus().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(
        err3.GetStatus().error_message(), "Database service unavailable"
    );
    EXPECT_EQ(
        err3.GetStatus().error_message().find("socket"), std::string::npos
    );

    // 4. PoolError -> sanitized INTERNAL ("Internal server error")
    userver::storages::postgres::PoolError pool_err(
        "Pool queue timeout after 1000ms"
    );
    auto err4 = Aquarium::Users::HandleDatabaseException(
        pool_err, "AddUser", "name=Alice"
    );
    EXPECT_EQ(err4.GetStatus().error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_EQ(err4.GetStatus().error_message(), "Internal server error");
}

UTEST(AuthCacheTests, CacheContainerUpdate) {
    Aquarium::Users::AuthCachePolicy::CacheContainer users;
    users["key-1"] =
        Aquarium::Users::UserDbInfo{10, "key-1", "Alice", {}, true, false};

    const auto* user_before = Aquarium::Users::FindUserById(users, 10);
    ASSERT_NE(user_before, nullptr);
    EXPECT_EQ(user_before->name, "Alice");

    // Profile update: name changed
    users["key-1"].name = "Alice_Updated";
    const auto* user_after = Aquarium::Users::FindUserById(users, 10);
    ASSERT_NE(user_after, nullptr);
    EXPECT_EQ(user_after->name, "Alice_Updated");
}

UTEST(AuthCacheTests, CacheContainerDeactivation) {
    Aquarium::Users::AuthCachePolicy::CacheContainer users;
    users["key-1"] =
        Aquarium::Users::UserDbInfo{10, "key-1", "Bob", {}, true, false};

    const auto* user = Aquarium::Users::FindUserById(users, 10);
    ASSERT_NE(user, nullptr);
    EXPECT_TRUE(user->active);

    // Deactivation
    users["key-1"].active = false;
    const auto* deactivated_user = Aquarium::Users::FindUserById(users, 10);
    ASSERT_NE(deactivated_user, nullptr);
    EXPECT_FALSE(deactivated_user->active);
}

UTEST(AuthCacheTests, CacheContainerPurgeOnFullInvalidation) {
    Aquarium::Users::AuthCachePolicy::CacheContainer users;
    users["key-1"] =
        Aquarium::Users::UserDbInfo{10, "key-1", "Charlie", {}, true, false};

    EXPECT_NE(users.find("key-1"), users.end());
    EXPECT_NE(Aquarium::Users::FindUserById(users, 10), nullptr);

    // Hard deletion + Full invalidation purges row from cache
    users.erase("key-1");
    EXPECT_EQ(users.find("key-1"), users.end());
    EXPECT_EQ(Aquarium::Users::FindUserById(users, 10), nullptr);
}

UTEST(AuthenticationTests, IsLoopbackPeer) {
    // Loopback IPv4
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("ipv4:127.0.0.1:8080"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("ipv4:127.1.2.3:1234"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("127.0.0.1:8080"));

    // Loopback IPv6
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("ipv6:[::1]:8080"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("ipv6:::1:8080"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("[::1]:8080"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("::1:8080"));

    // IPv4-mapped IPv6 loopback
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("ipv6:[::ffff:127.0.0.1]:8080"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("[::ffff:127.0.0.1]:8080"));

    // Unix domain sockets & inproc
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("unix:/tmp/aquarium.sock"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("unix-abstract:aquarium"));
    EXPECT_TRUE(Aquarium::Users::IsLoopbackPeer("inproc"));

    // Remote / non-loopback addresses
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer("ipv4:192.168.1.100:8080"));
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer("ipv4:10.0.0.1:8080"));
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer("ipv4:172.16.0.1:8080"));
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer("ipv4:8.8.8.8:8080"));
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer("ipv6:[2001:db8::1]:8080"));
    EXPECT_FALSE(Aquarium::Users::IsLoopbackPeer(""));
}

UTEST(AuthenticationTests, ValidateStartupKeys) {
    const std::string valid_internal_key =
        "valid_internal_key_minimum_32_characters_12345";
    const std::string valid_invite_key =
        "valid_invite_key_minimum_32_characters_long_123";

    // Valid production keys
    EXPECT_NO_THROW(Aquarium::Users::ValidateStartupKeys(
        valid_internal_key, valid_invite_key, false
    ));

    // Empty keys rejected in both modes
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys("", valid_invite_key, false),
        std::invalid_argument
    );
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys(valid_internal_key, "", false),
        std::invalid_argument
    );
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys("", valid_invite_key, true),
        std::invalid_argument
    );
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys(valid_internal_key, "", true),
        std::invalid_argument
    );

    // Short keys rejected in production mode
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys("short_key", valid_invite_key, false),
        std::invalid_argument
    );
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys(
            valid_internal_key, "short_key", false
        ),
        std::invalid_argument
    );

    // Weak / default secrets rejected in production mode
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys("zxc", valid_invite_key, false),
        std::invalid_argument
    );
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys(
            valid_internal_key, "secret_invite_key_value", false
        ),
        std::invalid_argument
    );

    // Low entropy rejected in production mode
    EXPECT_THROW(
        Aquarium::Users::ValidateStartupKeys(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", valid_invite_key, false
        ),
        std::invalid_argument
    );

    // In test mode, shorter and development keys are allowed
    EXPECT_NO_THROW(Aquarium::Users::ValidateStartupKeys(
        "zxc", "secret_invite_key_value", true
    ));
}

UTEST(AuthenticationTests, ConstantTimeCompare) {
    // Identical strings
    EXPECT_TRUE(Aquarium::Users::ConstantTimeCompare("secret123", "secret123"));
    EXPECT_TRUE(Aquarium::Users::ConstantTimeCompare("", ""));

    // Long keys (such as real 32+ char keys)
    const std::string key1 = "very_long_secret_key_32_chars_123456";
    const std::string key2 = "very_long_secret_key_32_chars_123456";
    EXPECT_TRUE(Aquarium::Users::ConstantTimeCompare(key1, key2));

    // Same length, single character difference at beginning, middle, and end
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("xecret123", "secret123"));
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("secrxt123", "secret123"));
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("secret124", "secret123"));

    // Different lengths (prefix or suffix matching)
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("secret12", "secret123"));
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("secret1234", "secret123"));
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("", "secret123"));
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("secret123", ""));

    // Completely different strings
    EXPECT_FALSE(Aquarium::Users::ConstantTimeCompare("abcdefghi", "123456789"));
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

UTEST_F(
    UserServiceRemotePeerFixture,
    GetAll_InternalKey_FromRemotePeer_ThrowsPermissionDenied
) {
    aquarium::api::GetAllUsersRequest req;
    userver::ugrpc::client::CallOptions opts;
    opts.AddMetadata("authorization", "Bearer internal_secret_key");
    UEXPECT_THROW(
        Client().GetAll(req, std::move(opts)),
        userver::ugrpc::client::PermissionDeniedError
    );
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

UTEST_F(
    UserServiceGrpcFixture,
    AddUser_InviteKeyTimingEdgeCases_ThrowsUnauthenticated
) {
    aquarium::api::AddUserRequest req;
    req.set_name("NewUser");

    // Prefix of valid key
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_ke")),
        userver::ugrpc::client::UnauthenticatedError
    );

    // Key with extra character appended
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_keyX")),
        userver::ugrpc::client::UnauthenticatedError
    );

    // Same length, 1 character difference at start
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("xecret_invite_key")),
        userver::ugrpc::client::UnauthenticatedError
    );

    // Same length, 1 character difference at end
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_kez")),
        userver::ugrpc::client::UnauthenticatedError
    );
}

UTEST_F(
    UserServiceGrpcFixture,
    InternalKey_TimingEdgeCases_ThrowsUnauthenticated
) {
    aquarium::api::GetAllUsersRequest req;

    // Prefix of internal key
    UEXPECT_THROW(
        Client().GetAll(req, MakeAuthOptions("internal_secret_ke")),
        userver::ugrpc::client::UnauthenticatedError
    );

    // Key with extra character appended
    UEXPECT_THROW(
        Client().GetAll(req, MakeAuthOptions("internal_secret_keyX")),
        userver::ugrpc::client::UnauthenticatedError
    );

    // Same length, 1 character difference
    UEXPECT_THROW(
        Client().GetAll(req, MakeAuthOptions("xnternal_secret_key")),
        userver::ugrpc::client::UnauthenticatedError
    );
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

UTEST_F(UserServiceGrpcFixture, GetAll_OmitsDeactivatedUsers) {
    // Bob (id=3) is active initially
    aquarium::api::GetAllUsersRequest get_all_req;
    auto initial_all = Client().GetAll(get_all_req, MakeAuthOptions("admin-token"));
    bool found_bob_initially = false;
    for (const auto& u : initial_all.users()) {
        if (u.id() == 3) {
            found_bob_initially = true;
            break;
        }
    }
    EXPECT_TRUE(found_bob_initially);

    // Delete Bob
    aquarium::api::DeleteUserRequest del_req;
    del_req.set_id(3);
    auto del_resp = Client().DeleteUser(del_req, MakeAuthOptions("admin-token"));
    EXPECT_TRUE(del_resp.success());

    // GetAll should no longer include Bob
    auto after_all = Client().GetAll(get_all_req, MakeAuthOptions("admin-token"));
    bool found_bob_after = false;
    for (const auto& u : after_all.users()) {
        if (u.id() == 3) {
            found_bob_after = true;
            break;
        }
    }
    EXPECT_FALSE(found_bob_after);
}

UTEST_F(UserServiceGrpcFixture, GetUser_DeactivatedUser_ThrowsNotFound) {
    // Delete Bob (id=3)
    aquarium::api::DeleteUserRequest del_req;
    del_req.set_id(3);
    auto del_resp = Client().DeleteUser(del_req, MakeAuthOptions("admin-token"));
    EXPECT_TRUE(del_resp.success());

    // GetUser for Bob throws NotFound
    aquarium::api::GetUserRequest get_req;
    get_req.set_id(3);
    UEXPECT_THROW(
        Client().GetUser(get_req, MakeAuthOptions("admin-token")),
        userver::ugrpc::client::NotFoundError
    );
}

UTEST_F(UserServiceGrpcFixture, DeleteUser_AlreadyDeactivated_ReturnsFalse) {
    aquarium::api::DeleteUserRequest del_req;
    del_req.set_id(3);
    auto first_resp = Client().DeleteUser(del_req, MakeAuthOptions("admin-token"));
    EXPECT_TRUE(first_resp.success());

    auto second_resp = Client().DeleteUser(del_req, MakeAuthOptions("admin-token"));
    EXPECT_FALSE(second_resp.success());
}

UTEST_F(UserServiceGrpcFixture, AddUser_EmptyName_ThrowsInvalidArgument) {
    aquarium::api::AddUserRequest req;
    req.set_name("");
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_key")),
        userver::ugrpc::client::InvalidArgumentError
    );
}

UTEST_F(UserServiceGrpcFixture, AddUser_WhitespaceName_ThrowsInvalidArgument) {
    aquarium::api::AddUserRequest req;
    req.set_name("   \t  \n ");
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_key")),
        userver::ugrpc::client::InvalidArgumentError
    );
}

UTEST_F(UserServiceGrpcFixture, AddUser_NameExceeds100Chars_ThrowsInvalidArgument) {
    aquarium::api::AddUserRequest req;
    req.set_name(std::string(101, 'a'));
    UEXPECT_THROW(
        Client().AddUser(req, MakeInviteOptions("secret_invite_key")),
        userver::ugrpc::client::InvalidArgumentError
    );
}

UTEST_F(UserServiceGrpcFixture, UpdateUser_EmptyName_ThrowsInvalidArgument) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(2);
    req.set_newname("");
    UEXPECT_THROW(
        Client().UpdateUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::InvalidArgumentError
    );
}

UTEST_F(UserServiceGrpcFixture, UpdateUser_WhitespaceName_ThrowsInvalidArgument) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(2);
    req.set_newname("   \n\t  ");
    UEXPECT_THROW(
        Client().UpdateUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::InvalidArgumentError
    );
}

UTEST_F(UserServiceGrpcFixture, UpdateUser_NameExceeds100Chars_ThrowsInvalidArgument) {
    aquarium::api::UpdateUserRequest req;
    req.set_id(2);
    req.set_newname(std::string(101, 'b'));
    UEXPECT_THROW(
        Client().UpdateUser(req, MakeAuthOptions("alice-token")),
        userver::ugrpc::client::InvalidArgumentError
    );
}
