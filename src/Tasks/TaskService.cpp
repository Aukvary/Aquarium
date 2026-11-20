#include "./TaskStore.hpp"
#include "../AccessControl.hpp"

#include <userver/engine/task/cancel.hpp>
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

void FillProtoComment(aquarium::api::Comment *comment,
                      const CommentDbInfo &commentInfo) {
  comment->set_id(commentInfo.Id);
  comment->set_taskid(commentInfo.TaskId);
  comment->set_authorid(commentInfo.AuthorId);
  comment->set_content(commentInfo.Content);
  comment->set_type(static_cast<aquarium::api::CommentType>(commentInfo.Type));
  *comment->mutable_createdat() = ToProtoTimestamp(commentInfo.CreatedAt);
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

void ValidateAddCommentRequest(
    const aquarium::api::AddCommentRequest &request) {
  ValidateTaskId(request.taskid());
  if (request.content().empty()) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Comment content cannot be empty"));
  }
  if (!aquarium::api::CommentType_IsValid(request.type())) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Invalid comment type"));
  }
}

void ValidateRejectTaskRequest(
    const aquarium::api::RejectTaskRequest &request) {
  ValidateTaskId(request.taskid());
  if (request.reason().empty()) {
    throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
        grpc::StatusCode::INVALID_ARGUMENT, "Rejection reason cannot be empty"));
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

bool CheckRejectTaskPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo) {
  return AccessControl::CanRejectTask(user, taskInfo);
}

bool CheckAddCommentPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo) {
  return AccessControl::CanAddComment(user, taskInfo);
}

bool CheckViewCommentsPermission(const Users::UserDbInfo *user,
                                 const TaskDbInfo &taskInfo) {
  return AccessControl::CanViewComments(user, taskInfo);
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

const userver::storages::postgres::Query kCountTasks{
    "SELECT COUNT(*)::BIGINT "
    "FROM task_schema.tasks t "
    "WHERE ($1::SMALLINT IS NULL OR t.status = $1) "
    "  AND ($2::BIGINT IS NULL OR t.author_id = $2) "
    "  AND ($3::BIGINT IS NULL OR EXISTS ( "
    "      SELECT 1 FROM task_schema.task_workers tw "
    "      WHERE tw.task_id = t.id AND tw.worker_id = $3 "
    "  ))",
    userver::storages::postgres::Query::Name{"count_tasks"}};

const userver::storages::postgres::Query kSelectTasks{
    "SELECT t.id, t.title, t.description, t.author_id, "
    "       COALESCE(ARRAY_AGG(tw.worker_id) FILTER (WHERE tw.worker_id IS NOT NULL), ARRAY[]::BIGINT[]) AS workers, "
    "       COALESCE(t.checker_id, 0) AS checker_id, "
    "       t.status, t.created_at, t.updated_at, t.due_date "
    "FROM ( "
    "    SELECT t.id, t.title, t.description, t.author_id, t.checker_id, "
    "           t.status, t.created_at, t.updated_at, t.due_date "
    "    FROM task_schema.tasks t "
    "    WHERE ($1::SMALLINT IS NULL OR t.status = $1) "
    "      AND ($2::BIGINT IS NULL OR t.author_id = $2) "
    "      AND ($3::BIGINT IS NULL OR EXISTS ( "
    "          SELECT 1 FROM task_schema.task_workers tw "
    "          WHERE tw.task_id = t.id AND tw.worker_id = $3 "
    "      )) "
    "    ORDER BY t.created_at DESC, t.id DESC "
    "    LIMIT $4 OFFSET $5 "
    ") t "
    "LEFT JOIN task_schema.task_workers tw ON t.id = tw.task_id "
    "GROUP BY t.id, t.title, t.description, t.author_id, t.checker_id, "
    "         t.status, t.created_at, t.updated_at, t.due_date "
    "ORDER BY t.created_at DESC, t.id DESC",
    userver::storages::postgres::Query::Name{"select_tasks"}};

} // namespace

