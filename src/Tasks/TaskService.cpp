#include "./TaskStore.hpp"
#include "../AccessControl.hpp"

#include <userver/cache/update_type.hpp>
#include <userver/logging/log.hpp>
#include <userver/storages/postgres/cluster.hpp>
#include <userver/storages/postgres/exceptions.hpp>
#include <userver/storages/postgres/io/row_types.hpp>
#include <userver/ugrpc/server/exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <TaskStoreService.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/support/status.h>

namespace Aquarium::Tasks {

google::protobuf::Timestamp
ToProtoTimestamp(const userver::storages::postgres::TimePointTz &tptz) {
  google::protobuf::Timestamp ts;
  const auto duration = tptz.GetUnderlying().time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
  ts.set_seconds(seconds.count());
  ts.set_nanos(static_cast<std::int32_t>(nanos.count()));
  return ts;
}

userver::storages::postgres::TimePointTz
FromProtoTimestamp(const google::protobuf::Timestamp &ts) {
  const auto tp = std::chrono::system_clock::from_time_t(ts.seconds()) +
                  std::chrono::nanoseconds(ts.nanos());
  return userver::storages::postgres::TimePointTz{tp};
}

void FillProtoTask(aquarium::api::Task *task, const TaskDbInfo &taskInfo) {
  task->set_id(taskInfo.Id);
  task->set_title(taskInfo.Title);
  task->set_description(taskInfo.Description);
  task->set_authorid(taskInfo.AuthorId);
  for (const auto workerId : taskInfo.WorkerIds) {
    task->add_workerids(workerId);
  }
  if (taskInfo.CheckerId != 0) {
    task->set_checkerid(taskInfo.CheckerId);
  }
  task->set_status(static_cast<aquarium::api::TaskStatus>(taskInfo.status));

  *task->mutable_createdat() = ToProtoTimestamp(taskInfo.CreatedAt);
  *task->mutable_updatedat() = ToProtoTimestamp(taskInfo.UpdatedAt);
  if (taskInfo.DueDate.has_value()) {
    *task->mutable_duedate() = ToProtoTimestamp(*taskInfo.DueDate);
  }
}

void ValidateCreateTaskRequest(
    const aquarium::api::CreateTaskRequest &request) {
  if (request.title().empty()) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Task title cannot be empty"));
  }

  const std::int64_t authorId = request.authorid();
  if (authorId <= 0) {
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid author id"));
  }

  if (!aquarium::api::TaskStatus_IsValid(request.status()) ||
      request.status() == aquarium::api::TaskStatus::UNSPECIFIED) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Invalid task status"));
  }

  for (const auto id : request.workerids()) {
    if (id <= 0) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT, "Invalid worker id"));
    }
  }
}

void ValidateTaskId(std::int64_t id) {
  if (id <= 0) {
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid task id"));
  }
}

void ValidateWorkerId(std::int64_t workerId) {
  if (workerId <= 0) {
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid worker id"));
  }
}

void ValidateTaskStatus(aquarium::api::TaskStatus status) {
  if (!aquarium::api::TaskStatus_IsValid(status) ||
      status == aquarium::api::TaskStatus::UNSPECIFIED) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Invalid task status"));
  }
}

bool CheckTaskUpdatePermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo) {
  return AccessControl::CanUpdateTask(user, taskInfo);
}

bool CheckTaskStatusUpdatePermission(const Users::UserDbInfo *user,
                                     const TaskDbInfo &taskInfo,
                                     aquarium::api::TaskStatus targetStatus) {
  return AccessControl::CanUpdateTaskStatus(user, taskInfo, targetStatus);
}

bool CheckTaskWorkerPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo) {
  return AccessControl::CanModifyTaskWorkers(user, taskInfo);
}

bool CheckTaskDeletePermission(const Users::UserDbInfo *user,
                               std::int64_t authorId) {
  return AccessControl::CanDeleteTask(user, authorId);
}

std::vector<TaskDbInfo>
FilterTasks(const std::vector<TaskDbInfo> &tasks,
            std::optional<aquarium::api::TaskStatus> status,
            std::optional<std::int64_t> authorId,
            std::optional<std::int64_t> workerId) {
  std::vector<TaskDbInfo> matched;
  matched.reserve(tasks.size());
  for (const auto &taskInfo : tasks) {
    if (status.has_value() &&
        taskInfo.status != static_cast<std::int16_t>(*status)) {
      continue;
    }
    if (authorId.has_value() && taskInfo.AuthorId != *authorId) {
      continue;
    }
    if (workerId.has_value() &&
        std::ranges::find(taskInfo.WorkerIds, *workerId) ==
            taskInfo.WorkerIds.end()) {
      continue;
    }
    matched.push_back(taskInfo);
  }

  std::ranges::sort(matched, [](const auto &lhs, const auto &rhs) {
    return lhs.Id < rhs.Id;
  });

  return matched;
}

