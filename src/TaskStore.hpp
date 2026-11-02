#pragma once

#include "./Users.hpp"
#include "userver/components/component_config.hpp"
#include "userver/components/component_context.hpp"
#include "userver/storages/postgres/postgres_fwd.hpp"

#include <cstdint>
#include <google/protobuf/timestamp.pb.h>
#include <string>
#include <TaskStoreService.grpc.pb.h>
#include <TaskStoreService.pb.h>
#include <TaskStoreService_service.usrv.pb.hpp>
#include <unordered_map>
#include <UserStoreService_service.usrv.pb.hpp>
#include <vector>

namespace Aquarium::Tasks {
struct TaskDbInfo {
    std::int64_t Id;
    std::string Title;
    std::string Description;
    std::int64_t AuthorId;
    std::vector<std::int64_t> WorkerIds;
    std::int64_t CheckerId;
    aquarium::api::TaskStatus status;
    userver::storages::postgres::TimePointTz CreatedAt;
    userver::storages::postgres::TimePointTz UpdatedAt;
};

struct TaskCachePolicy {
    static constexpr std::string_view kName = "tasks-pg-cache";

    using ValueType = TaskDbInfo;
    using UpdatedFieldType = userver::storages::postgres::TimePointTz;
    using CacheContainer = std::unordered_map<std::int64_t, TaskDbInfo>;

    static constexpr auto kKeyMember = &TaskDbInfo::Id;
    static constexpr const char* kQuery =
        "SELECT id, workers, checker_id, status, created_at, updated_at "
        "FROM task_schema.task_cache_view";
    static constexpr const char* kUpdatedField = "updated_at";
};

using TaskCache = userver::components::PostgreCache<TaskCachePolicy>;

class TaskStoreService final
    : public aquarium::api::TaskStoreServiceBase::Component {
    using Base = aquarium::api::TaskStoreServiceBase::Component;

private:
    userver::storages::postgres::ClusterPtr _pgCluster;
    Users::AuthCache& _authCache;
    TaskCache& _taskCache;

public:
    static constexpr std::string_view kName = "task-store-service";

    TaskStoreService(
        const userver::components::ComponentConfig& cfg,
        const userver::components::ComponentContext& ctx
    );

    CreateTaskResult CreateTask(
        CallContext& ctx,
        aquarium::api::CreateTaskRequest&& request
    ) override;

    GetTaskResult GetTask(
        CallContext& ctx,
        aquarium::api::GetTaskRequest&& request
    ) override;

    UpdateTaskResult UpdateTask(
        CallContext& ctx,
        aquarium::api::UpdateTaskRequest&& request
    ) override;

    UpdateTaskStatusResult UpdateTaskStatus(
        CallContext& ctx,
        aquarium::api::UpdateTaskStatusRequest&& request
    ) override;

    AssignWorkerResult AssignWorker(
        CallContext& ctx,
        aquarium::api::AssignWorkerRequest&& request
    ) override;

    RemoveWorkerResult RemoveWorker(
        CallContext& ctx,
        aquarium::api::RemoveWorkerRequest&& request
    ) override;

    DeleteTaskResult DeleteTask(
        CallContext& ctx,
        aquarium::api::DeleteTaskRequest&& request
    ) override;
};
} // namespace Aquarium::Tasks