TaskStoreService::TaskStoreService(
    const userver::components::ComponentConfig &cfg,
    const userver::components::ComponentContext &ctx)
    : TaskStoreService::Base(cfg, ctx),
      _pgCluster(ctx.FindComponent<userver::components::Postgres>("postgres-db")
                     .GetCluster()),
      _authCache(ctx.FindComponent<Users::AuthCache>()),
      _eventChannel("task_events") {}

void TaskStoreService::PublishEvent(aquarium::api::TaskEvent event) {
  try {
    _eventChannel.SendEvent(event);
  } catch (const std::exception &ex) {
    LOG_WARNING() << "Failed to publish task event: " << ex.what();
  }
}

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

    aquarium::api::CreateTaskResponse response;
    FillProtoTask(response.mutable_task(), createdTaskInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_CREATED);
    event.set_taskid(createdTaskInfo.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(createdTaskInfo.CreatedAt);
    PublishEvent(std::move(event));

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

    auto res = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kSlave,
        kSelectTaskFromView, id);

    if (res.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        res.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    aquarium::api::GetTaskResponse response;
    FillProtoTask(response.mutable_task(), taskInfo);
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
    std::optional<std::int16_t> statusFilter;
    if (request.has_status()) {
      ValidateTaskStatus(request.status());
      statusFilter = static_cast<std::int16_t>(request.status());
    }

    std::optional<std::int64_t> authorFilter;
    if (request.has_authorid()) {
      if (request.authorid() <= 0) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT, "Invalid author id"));
      }
      authorFilter = request.authorid();
    }

    std::optional<std::int64_t> workerFilter;
    if (request.has_workerid()) {
      ValidateWorkerId(request.workerid());
      workerFilter = request.workerid();
    }

    std::int32_t limit = request.limit();
    if (limit <= 0) {
      limit = 50;
    } else if (limit > 100) {
      limit = 100;
    }

    std::int32_t offset = request.offset();
    if (offset < 0) {
      offset = 0;
    }

    auto countRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kSlave,
        kCountTasks, statusFilter, authorFilter, workerFilter);
    const auto totalCount = countRes.AsSingleRow<std::int64_t>();

    aquarium::api::GetTasksResponse response;
    response.set_totalcount(totalCount);

    if (totalCount > 0 && offset < totalCount) {
      auto tasksRes = _pgCluster->Execute(
          userver::storages::postgres::ClusterHostType::kSlave,
          kSelectTasks, statusFilter, authorFilter, workerFilter,
          static_cast<std::int64_t>(limit), static_cast<std::int64_t>(offset));

      auto matchedTasks = tasksRes.AsContainer<std::vector<TaskDbInfo>>(
          userver::storages::postgres::kRowTag);

      for (const auto &taskInfo : matchedTasks) {
        FillProtoTask(response.add_tasks(), taskInfo);
      }
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

    aquarium::api::UpdateTaskResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_UPDATED);
    event.set_taskid(updatedTaskInfo.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(updatedTaskInfo.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::UpdateTaskStatusResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::STATUS_CHANGED);
    event.set_taskid(updatedTaskInfo.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(updatedTaskInfo.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::AssignWorkerResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::WORKER_ASSIGNED);
    event.set_taskid(updatedTaskInfo.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(updatedTaskInfo.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::RemoveWorkerResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::WORKER_REMOVED);
    event.set_taskid(updatedTaskInfo.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(updatedTaskInfo.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_DELETED);
    event.set_taskid(taskId);
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(
        userver::storages::postgres::TimePointTz{std::chrono::system_clock::now()});
    PublishEvent(std::move(event));

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

TaskStoreService::AddCommentResult
TaskStoreService::AddComment(CallContext &ctx,
                             aquarium::api::AddCommentRequest &&request) {
  static const userver::storages::postgres::Query kInsertComment{
      "INSERT INTO task_schema.task_comments (task_id, author_id, content, comment_type) "
      "VALUES ($1, $2, $3, $4) "
      "RETURNING id, task_id, author_id, content, comment_type, created_at",
      userver::storages::postgres::Query::Name{"insert_task_comment"}};

  try {
    ValidateAddCommentRequest(request);
    const auto taskId = request.taskid();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto checkRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster,
        kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckAddCommentPermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only task participants or an admin can add comments"));
    }

    std::int64_t authorId = user ? user->id : 0;
    if (request.has_authorid() && request.authorid() != 0) {
      if (user && request.authorid() != user->id && !user->admin) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::PERMISSION_DENIED,
            "Cannot add comment on behalf of another user"));
      }
      authorId = request.authorid();
    }

    auto commentRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster,
        kInsertComment, taskId, authorId, request.content(),
        static_cast<std::int16_t>(request.type()));

    auto commentInfo = commentRes.AsSingleRow<CommentDbInfo>(
        userver::storages::postgres::kRowTag);

    aquarium::api::AddCommentResponse response;
    FillProtoComment(response.mutable_comment(), commentInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::COMMENT_ADDED);
    event.set_taskid(commentInfo.TaskId);
    *event.mutable_comment() = response.comment();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(commentInfo.CreatedAt);
    PublishEvent(std::move(event));

    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const userver::storages::postgres::ForeignKeyViolation &ex) {
    LOG_WARNING() << "AddComment foreign key error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::NOT_FOUND, "Referenced entity not found"));
  } catch (const std::exception &ex) {
    LOG_ERROR() << "AddComment error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::GetCommentsResult
TaskStoreService::GetComments(CallContext &ctx,
                              aquarium::api::GetCommentsRequest &&request) {
  static const userver::storages::postgres::Query kSelectCommentsByTaskId{
      "SELECT id, task_id, author_id, content, comment_type, created_at "
      "FROM task_schema.task_comments "
      "WHERE task_id = $1 "
      "ORDER BY created_at ASC, id ASC",
      userver::storages::postgres::Query::Name{"select_comments_by_task_id"}};

  try {
    ValidateTaskId(request.taskid());
    const auto taskId = request.taskid();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto checkRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster,
        kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckViewCommentsPermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Access denied to task comments"));
    }

    auto commentsRes = _pgCluster->Execute(
        userver::storages::postgres::ClusterHostType::kMaster,
        kSelectCommentsByTaskId, taskId);

    auto comments = commentsRes.AsSetOf<CommentDbInfo>(
        userver::storages::postgres::kRowTag);

    aquarium::api::GetCommentsResponse response;
    for (const auto &commentInfo : comments) {
      FillProtoComment(response.add_comments(), commentInfo);
    }
    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "GetComments error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::RejectTaskResult
TaskStoreService::RejectTask(CallContext &ctx,
                             aquarium::api::RejectTaskRequest &&request) {
  static const userver::storages::postgres::Query kUpdateTaskStatus{
      "UPDATE task_schema.tasks SET status = $2 WHERE id = $1",
      userver::storages::postgres::Query::Name{"update_task_status_on_reject"}};

  static const userver::storages::postgres::Query kInsertComment{
      "INSERT INTO task_schema.task_comments (task_id, author_id, content, comment_type) "
      "VALUES ($1, $2, $3, $4) "
      "RETURNING id, task_id, author_id, content, comment_type, created_at",
      userver::storages::postgres::Query::Name{"insert_task_comment"}};

  try {
    ValidateRejectTaskRequest(request);
    const auto taskId = request.taskid();

    const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);

    auto transaction =
        _pgCluster->Begin("reject_task_trx",
                          userver::storages::postgres::ClusterHostType::kMaster,
                          userver::storages::postgres::TransactionOptions{});

    auto checkRes = transaction.Execute(kSelectTaskFromView, taskId);
    if (checkRes.IsEmpty()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto taskInfo =
        checkRes.AsSingleRow<TaskDbInfo>(userver::storages::postgres::kRowTag);

    if (!CheckRejectTaskPermission(user, taskInfo)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only the checker, author or an admin can reject the task"));
    }

    auto currentStatus = static_cast<aquarium::api::TaskStatus>(taskInfo.status);
    if (currentStatus != aquarium::api::TaskStatus::COMPLETE &&
        currentStatus != aquarium::api::TaskStatus::IN_REVIEW) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          "Task can only be rejected from COMPLETE or IN_REVIEW status"));
    }

    transaction.Execute(kUpdateTaskStatus, taskId,
                        static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE));

    const auto authorId = user ? user->id : 0;
    auto commentRes = transaction.Execute(
        kInsertComment, taskId, authorId, request.reason(),
        static_cast<std::int16_t>(aquarium::api::CommentType::COMMENT_REVISION_REASON));

    auto commentInfo = commentRes.AsSingleRow<CommentDbInfo>(
        userver::storages::postgres::kRowTag);

    auto updatedRes = transaction.Execute(kSelectTaskFromView, taskId);
    auto updatedTaskInfo = updatedRes.AsSingleRow<TaskDbInfo>(
        userver::storages::postgres::kRowTag);

    transaction.Commit();

    aquarium::api::RejectTaskResponse response;
    FillProtoTask(response.mutable_task(), updatedTaskInfo);
    FillProtoComment(response.mutable_comment(), commentInfo);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_REJECTED);
    event.set_taskid(updatedTaskInfo.Id);
    *event.mutable_task() = response.task();
    *event.mutable_comment() = response.comment();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = ToProtoTimestamp(commentInfo.CreatedAt);
    PublishEvent(std::move(event));

    return response;
  } catch (const userver::ugrpc::server::ErrorWithStatus &) {
    throw;
  } catch (const std::exception &ex) {
    LOG_ERROR() << "RejectTask error: " << ex.what();
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::INTERNAL, "Internal server error"));
  }
}

