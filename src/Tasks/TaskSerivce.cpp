#include "./TaskStore.hpp"
#include "userver/cache/update_type.hpp"

#include <algorithm>
#include <cstdint>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/support/status.h>
#include <optional>
#include <ranges>
#include <string>
#include <TaskStoreService.pb.h>
#include <userver/ugrpc/server/exceptions.hpp>
#include <utility>
#include <vector>

namespace Aquarium::Tasks {
TaskStoreService::TaskStoreService(
    const userver::components::ComponentConfig& cfg,
    const userver::components::ComponentContext& ctx
)
    : TaskStoreService::Base(cfg, ctx),
      _pgCluster(ctx.FindComponent<userver::components::Postgres>("postgres-db")
                     .GetCluster()),
      _authCache(ctx.FindComponent<Users::AuthCache>()),
      _taskCache(ctx.FindComponent<TaskCache>()) {}

TaskStoreService::CreateTaskResult TaskStoreService::CreateTask(
    CallContext& ctx,
    aquarium::api::CreateTaskRequest&& request
) {
    static const userver::storages::postgres::Query kInsertTask{
        "INSERT INTO task_schema.tasks (title, description, author_id, "
        "checker_id, status, due_date) "
        "VALUES ($1, $2, $3, $4, $5, $6) "
        "RETURNING id, created_at, updated_at",
        userver::storages::postgres::Query::Name{"insert_task"}
    };

    static const userver::storages::postgres::Query kInsertTaskWorkers{
        "INSERT INTO task_schema.task_workers (task_id, worker_id) "
        "SELECT $1, UNNEST($2::BIGINT[]) "
        "ON CONFLICT DO NOTHING",
        userver::storages::postgres::Query::Name{"insert_task_workers"}
    };

    try {
        std::string title = std::move(*request.mutable_title());
        std::string description = std::move(*request.mutable_description());
        std::int64_t authorId = request.authorid();

        std::vector<std::int64_t> workerIds;
        workerIds.reserve(request.workerids_size());
        for (const auto id : request.workerids()) {
            workerIds.push_back(id);
        }

        std::int64_t checkerId =
            request.has_checkerid() ? request.checkerid() : authorId;
        auto taskStatus = request.status();
        auto statusRaw = static_cast<std::int16_t>(taskStatus);

        std::optional<userver::storages::postgres::TimePointTz> dueDate;
        std::optional<google::protobuf::Timestamp> protoDueDate;
        if (request.has_duedate()) {
            protoDueDate = request.duedate();
            auto tp = std::chrono::system_clock::from_time_t(
                          protoDueDate->seconds()
                      ) +
                      std::chrono::nanoseconds(protoDueDate->nanos());
            dueDate = userver::storages::postgres::TimePointTz{tp};
        }

        auto transaction = _pgCluster->Begin(
            "create_task_trx",
            userver::storages::postgres::ClusterHostType::kMaster,
            userver::storages::postgres::TransactionOptions{}
        );

        auto taskRes = transaction.Execute(
            kInsertTask, title, description, authorId, checkerId, statusRaw,
            dueDate
        );

        auto [taskId, createdAt, updatedAt] = taskRes.AsSingleRow<std::tuple<
            std::int64_t, userver::storages::postgres::TimePointTz,
            userver::storages::postgres::TimePointTz>>();

        if (!workerIds.empty()) {
            transaction.Execute(kInsertTaskWorkers, taskId, workerIds);
        }

        transaction.Commit();
        _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

        aquarium::api::CreateTaskResponse response;
        auto* task = response.mutable_task();

        task->set_id(taskId);
        task->set_title(std::move(title));
        task->set_description(std::move(description));
        task->set_authorid(authorId);
        task->set_checkerid(checkerId);
        task->set_status(taskStatus);

        for (const auto id : workerIds) {
            task->add_workerids(id);
        }

        if (protoDueDate.has_value()) {
            *task->mutable_duedate() = std::move(*protoDueDate);
        }

        auto toProtoTimestamp =
            [](const userver::storages::postgres::TimePointTz& tptz) {
                google::protobuf::Timestamp ts;
                auto duration = tptz.GetUnderlying().time_since_epoch();
                auto seconds =
                    std::chrono::duration_cast<std::chrono::seconds>(duration);
                auto nanos =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        duration - seconds
                    );
                ts.set_seconds(seconds.count());
                ts.set_nanos(static_cast<std::int32_t>(nanos.count()));
                return ts;
            };

        *task->mutable_createdat() = toProtoTimestamp(createdAt);
        *task->mutable_updatedat() = toProtoTimestamp(updatedAt);

        return response;
    } catch (const std::exception& ex) {
        LOG_ERROR() << "CreateTask error: " << ex.what();
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(
                grpc::StatusCode::INTERNAL,
                std::string("DB/Internal error: ") + ex.what()
            )
        );
    }
}

TaskStoreService::GetTaskResult TaskStoreService::GetTask(
    CallContext& ctx,
    aquarium::api::GetTaskRequest&& request
) {
    auto snapshot = _taskCache.Get();
    const auto id = request.id();
    auto it = std::ranges::find_if(*snapshot, [id](const auto& pair) {
        return pair.second.Id == id;
    });

    if (it == snapshot->end()) {
        throw userver::ugrpc::server::ErrorWithStatus(
            grpc::Status(grpc::StatusCode::NOT_FOUND, "Undefined task")
        );
    }

    const auto& task_info = it->second;

    aquarium::api::GetTaskResponse response;

    auto* task = response.mutable_task();

    task->set_id(task_info.Id);
    task->set_title(task_info.Title);
    task->set_description(task_info.Description);

    task->set_authorid(task_info.AuthorId);
    for (int i = 0; auto id : task_info.WorkerIds) {
        task->set_workerids(i++, id);
    }

    task->set_status(task_info.status);

    return response;
}

TaskStoreService::UpdateTaskResult TaskStoreService::UpdateTask(
    CallContext& ctx,
    aquarium::api::UpdateTaskRequest&& request
) {}

TaskStoreService::UpdateTaskStatusResult TaskStoreService::UpdateTaskStatus(
    CallContext& ctx,
    aquarium::api::UpdateTaskStatusRequest&& request
) {}

TaskStoreService::AssignWorkerResult TaskStoreService::AssignWorker(
    CallContext& ctx,
    aquarium::api::AssignWorkerRequest&& request
) {}

TaskStoreService::RemoveWorkerResult TaskStoreService::RemoveWorker(
    CallContext& ctx,
    aquarium::api::RemoveWorkerRequest&& request
) {}

TaskStoreService::DeleteTaskResult TaskStoreService::DeleteTask(
    CallContext& ctx,
    aquarium::api::DeleteTaskRequest&& request
) {}
} // namespace Aquarium::Tasks