namespace {

const userver::storages::postgres::Query kSelectTaskFromView{
    "SELECT id, title, description, author_id, workers, checker_id, "
    "status, created_at, updated_at, due_date "
    "FROM task_schema.task_cache_view WHERE id = $1",
    userver::storages::postgres::Query::Name{"select_task_from_view"}};

} // namespace

TaskStoreService::TaskStoreService(
    const userver::components::ComponentConfig &cfg,
    const userver::components::ComponentContext &ctx)
    : TaskStoreService::Base(cfg, ctx),
      _pgCluster(ctx.FindComponent<userver::components::Postgres>("postgres-db")
                     .GetCluster()),
      _authCache(ctx.FindComponent<Users::AuthCache>()),
      _taskCache(ctx.FindComponent<TaskCache>()) {}

TaskStoreService::CreateTaskResult
TaskStoreService::CreateTask(CallContext &ctx,
                             aquarium::api::CreateTaskRequest &&request) {
  static const userver::storages::postgres::Query kInsertTask{
      "INSERT INTO task_schema.tasks (title, description, author_id, "
      "checker_id, status, due_date) "
      "VALUES ($1, $2, $3, $4, $5, $6) "
      "RETURNING id",
      userver::storages::postgres::Query::Name{"insert_task"}};

  static const userver::storages::postgres::Query kInsertTaskWorkers{
      "INSERT INTO task_schema.task_workers (task_id, worker_id) "
      "SELECT $1, UNNEST($2::BIGINT[]) "
      "ON CONFLICT DO NOTHING",
      userver::storages::postgres::Query::Name{"insert_task_workers"}};

  try {
    ValidateCreateTaskRequest(request);

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);
    if (!AccessControl::CanCreateTask(user, request.authorid())) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author or an admin can create this task"));
    }

    const std::int64_t authorId = request.authorid();
    std::string title = std::move(*request.mutable_title());
    std::string description = std::move(*request.mutable_description());

    std::vector<std::int64_t> workerIds;
    workerIds.reserve(request.workerids_size());
    for (const auto id : request.workerids()) {
      workerIds.push_back(id);
    }

    std::optional<std::int64_t> checkerId;
    if (request.has_checkerid() && request.checkerid() != 0) {
      checkerId = request.checkerid();
    }

    auto taskStatus = request.status();
    if (!checkerId.has_value() || *checkerId == 0) {
      if (taskStatus == aquarium::api::TaskStatus::IN_REVIEW) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "Task without a checker cannot be created in IN_REVIEW status"));
      }
      if (taskStatus == aquarium::api::TaskStatus::COMPLETE) {
        taskStatus = aquarium::api::TaskStatus::VERIFIED;
      }
    }
    const auto statusRaw = static_cast<std::int16_t>(taskStatus);

    std::optional<userver::storages::postgres::TimePointTz> dueDate;
    if (request.has_duedate()) {
      dueDate = FromProtoTimestamp(request.duedate());
    }

    auto transaction =
        _pgCluster->Begin("create_task_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto taskRes = transaction.Execute(kInsertTask, title, description,
                                       authorId, checkerId, statusRaw, dueDate);

    const auto taskId = taskRes.AsSingleRow<std::int64_t>();

    if (!workerIds.empty()) {
      transaction.Execute(kInsertTaskWorkers, taskId, workerIds);
    }

    auto createdRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto createdTaskInfo = createdRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();
    _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

    aquarium::api::CreateTaskResponse response;
    FillProtoTask(response.mutable_task(), createdTaskInfo);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const userver::storages::postgres::ForeignKeyViolation &ex) {
    LOG_WARNING() << "CreateTask foreign key violation: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::NOT_FOUND,
                     "Author, checker or worker user not found"));
  } catch (const std::exception &ex) {
    LOG_ERROR() << "CreateTask error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::GetTaskResult
TaskStoreService::GetTask(CallContext & /*ctx*/,
                          aquarium::api::GetTaskRequest &&request) {
  try {
    ValidateTaskId(request.id());
    const auto id = request.id();

    auto snapshot = _taskCache.Get();
    auto it = snapshot->find(id);

    if (it == snapshot->end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    aquarium::api::GetTaskResponse response;
    FillProtoTask(response.mutable_task(), it->second);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "GetTask error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::GetTasksResult
TaskStoreService::GetTasks(CallContext & /*ctx*/,
                           aquarium::api::GetTasksRequest &&request) {
  try {
    auto snapshot = _taskCache.Get();

    std::vector<TaskDbInfo> allTasks;
    allTasks.reserve(snapshot->size());
    for (const auto &[id, taskInfo] : *snapshot) {
      allTasks.push_back(taskInfo);
    }

    std::optional<aquarium::api::TaskStatus> statusFilter;
    if (request.has_status()) {
      statusFilter = request.status();
    }

    std::optional<std::int64_t> authorFilter;
    if (request.has_authorid()) {
      authorFilter = request.authorid();
    }

    std::optional<std::int64_t> workerFilter;
    if (request.has_workerid()) {
      workerFilter = request.workerid();
    }

    auto matchedTasks =
        FilterTasks(allTasks, statusFilter, authorFilter, workerFilter);

    aquarium::api::GetTasksResponse response;
    for (const auto &taskInfo : matchedTasks) {
      FillProtoTask(response.add_tasks(), taskInfo);
    }

    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "GetTasks error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::UpdateTaskResult
TaskStoreService::UpdateTask(CallContext &ctx,
                             aquarium::api::UpdateTaskRequest &&request) {
  static const userver::storages::postgres::Query kUpdateTask{
      "UPDATE task_schema.tasks "
      "SET title = CASE WHEN $2 THEN $3 ELSE title END, "
      "    description = CASE WHEN $4 THEN $5 ELSE description END, "
      "    checker_id = CASE WHEN $6 THEN $7 ELSE checker_id END, "
      "    due_date = CASE WHEN $8 THEN $9 ELSE due_date END "
      "WHERE id = $1",
      userver::storages::postgres::Query::Name{"update_task"}};

  try {
    ValidateTaskId(request.id());
    const auto taskId = request.id();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto transaction =
        _pgCluster->Begin("update_task_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto checkRes = transaction.Execute(kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckTaskUpdatePermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can update the task"));
    }

    const bool updateTitle = request.has_title();
    std::string newTitle =
        updateTitle ? std::move(*request.mutable_title()) : "";

    const bool updateDescription = request.has_description();
    std::string newDescription =
        updateDescription ? std::move(*request.mutable_description()) : "";

    const bool updateChecker = request.has_checkerid();
    std::optional<std::int64_t> newCheckerId;
    if (updateChecker && request.checkerid() != 0) {
      newCheckerId = request.checkerid();
    }

    if (updateChecker && !AccessControl::CanChangeChecker(user, taskInfo, newCheckerId)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only the author or an admin can change the checker"));
    }

    const bool updateDueDate = request.has_duedate();
    std::optional<userver::storages::postgres::TimePointTz> newDueDate;
    if (updateDueDate) {
      newDueDate = FromProtoTimestamp(request.duedate());
    }

    transaction.Execute(kUpdateTask, taskId, updateTitle, newTitle,
                        updateDescription, newDescription, updateChecker,
                        newCheckerId, updateDueDate, newDueDate);

    auto updatedRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto updatedTaskInfo = updatedRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();
    _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

    aquarium::api::UpdateTaskResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const userver::storages::postgres::ForeignKeyViolation &ex) {
    LOG_WARNING() << "UpdateTask foreign key error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::NOT_FOUND, "Referenced user not found"));
  } catch (const std::exception &ex) {
    LOG_ERROR() << "UpdateTask error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::UpdateTaskStatusResult TaskStoreService::UpdateTaskStatus(
    CallContext &ctx, aquarium::api::UpdateTaskStatusRequest &&request) {
  static const userver::storages::postgres::Query kUpdateTaskStatus{
      "UPDATE task_schema.tasks SET status = $2 WHERE id = $1",
      userver::storages::postgres::Query::Name{"update_task_status"}};

  try {
    ValidateTaskId(request.id());
    ValidateTaskStatus(request.status());
    const auto taskId = request.id();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto transaction =
        _pgCluster->Begin("update_task_status_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto checkRes = transaction.Execute(kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    auto targetStatus = request.status();
    if (taskInfo.CheckerId == 0) {
      if (targetStatus == aquarium::api::TaskStatus::IN_REVIEW) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "Task without a checker cannot be put in IN_REVIEW status"));
      }
      if (targetStatus == aquarium::api::TaskStatus::COMPLETE) {
        targetStatus = aquarium::api::TaskStatus::VERIFIED;
      }
    }

    if (!CheckTaskStatusUpdatePermission(user, taskInfo, targetStatus)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Permission denied to update task status"));
    }

    const auto statusRaw = static_cast<std::int16_t>(targetStatus);
    transaction.Execute(kUpdateTaskStatus, taskId, statusRaw);

    auto updatedRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto updatedTaskInfo = updatedRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();
    _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

    aquarium::api::UpdateTaskStatusResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "UpdateTaskStatus error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::AssignWorkerResult
TaskStoreService::AssignWorker(CallContext &ctx,
                               aquarium::api::AssignWorkerRequest &&request) {
  static const userver::storages::postgres::Query kInsertTaskWorker{
      "INSERT INTO task_schema.task_workers (task_id, worker_id) "
      "VALUES ($1, $2) "
      "ON CONFLICT (task_id, worker_id) DO NOTHING",
      userver::storages::postgres::Query::Name{"insert_task_worker"}};

  try {
    ValidateTaskId(request.id());
    ValidateWorkerId(request.workerid());
    const auto taskId = request.id();
    const auto workerId = request.workerid();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto transaction =
        _pgCluster->Begin("assign_worker_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto checkRes = transaction.Execute(kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckTaskWorkerPermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can assign workers"));
    }

    transaction.Execute(kInsertTaskWorker, taskId, workerId);

    auto updatedRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto updatedTaskInfo = updatedRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();
    _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

    aquarium::api::AssignWorkerResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const userver::storages::postgres::ForeignKeyViolation &ex) {
    LOG_WARNING() << "AssignWorker foreign key error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::NOT_FOUND, "Worker user not found"));
  } catch (const std::exception &ex) {
    LOG_ERROR() << "AssignWorker error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::RemoveWorkerResult
TaskStoreService::RemoveWorker(CallContext &ctx,
                               aquarium::api::RemoveWorkerRequest &&request) {
  static const userver::storages::postgres::Query kDeleteTaskWorker{
      "DELETE FROM task_schema.task_workers "
      "WHERE task_id = $1 AND worker_id = $2",
      userver::storages::postgres::Query::Name{"delete_task_worker"}};

  try {
    ValidateTaskId(request.id());
    ValidateWorkerId(request.workerid());
    const auto taskId = request.id();
    const auto workerId = request.workerid();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto transaction =
        _pgCluster->Begin("remove_worker_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto checkRes = transaction.Execute(kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckTaskWorkerPermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can remove workers"));
    }

    transaction.Execute(kDeleteTaskWorker, taskId, workerId);

    auto updatedRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto updatedTaskInfo = updatedRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();
    _taskCache.InvalidateAsync(userver::cache::UpdateType::kIncremental);

    aquarium::api::RemoveWorkerResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "RemoveWorker error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::DeleteTaskResult
TaskStoreService::DeleteTask(CallContext &ctx,
                             aquarium::api::DeleteTaskRequest &&request) {
  static const userver::storages::postgres::Query kSelectTaskAuthor{
      "SELECT author_id FROM task_schema.tasks WHERE id = $1",
      userver::storages::postgres::Query::Name{"select_task_author"}};

  static const userver::storages::postgres::Query kDeleteTask{
      "DELETE FROM task_schema.tasks WHERE id = $1",
      userver::storages::postgres::Query::Name{"delete_task"}};

  try {
    ValidateTaskId(request.id());
    const auto taskId = request.id();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);
    auto checkRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster,
        kSelectTaskAuthor, taskId);

    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto authorId = checkRes.AsSingleRow<std::int64_t>();
    if (!CheckTaskDeletePermission(user, authorId)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only the author or an admin can delete the task"));
    }

    _pgCluster->Execute(userver::storages::postgres::ClusterHostType::kMaster,
                        kDeleteTask, taskId);

    _taskCache.InvalidateAsync(userver::cache::UpdateType::kFull);

    aquarium::api::DeleteTaskResponse response;
    response.set_success(true);
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "DeleteTask error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

} // namespace Aquarium::Tasks
