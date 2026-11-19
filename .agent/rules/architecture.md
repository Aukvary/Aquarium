---
trigger: always_on
---

# Architecture Guidelines for Aquarium

## 1. High-Level Overview
Aquarium is a high-performance backend microservice developed in **C++20** using the **userver** asynchronous framework (v3.2). It exposes **gRPC** interfaces defined in Protocol Buffers (proto3) and interacts with a **PostgreSQL** relational database.

### Core Technologies
- **Language**: C++20
- **Framework**: Yandex `userver` (coroutine-based engine, `ugrpc`, `postgresql`)
- **Protocol**: gRPC (`userver::ugrpc::server` and `userver::ugrpc::client`)
- **Database**: PostgreSQL 16 (`userver::storages::postgres`)
- **Caching**: `userver::components::PostgreCache` (in-memory Postgres cache)
- **Build System**: CMake (>= 3.20) + Ninja

---

## 2. Directory Structure & Layering

```
Aquarium/
├── CMakeLists.txt              # CMake build definitions (FetchContent userver, protobuf, grpc)
├── build.sh                    # Build & database migration runner
├── docker-compose.yml          # Containerized dependencies (Postgres)
├── configs/
│   ├── static_config.yaml      # Static userver component topology and configs
│   └── config_vars.yaml        # Environmental variables & secrets for static config
├── proto/                      # gRPC Service & message definitions (proto3)
│   ├── UserStoreService.proto  # User management RPCs
│   ├── TaskStoreService.proto  # Task management RPCs
│   └── UtilsService.proto      # Health check and utilities RPCs
├── sql/                        # SQL schemas and migration scripts
│   ├── aquarium.sql            # Database initialization
│   ├── users.sql               # user_schema (tokens/users table)
│   └── Tasks.sql               # task_schema (tasks, task_workers, view, triggers)
└── src/                        # Service implementations and domain logic
    ├── main.cpp                # Component list registration & DaemonMain entry point
    ├── AccessControl.hpp       # AccessControl authorization rules (users & tasks)
    ├── Users.hpp               # User types, AuthCache policy, middleware declaration
    ├── Users/
    │   ├── UserStoreService.cpp# UserStore gRPC service implementation
    │   └── Auth.cpp            # AuthenticationMiddleware & authentication checks
    ├── TaskStore.hpp           # Task types, TaskCache policy, TaskStoreService declaration
    ├── Tasks/
    │   └── TaskService.cpp     # TaskStore gRPC service implementation
    ├── Utils.hpp               # UtilsService declaration & client wrappers
    └── Utils/
        ├── HealthService.cpp   # Health service implementation
        └── AccessControl.cpp   # AccessControl permission checks implementation
```

---

## 3. Component Architecture & Dependency Graph

Userver uses an inversion-of-control component system. All services, caches, and database pools are registered in `src/main.cpp` via `components::MinimalServerComponentList()`:

1. **`postgres-db`** (`userver::components::Postgres`):
   - Manages connection pooling and query execution.
   - Referenced by `AuthCache`, `TaskCache`, and services via `ClusterPtr`.
2. **`auth-pg-cache` & `tasks-pg-cache`** (`userver::components::PostgreCache`):
   - Periodically update in-memory cache snapshots from Postgres.
   - Support incremental invalidation (`InvalidateAsync(UpdateType::kIncremental)`).
3. **`authentication-middleware`** (`userver::ugrpc::server::MiddlewareFactoryComponentBase`):
   - Attached to gRPC server pipeline.
   - Extracts metadata headers (`authorization: Bearer <key>`, `invite-key: Invite <key>`).
   - Validates tokens against `AuthCache` and stores the authenticated `UserDbInfo` in the request's `StorageContext`.
4. **gRPC Services** (`userver::ugrpc::server::ServiceComponentBase`):
   - `UserStoreService`: Handles user CRUD.
   - `TaskStoreService`: Handles task lifecycle and assignments.
   - `UtilsService`: Service health monitoring.

---

## 4. Key Architectural Patterns & Constraints

### 4.1. Concurrency & Coroutines
- All business logic runs inside userver coroutine threads (`main-task-processor`).
- **Never block the coroutine thread** with synchronous I/O or blocking syscalls (`std::this_thread::sleep_for`, blocking sockets, or sync disk I/O).
- Use userver's async primitives: `userver::engine::SleepFor`, `engine::Task`, `engine::AsyncNoSpan`.
- For blocking operations (e.g. legacy libraries, filesystem access), dispatch them to `fs-task-processor`.

### 4.2. Database Operations
- Always obtain a cluster handle via `_pgCluster->Execute(...)` or `_pgCluster->Begin(...)`.
- Use parameterized queries with `$1, $2, ...` to prevent SQL injection.
- Mark queries `static const userver::storages::postgres::Query` to leverage query caching and name tagging.
- Multi-step modifications (e.g. creating a task and inserting workers) **must** run inside a Postgres transaction (`_pgCluster->Begin(...)`).

### 4.3. Cache Consistency Rules
- When updating or deleting records that are tracked by a `PostgreCache`, remember:
  - Updates must touch the timestamp column (e.g. `updated` or `updated_at`) so incremental caching queries (`WHERE updated > $last_update`) pick them up.
  - Deletions are not captured by incremental queries unless soft-delete/tombstone patterns are used, or `InvalidateAsync(UpdateType::kFull)` is triggered.
- Cache lookups:
  - If the cache container is an `unordered_map` keyed by primary ID, use direct hash lookup (`snapshot->find(id)`), not linear scans.

### 4.4. Security & Permissions
- Authentication tokens must never be exposed or returned in batch retrieval RPCs (like `GetAllUsers`).
- Every mutation endpoint must authenticate the caller and verify that `request.author_id` or target resources belong to the caller (or that caller has `admin` role).
- gRPC errors must use standard `grpc::StatusCode` mapped cleanly through `userver::ugrpc::server::ErrorWithStatus`:
  - `StatusCode::UNAUTHENTICATED`: Missing or invalid credentials.
  - `StatusCode::PERMISSION_DENIED`: Valid credentials, but insufficient permissions.
  - `StatusCode::NOT_FOUND`: Resource missing.
  - `StatusCode::INVALID_ARGUMENT`: Validation failure.
  - Never leak raw SQL or database internal errors to clients.