TaskStoreService::SubscribeEventsResult
TaskStoreService::SubscribeEvents(CallContext &ctx,
                                  aquarium::api::SubscribeEventsRequest && /*request*/,
                                  SubscribeEventsWriter &writer) {
  const auto *user = ctx.GetStorageContext().GetOptional(Users::kAuthUser);
  if (!user) {
    throw userver::ugrpc::server::ErrorWithStatus(
        grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required"));
  }

  using EventQueue =
      userver::concurrent::NonFifoMpscQueue<aquarium::api::TaskEvent>;
  auto queue = EventQueue::Create(1000);
  auto producer = queue->GetProducer();
  auto consumer = queue->GetConsumer();

  struct SubscriberListener {
    EventQueue::Producer producer;

    void OnEvent(const aquarium::api::TaskEvent &event) {
      (void)producer.PushNoblock(aquarium::api::TaskEvent(event));
    }
  };

  auto listener = std::make_unique<SubscriberListener>(std::move(producer));
  auto sub = _eventChannel.AddListener(listener.get(), "TaskEventsSubscriber",
                                       &SubscriberListener::OnEvent);

  while (!ctx.GetServerContext().IsCancelled() &&
         !userver::engine::current_task::ShouldCancel()) {
    aquarium::api::TaskEvent event;
    if (consumer.Pop(event, userver::engine::Deadline::FromDuration(
                                std::chrono::milliseconds(200)))) {
      try {
        writer.Write(std::move(event));
      } catch (const userver::ugrpc::server::RpcError &) {
        break;
      } catch (const std::exception &ex) {
        LOG_WARNING() << "Failed to write TaskEvent: " << ex.what();
        break;
      }
    }
  }

  sub.Unsubscribe();
  return grpc::Status::OK;
}

} // namespace Aquarium::Tasks
