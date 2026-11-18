---
trigger: always_on
---

# Testing Guidelines for Aquarium

This document outlines the testing strategy, tools, and best practices for the Aquarium service using the **userver** test ecosystem.

---

## 1. Testing Pyramid & Tools

Aquarium leverages userver's built-in testing capabilities:

1. **Unit & Coroutine Tests (`userver::utest`)**:
   - Built on top of GoogleTest (`gtest`).
   - Runs test bodies inside userver's coroutine engine (`TaskProcessor`), allowing asynchronous engine calls, channels, and async primitives to be tested directly.
2. **gRPC Component Unit Tests (`userver::ugrpc::tests::ServiceBase`)**:
   - In-process testing of gRPC handlers without spinning up full external network listeners.
3. **Integration & Functional Tests (`pytest` + `pytest-userver`)**:
   - Uses `testsuite` via `components::TestsuiteSupport` (already enabled in `main.cpp`).
   - Spins up PostgreSQL test instances (e.g., via Docker or test containers).
   - Issues real gRPC calls using Python stubs to verify end-to-end behavior, middleware execution, and database constraints.

---

## 2. Writing Unit & Coroutine Tests with `UTEST`

When testing async code or components that interact with userver engine primitives, use `UTEST` instead of standard `TEST`:

```cpp
#include <userver/utest/utest.hpp>

UTEST(TimestampConversion, ValidConversion) {
    // Coroutine-friendly test execution
    userver::engine::SleepFor(std::chrono::milliseconds(10));
    EXPECT_EQ(1 + 1, 2);
}

class TaskServiceFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup mocks or state
    }
};

UTEST_F(TaskServiceFixture, CreateTaskValidation) {
    // Test logic with fixture
}
```

### Key Assertions & Utest Macros
- Use standard GoogleTest assertions: `EXPECT_EQ`, `ASSERT_TRUE`, `EXPECT_THROW`.
- For userver-specific checks, use `UEXPECT_THROW` and `UASSERT_THROW`.
- Never use blocking sleep calls (`std::this_thread::sleep_for`) inside `UTEST`; always use `userver::engine::SleepFor`.

---

## 3. Database & Caching Tests

### Database Isolation
- Tests interacting with PostgreSQL must run against an isolated test database (see `sql/aquarium.sql`, `sql/users.sql`, `sql/Tasks.sql`).
- Test cases creating database entities must either:
  - Run within a transaction and roll back at test completion, or
  - Clean up created entities (`DELETE FROM ...`) in teardown to prevent state pollution across tests.

### Caching Assertions
- When testing services using `PostgreCache`:
  - Verify that changes in the DB trigger cache updates via `InvalidateAsync(UpdateType::kIncremental)` or `InvalidateAsync(UpdateType::kFull)`.
  - When asserting cache content, account for asynchronous cache invalidation by using poll/wait loops with timeouts (`userver::dump` or testsuite cache control endpoints).

---

## 4. Testing Authentication & Authorization

All gRPC tests must verify security invariants:

1. **Unauthenticated Access**: Verify that calls without `authorization` metadata header return `grpc::StatusCode::UNAUTHENTICATED`.
2. **Invalid Format**: Verify that headers missing `Bearer ` prefix return `grpc::StatusCode::UNAUTHENTICATED`.
3. **Inactive Users**: Verify that requests with a valid token for a user with `active = false` are rejected with `UNAUTHENTICATED`.
4. **Permission Checks**:
   - Non-admin users cannot update or delete resources belonging to other users (`PERMISSION_DENIED`).
   - Admin users can manage any user's resources.
5. **AddUser Invite Key**: Verify that `AddUser` requires `invite-key: Invite <secret>` in metadata and rejects invalid or missing invite keys.

---

## 5. Running Tests

### Running C++ Tests (ctest / Ninja)
```bash
# Build test targets
cmake --build build --target aquarium-unittest

# Run via ctest
ctest --test-dir build --output-on-failure
```

### Running Functional Tests (pytest)
```bash
# Run pytest testsuite
pytest tests/
```
