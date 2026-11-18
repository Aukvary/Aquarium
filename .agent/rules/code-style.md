---
trigger: glob
globs: "**/*.cpp, **/*.hpp, **/*.cc, **/*.h, **/*.proto"
---

# Code Style Guidelines for Aquarium

The codebase strictly enforces formatting based on the project's `.clang-format` specification (LLVM-based with 4-space indentation and 80-column limits) and C++20 conventions.

---

## 1. Clang-Format & Formatting Rules

### Layout & Spacing
- **Indentation**: 4 spaces (`IndentWidth: 4`). Never use tabs (`UseTab: Never`).
- **Continuation Indent**: 4 spaces (`ContinuationIndentWidth: 4`).
- **Column Limit**: **80 characters** (`ColumnLimit: 80`). Wrap lines exceeding 80 characters cleanly.
- **Access Modifier Offset**: -4 spaces (`AccessModifierOffset: -4`) so `public:`, `private:`, `protected:` align with the class/struct definition.
- **Pointers & References**: Align to the left (`PointerAlignment: Left`):
  ```cpp
  const UserDbInfo* user = nullptr;
  std::string& name_ref;
  ```

### Braces & Statements
- **Brace Style**: Custom K&R / Attach style:
  - Opening brace on the **same line** for functions, classes, structs, enums, and control statements:
    ```cpp
    class TaskStoreService {
    public:
        void Execute() {
            if (condition) {
                DoWork();
            } else {
                HandleElse();
            }
        }
    };
    ```
  - **No short single-line control statements**: Never write single-line `if`, `for`, `while`, or code blocks (`AllowShortIfStatementsOnASingleLine: Never`, `AllowShortBlocksOnASingleLine: Never`). Always use explicit braces:
    ```cpp
    // Correct
    if (user_it == users.end()) {
        return false;
    }

    // Incorrect
    if (user_it == users.end()) return false;
    ```
  - Empty functions may be collapsed if empty: `void Foo() {}`.

### Functions, Arguments & Templates
- **Template Declarations**: Always break after template declaration (`AlwaysBreakTemplateDeclarations: Yes`):
  ```cpp
  template <typename T>
  class CacheHelper {
      // ...
  };
  ```
- **Space After Template Keyword**: Put a space between `template` and `<` (`SpaceAfterTemplateKeyword: true`).
- **Parameter Wrapping**: Do not bin-pack parameters (`BinPackParameters: false`). Each argument/parameter should be placed on a new line when wrapping:
  ```cpp
  TaskStoreService::GetTaskResult TaskStoreService::GetTask(
      CallContext& ctx,
      aquarium::api::GetTaskRequest&& request
  ) {
      // ...
  }
  ```
- **Constructor Initializers**: Break before colon (`BreakConstructorInitializers: BeforeColon`):
  ```cpp
  TaskStoreService::TaskStoreService(
      const userver::components::ComponentConfig& cfg,
      const userver::components::ComponentContext& ctx
  )
      : TaskStoreService::Base(cfg, ctx),
        _pgCluster(ctx.FindComponent<Postgres>("postgres-db").GetCluster()),
        _taskCache(ctx.FindComponent<TaskCache>()) {}
  ```

### Parentheses & Whitespace
- Space before parentheses only for control statements (`if (cond)`, `for (...)`), **not** function calls (`func(arg)`).
- No space inside parentheses, brackets, or angle brackets (`func(a, b)`, `map[key]`, `std::vector<int>`).
- No space after logical not (`!valid`).
- Space around assignment operators (`a = b + 1`).

---

## 2. Include Headers & Organization

Headers are regrouped and sorted case-insensitively (`IncludeBlocks: Regroup`, `SortIncludes: CaseInsensitive`):

```cpp
// 1. Associated header
#include "./TaskStore.hpp"

// 2. Userver & framework headers
#include <userver/cache/update_type.hpp>
#include <userver/ugrpc/server/exceptions.hpp>

// 3. System & standard library headers
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// 4. Protobuf & generated gRPC headers
#include <google/protobuf/timestamp.pb.h>
#include <TaskStoreService.pb.h>
```

- Always close namespace definitions with a comment:
  ```cpp
  } // namespace Aquarium::Tasks
  ```

---

## 3. Naming Conventions

- **Namespaces**: PascalCase (`namespace Aquarium::Users`, `namespace Aquarium::Tasks`).
- **Classes & Structs**: PascalCase (`UserStoreService`, `TaskDbInfo`, `AuthCachePolicy`).
- **Member Variables**: Lower camelCase prefixed with underscore (`_pgCluster`, `_authCache`, `_taskCache`, `_inviteKey`).
- **Local Variables & Function Parameters**: Lower camelCase (`authorId`, `workerIds`, `taskInfo`, `request`).
- **Constants**: Prefixed with `k` followed by PascalCase (`kQuery`, `kName`, `kAuthUser`, `kInsertTask`).
- **Enums**: PascalCase for enum type, UPPER_SNAKE_CASE or PascalCase for enumerators matching Protobuf generated enums.

---

## 4. Modern C++ (C++20) & Userver Idioms

- **Move Semantics**: Pass protobuf requests by rvalue reference (`Request&& request`) and move fields when taking ownership (`std::move(*request.mutable_title())`).
- **Type Safety**: Prefer strongly-typed integers from `<cstdint>` (`std::int64_t`, `std::int16_t`) over raw `long` or `short`.
- **String Handling**: Use `std::string_view` for read-only string parameters.
- **Exceptions & Errors**: Throw `userver::ugrpc::server::ErrorWithStatus` with appropriate `grpc::StatusCode` for API failures.
- **Timing & Timestamps**: Use `userver::storages::postgres::TimePointTz` for Postgres timestamps and convert to/from `google::protobuf::Timestamp` using standard chrono duration conversions.
