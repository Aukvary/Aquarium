#include <gmock/gmock.h>
#include <userver/ugrpc/client/call_options.hpp>
#include <userver/ugrpc/client/exceptions.hpp>
#include <userver/ugrpc/server/exceptions.hpp>
#include <userver/ugrpc/tests/service_fixtures.hpp>
#include <userver/utest/utest.hpp>

#include <AccessControl.hpp>
#include <TaskStore.hpp>
#include <TaskStoreService.pb.h>
#include <TaskStoreService_client.usrv.pb.hpp>
#include <TaskStoreService_service.usrv.pb.hpp>
#include <Users.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

class TestTaskStoreService final : public aquarium::api::TaskStoreServiceBase {
public:
  void AddTaskDirect(const Aquarium::Tasks::TaskDbInfo &task) {
    tasks_[task.Id] = task;
  }

  CreateTaskResult
  CreateTask(CallContext &context,
             aquarium::api::CreateTaskRequest &&request) override {
    Aquarium::Tasks::ValidateCreateTaskRequest(request);

    const auto *user =
        context.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::AccessControl::CanCreateTask(user, request.authorid())) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author or an admin can create this task"));
    }

    std::int64_t next_id = tasks_.empty() ? 1 : tasks_.rbegin()->first + 1;
    auto now = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};

    std::vector<std::int64_t> worker_ids(request.workerids().begin(),
                                         request.workerids().end());
    std::int64_t checker_id = request.has_checkerid() ? request.checkerid() : 0;

    auto status = request.status();
    if (checker_id == 0) {
      if (status == aquarium::api::TaskStatus::IN_REVIEW) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "Task without a checker cannot be created in IN_REVIEW status"));
      }
      if (status == aquarium::api::TaskStatus::COMPLETE) {
        status = aquarium::api::TaskStatus::VERIFIED;
      }
    }

    std::optional<userver::storages::postgres::TimePointTz> due_date;
    if (request.has_duedate()) {
      due_date = Aquarium::Tasks::FromProtoTimestamp(request.duedate());
    }

    Aquarium::Tasks::TaskDbInfo task_info{next_id,
                                          request.title(),
                                          request.description(),
                                          request.authorid(),
                                          std::move(worker_ids),
                                          checker_id,
                                          static_cast<std::int16_t>(status),
                                          now,
                                          now,
                                          due_date};
    tasks_[next_id] = task_info;

    aquarium::api::CreateTaskResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), task_info);
    return response;
  }

  GetTaskResult GetTask(CallContext & /*context*/,
                        aquarium::api::GetTaskRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    aquarium::api::GetTaskResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    return response;
  }

  GetTasksResult GetTasks(CallContext & /*context*/,
                          aquarium::api::GetTasksRequest &&request) override {
    std::vector<Aquarium::Tasks::TaskDbInfo> all_tasks;
    all_tasks.reserve(tasks_.size());
    for (const auto &[id, task] : tasks_) {
      all_tasks.push_back(task);
    }

    std::optional<aquarium::api::TaskStatus> status;
    if (request.has_status())
      status = request.status();
    std::optional<std::int64_t> author_id;
    if (request.has_authorid())
      author_id = request.authorid();
    std::optional<std::int64_t> worker_id;
    if (request.has_workerid())
      worker_id = request.workerid();

    auto matched =
        Aquarium::Tasks::FilterTasks(all_tasks, status, author_id, worker_id);

    aquarium::api::GetTasksResponse response;
    for (const auto &task : matched) {
      Aquarium::Tasks::FillProtoTask(response.add_tasks(), task);
    }
    return response;
  }

  UpdateTaskResult
  UpdateTask(CallContext &ctx,
             aquarium::api::UpdateTaskRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckTaskUpdatePermission(user, it->second)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can update the task"));
    }

    if (request.has_title())
      it->second.Title = request.title();
    if (request.has_description())
      it->second.Description = request.description();
    if (request.has_checkerid())
      it->second.CheckerId = request.checkerid();
    if (request.has_duedate()) {
      it->second.DueDate =
          Aquarium::Tasks::FromProtoTimestamp(request.duedate());
    }
    it->second.UpdatedAt = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};

    aquarium::api::UpdateTaskResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    return response;
  }

  UpdateTaskStatusResult
  UpdateTaskStatus(CallContext &ctx,
                   aquarium::api::UpdateTaskStatusRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());
    Aquarium::Tasks::ValidateTaskStatus(request.status());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    auto targetStatus = request.status();
    if (it->second.CheckerId == 0) {
      if (targetStatus == aquarium::api::TaskStatus::IN_REVIEW) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "Task without a checker cannot be put in IN_REVIEW status"));
      }
      if (targetStatus == aquarium::api::TaskStatus::COMPLETE) {
        targetStatus = aquarium::api::TaskStatus::VERIFIED;
      }
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckTaskStatusUpdatePermission(user, it->second,
                                                          targetStatus)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Permission denied to update task status"));
    }

    it->second.status = static_cast<std::int16_t>(targetStatus);
    it->second.UpdatedAt = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};

    aquarium::api::UpdateTaskStatusResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    return response;
  }

  AssignWorkerResult
  AssignWorker(CallContext &ctx,
               aquarium::api::AssignWorkerRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());
    Aquarium::Tasks::ValidateWorkerId(request.workerid());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckTaskWorkerPermission(user, it->second)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can assign workers"));
    }

    auto &w = it->second.WorkerIds;
    if (std::find(w.begin(), w.end(), request.workerid()) == w.end()) {
      w.push_back(request.workerid());
    }

    aquarium::api::AssignWorkerResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    return response;
  }

  RemoveWorkerResult
  RemoveWorker(CallContext &ctx,
               aquarium::api::RemoveWorkerRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());
    Aquarium::Tasks::ValidateWorkerId(request.workerid());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckTaskWorkerPermission(user, it->second)) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::PERMISSION_DENIED,
          "Only the author, checker or an admin can remove workers"));
    }

    auto &w = it->second.WorkerIds;
    w.erase(std::remove(w.begin(), w.end(), request.workerid()), w.end());

    aquarium::api::RemoveWorkerResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    return response;
  }

  DeleteTaskResult
  DeleteTask(CallContext &ctx,
             aquarium::api::DeleteTaskRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.id());

    auto it = tasks_.find(request.id());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckTaskDeletePermission(user,
                                                    it->second.AuthorId)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only the author or an admin can delete the task"));
    }

    tasks_.erase(it);
    aquarium::api::DeleteTaskResponse response;
    response.set_success(true);
    return response;
  }

private:
  std::map<std::int64_t, Aquarium::Tasks::TaskDbInfo> tasks_;
};

class TaskServiceGrpcFixture
    : public userver::ugrpc::tests::ServiceFixtureBase {
protected:
  TaskServiceGrpcFixture() {
    // Users setup:
    // 1: Admin
    // 2: Author Alice
    // 3: Checker Bob
    // 4: Worker Charlie
    // 5: Stranger Dave
    Aquarium::Users::UserDbInfo admin_user{1,  "admin-token", "Admin",
                                           {}, true,          true};
    Aquarium::Users::UserDbInfo author_user{2,  "author-token", "Alice",
                                            {}, true,           false};
    Aquarium::Users::UserDbInfo checker_user{
        3, "checker-token", "Bob", {}, true, false};
    Aquarium::Users::UserDbInfo worker_user{4,  "worker-token", "Charlie",
                                            {}, true,           false};
    Aquarium::Users::UserDbInfo stranger_user{
        5, "stranger-token", "Dave", {}, true, false};

    token_to_user_[admin_user.key] = admin_user;
    token_to_user_[author_user.key] = author_user;
    token_to_user_[checker_user.key] = checker_user;
    token_to_user_[worker_user.key] = worker_user;
    token_to_user_[stranger_user.key] = stranger_user;

    // Seed initial task:
    // id: 1, author: 2 (Alice), checker: 3 (Bob), worker: {4} (Charlie),
    // status: PLAN
    auto now = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};
    Aquarium::Tasks::TaskDbInfo task1{
        1,
        "Initial Task",
        "Description",
        2,
        {4},
        3,
        static_cast<std::int16_t>(aquarium::api::TaskStatus::PLAN),
        now,
        now,
        std::nullopt};
    service_.AddTaskDirect(task1);

    auto middleware =
        std::make_shared<Aquarium::Users::AuthenticationMiddleware>(
            "task_invite_key",
            [this](std::string_view token)
                -> std::optional<Aquarium::Users::UserDbInfo> {
              auto it = token_to_user_.find(std::string(token));
              if (it != token_to_user_.end()) {
                return it->second;
              }
              return std::nullopt;
            },
            "task_internal_key");

    SetServerMiddlewares({std::move(middleware)});
    RegisterService(service_);
    StartServer();
    client_ = MakeClient<aquarium::api::TaskStoreServiceClient>();
  }

  ~TaskServiceGrpcFixture() override {
    client_.reset();
    StopServer();
  }

  aquarium::api::TaskStoreServiceClient &Client() { return *client_; }

  static userver::ugrpc::client::CallOptions
  MakeAuthOptions(const std::string &token) {
    userver::ugrpc::client::CallOptions opts;
    opts.AddMetadata("authorization", "Bearer " + token);
    return opts;
  }

protected:
  std::unordered_map<std::string, Aquarium::Users::UserDbInfo> token_to_user_;
  TestTaskStoreService service_;
  std::optional<aquarium::api::TaskStoreServiceClient> client_;
};

} // namespace

// ============================================================================
// Unit Tests: Timestamps & Proto Conversions
// ============================================================================

UTEST(TaskTimestampTests, RoundtripConversion) {
  const auto now = std::chrono::system_clock::now();
  const userver::storages::postgres::TimePointTz original{now};

  const auto proto_ts = Aquarium::Tasks::ToProtoTimestamp(original);
  const auto converted_back = Aquarium::Tasks::FromProtoTimestamp(proto_ts);

  const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
      original.GetUnderlying() - converted_back.GetUnderlying());
  EXPECT_EQ(diff.count(), 0);
}

UTEST(TaskTimestampTests, FillProtoTask_AllFields) {
  auto now = userver::storages::postgres::TimePointTz{
      std::chrono::system_clock::now()};
  Aquarium::Tasks::TaskDbInfo info{
      42,
      "Test Title",
      "Test Description",
      10,
      {20, 30},
      15,
      static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE),
      now,
      now,
      now};

  aquarium::api::Task task;
  Aquarium::Tasks::FillProtoTask(&task, info);

  EXPECT_EQ(task.id(), 42);
  EXPECT_EQ(task.title(), "Test Title");
  EXPECT_EQ(task.description(), "Test Description");
  EXPECT_EQ(task.authorid(), 10);
  ASSERT_EQ(task.workerids_size(), 2);
  EXPECT_EQ(task.workerids(0), 20);
  EXPECT_EQ(task.workerids(1), 30);
  EXPECT_TRUE(task.has_checkerid());
  EXPECT_EQ(task.checkerid(), 15);
  EXPECT_EQ(task.status(), aquarium::api::TaskStatus::ACTIVE);
  EXPECT_TRUE(task.has_createdat());
  EXPECT_TRUE(task.has_updatedat());
  EXPECT_TRUE(task.has_duedate());
}

UTEST(TaskTimestampTests, FillProtoTask_OptionalFieldsOmitted) {
  auto now = userver::storages::postgres::TimePointTz{
      std::chrono::system_clock::now()};
  Aquarium::Tasks::TaskDbInfo info{
      1,
      "Title",
      "Desc",
      5,
      {},
      0,
      static_cast<std::int16_t>(aquarium::api::TaskStatus::PLAN),
      now,
      now,
      std::nullopt};

  aquarium::api::Task task;
  Aquarium::Tasks::FillProtoTask(&task, info);

  EXPECT_FALSE(task.has_checkerid());
  EXPECT_FALSE(task.has_duedate());
  EXPECT_EQ(task.workerids_size(), 0);
}

// ============================================================================
// Unit Tests: Request Validation Helpers
// ============================================================================

UTEST(TaskValidationTests, ValidateCreateTaskRequest_EmptyTitle) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("");
  req.set_authorid(1);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(Aquarium::Tasks::ValidateCreateTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
}

UTEST(TaskValidationTests, ValidateCreateTaskRequest_InvalidAuthorId) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Title");
  req.set_authorid(0);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(Aquarium::Tasks::ValidateCreateTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
}

UTEST(TaskValidationTests, ValidateCreateTaskRequest_InvalidStatus) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Title");
  req.set_authorid(1);
  req.set_status(static_cast<aquarium::api::TaskStatus>(999));

  UEXPECT_THROW(Aquarium::Tasks::ValidateCreateTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
}

UTEST(TaskValidationTests, ValidateCreateTaskRequest_InvalidWorkerId) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Title");
  req.set_authorid(1);
  req.set_status(aquarium::api::TaskStatus::PLAN);
  req.add_workerids(10);
  req.add_workerids(-1); // invalid worker

  UEXPECT_THROW(Aquarium::Tasks::ValidateCreateTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
}

UTEST(TaskValidationTests, ValidateCreateTaskRequest_Valid) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Valid Title");
  req.set_authorid(1);
  req.set_status(aquarium::api::TaskStatus::ACTIVE);
  req.add_workerids(10);
  req.add_workerids(20);

  EXPECT_NO_THROW(Aquarium::Tasks::ValidateCreateTaskRequest(req));
}

UTEST(TaskValidationTests, ValidateTaskId_Checks) {
  UEXPECT_THROW(Aquarium::Tasks::ValidateTaskId(0),
                userver::ugrpc::server::ErrorWithStatus);
  UEXPECT_THROW(Aquarium::Tasks::ValidateTaskId(-10),
                userver::ugrpc::server::ErrorWithStatus);
  EXPECT_NO_THROW(Aquarium::Tasks::ValidateTaskId(1));
  EXPECT_NO_THROW(Aquarium::Tasks::ValidateTaskId(100));
}

UTEST(TaskValidationTests, ValidateWorkerId_Checks) {
  UEXPECT_THROW(Aquarium::Tasks::ValidateWorkerId(0),
                userver::ugrpc::server::ErrorWithStatus);
  UEXPECT_THROW(Aquarium::Tasks::ValidateWorkerId(-1),
                userver::ugrpc::server::ErrorWithStatus);
  EXPECT_NO_THROW(Aquarium::Tasks::ValidateWorkerId(10));
}

UTEST(TaskValidationTests, ValidateTaskStatus_Checks) {
  UEXPECT_THROW(Aquarium::Tasks::ValidateTaskStatus(
                    static_cast<aquarium::api::TaskStatus>(999)),
                userver::ugrpc::server::ErrorWithStatus);
  EXPECT_NO_THROW(
      Aquarium::Tasks::ValidateTaskStatus(aquarium::api::TaskStatus::PLAN));
  EXPECT_NO_THROW(
      Aquarium::Tasks::ValidateTaskStatus(aquarium::api::TaskStatus::COMPLETE));
}

// ============================================================================
// Unit Tests: Permissions
// ============================================================================

UTEST(TaskPermissionTests, UpdatePermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&author, taskWithChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&admin, taskWithChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&checker, taskWithChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&worker, taskWithChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&stranger, taskWithChecker));

  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&stranger, taskNoChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&worker, taskNoChecker));
}

UTEST(TaskPermissionTests, StatusUpdatePermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{50, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40, 41};

  // With checker: author and checker have full rights
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &author, taskWithChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &checker, taskWithChecker, aquarium::api::TaskStatus::IN_REVIEW));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &checker, taskWithChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &admin, taskWithChecker, aquarium::api::TaskStatus::VERIFIED));

  // With checker: worker can update to COMPLETE/ACTIVE/PLAN, but NOT
  // IN_REVIEW/VERIFIED/ARCHIVE
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskWithChecker, aquarium::api::TaskStatus::COMPLETE));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskWithChecker, aquarium::api::TaskStatus::ACTIVE));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskWithChecker, aquarium::api::TaskStatus::IN_REVIEW));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskWithChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskWithChecker, aquarium::api::TaskStatus::ARCHIVE));

  // With checker: stranger has no rights
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &stranger, taskWithChecker, aquarium::api::TaskStatus::COMPLETE));

  // Without checker: anyone can update status
  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &stranger, taskNoChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskNoChecker, aquarium::api::TaskStatus::ACTIVE));
}

UTEST(TaskPermissionTests, WorkerPermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{50, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40};

  // With checker: only author, checker and admin can assign/remove workers
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&author, taskWithChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&admin, taskWithChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&checker, taskWithChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&worker, taskWithChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&stranger, taskWithChecker));

  // Without checker: anyone can assign/remove workers
  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&worker, taskNoChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&stranger, taskNoChecker));
}

UTEST(TaskPermissionTests, DeletePermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  EXPECT_TRUE(Aquarium::Tasks::CheckTaskDeletePermission(&author, 10));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskDeletePermission(&admin, 10));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskDeletePermission(&stranger, 10));
}

UTEST(TaskPermissionTests, CreatePermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  EXPECT_TRUE(Aquarium::AccessControl::CanCreateTask(&author, 10));
  EXPECT_TRUE(Aquarium::AccessControl::CanCreateTask(&admin, 10));
  EXPECT_FALSE(Aquarium::AccessControl::CanCreateTask(&stranger, 10));
  EXPECT_FALSE(Aquarium::AccessControl::CanCreateTask(nullptr, 10));
}

// ============================================================================
// Unit Tests: Filtering & Sorting
// ============================================================================

UTEST(TaskFilterTests, FilterTasks_Combinations) {
  auto now = userver::storages::postgres::TimePointTz{
      std::chrono::system_clock::now()};
  std::vector<Aquarium::Tasks::TaskDbInfo> tasks{
      {1,
       "T1",
       "",
       10,
       {20, 21},
       0,
       static_cast<std::int16_t>(aquarium::api::TaskStatus::PLAN),
       now,
       now,
       std::nullopt},
      {2,
       "T2",
       "",
       10,
       {21},
       0,
       static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE),
       now,
       now,
       std::nullopt},
      {3,
       "T3",
       "",
       11,
       {22},
       0,
       static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE),
       now,
       now,
       std::nullopt},
      {4,
       "T4",
       "",
       11,
       {20},
       0,
       static_cast<std::int16_t>(aquarium::api::TaskStatus::COMPLETE),
       now,
       now,
       std::nullopt},
  };

  // Filter by status ACTIVE -> T2, T3
  auto res_active = Aquarium::Tasks::FilterTasks(
      tasks, aquarium::api::TaskStatus::ACTIVE, std::nullopt, std::nullopt);
  ASSERT_EQ(res_active.size(), 2);
  EXPECT_EQ(res_active[0].Id, 2);
  EXPECT_EQ(res_active[1].Id, 3);

  // Filter by author 10 -> T1, T2
  auto res_author10 =
      Aquarium::Tasks::FilterTasks(tasks, std::nullopt, 10, std::nullopt);
  ASSERT_EQ(res_author10.size(), 2);
  EXPECT_EQ(res_author10[0].Id, 1);
  EXPECT_EQ(res_author10[1].Id, 2);

  // Filter by worker 20 -> T1, T4
  auto res_worker20 =
      Aquarium::Tasks::FilterTasks(tasks, std::nullopt, std::nullopt, 20);
  ASSERT_EQ(res_worker20.size(), 2);
  EXPECT_EQ(res_worker20[0].Id, 1);
  EXPECT_EQ(res_worker20[1].Id, 4);

  // Filter by status ACTIVE and author 10 -> T2
  auto res_combined = Aquarium::Tasks::FilterTasks(
      tasks, aquarium::api::TaskStatus::ACTIVE, 10, std::nullopt);
  ASSERT_EQ(res_combined.size(), 1);
  EXPECT_EQ(res_combined[0].Id, 2);

  // Filter with no match -> empty
  auto res_empty = Aquarium::Tasks::FilterTasks(
      tasks, aquarium::api::TaskStatus::ARCHIVE, std::nullopt, std::nullopt);
  EXPECT_TRUE(res_empty.empty());
}

// ============================================================================
// gRPC In-Process Integration Tests
// ============================================================================

UTEST_F(TaskServiceGrpcFixture, CreateTask_WithoutAuth_ThrowsUnauthenticated) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Test");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);
  UEXPECT_THROW(Client().CreateTask(req, {}),
                userver::ugrpc::client::UnauthenticatedError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_EmptyTitle_ThrowsInvalidArgument) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);
  UEXPECT_THROW(Client().CreateTask(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_Valid_Succeeds) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("New Task");
  req.set_description("Some description");
  req.set_authorid(2);
  req.set_checkerid(3);
  req.add_workerids(4);
  req.set_status(aquarium::api::TaskStatus::ACTIVE);

  auto resp = Client().CreateTask(req, MakeAuthOptions("author-token"));
  EXPECT_EQ(resp.task().title(), "New Task");
  EXPECT_EQ(resp.task().authorid(), 2);
  EXPECT_EQ(resp.task().checkerid(), 3);
  ASSERT_EQ(resp.task().workerids_size(), 1);
  EXPECT_EQ(resp.task().workerids(0), 4);
  EXPECT_EQ(resp.task().status(), aquarium::api::TaskStatus::ACTIVE);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Hacked Task");
  req.set_description("Stranger tries to create task for author");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(Client().CreateTask(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, GetTask_Existing_Succeeds) {
  aquarium::api::GetTaskRequest req;
  req.set_id(1);

  auto resp = Client().GetTask(req, MakeAuthOptions("author-token"));
  EXPECT_EQ(resp.task().id(), 1);
  EXPECT_EQ(resp.task().title(), "Initial Task");
}

UTEST_F(TaskServiceGrpcFixture, GetTask_NonExistent_ThrowsNotFound) {
  aquarium::api::GetTaskRequest req;
  req.set_id(999);

  UEXPECT_THROW(Client().GetTask(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::NotFoundError);
}

UTEST_F(TaskServiceGrpcFixture, GetTasks_FilterByStatus_Succeeds) {
  aquarium::api::GetTasksRequest req;
  req.set_status(aquarium::api::TaskStatus::PLAN);

  auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
  ASSERT_EQ(resp.tasks_size(), 1);
  EXPECT_EQ(resp.tasks(0).id(), 1);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_ByAuthor_Succeeds) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title("Updated Title By Author");

  auto resp = Client().UpdateTask(req, MakeAuthOptions("author-token"));
  EXPECT_EQ(resp.task().title(), "Updated Title By Author");
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title("Hacked Title");

  UEXPECT_THROW(Client().UpdateTask(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_ByAdmin_Succeeds) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title("Admin Overwrite");

  auto resp = Client().UpdateTask(req, MakeAuthOptions("admin-token"));
  EXPECT_EQ(resp.task().title(), "Admin Overwrite");
}

UTEST_F(TaskServiceGrpcFixture, UpdateTaskStatus_ByWorker_Succeeds) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::ACTIVE);

  auto resp = Client().UpdateTaskStatus(req, MakeAuthOptions("worker-token"));
  EXPECT_EQ(resp.task().status(), aquarium::api::TaskStatus::ACTIVE);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTaskStatus_ByChecker_Succeeds) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::COMPLETE);

  auto resp = Client().UpdateTaskStatus(req, MakeAuthOptions("checker-token"));
  EXPECT_EQ(resp.task().status(), aquarium::api::TaskStatus::COMPLETE);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::ARCHIVE);

  UEXPECT_THROW(
      Client().UpdateTaskStatus(req, MakeAuthOptions("stranger-token")),
      userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, AssignAndRemoveWorker_ByAuthor_Succeeds) {
  // Assign worker 5
  aquarium::api::AssignWorkerRequest assign_req;
  assign_req.set_id(1);
  assign_req.set_workerid(5);

  auto assign_resp =
      Client().AssignWorker(assign_req, MakeAuthOptions("author-token"));
  EXPECT_THAT(assign_resp.task().workerids(), testing::Contains(5));

  // Remove worker 5
  aquarium::api::RemoveWorkerRequest remove_req;
  remove_req.set_id(1);
  remove_req.set_workerid(5);

  auto remove_resp =
      Client().RemoveWorker(remove_req, MakeAuthOptions("author-token"));
  EXPECT_THAT(remove_resp.task().workerids(),
              testing::Not(testing::Contains(5)));
}

UTEST_F(TaskServiceGrpcFixture,
        AssignWorker_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::AssignWorkerRequest req;
  req.set_id(1);
  req.set_workerid(5);

  UEXPECT_THROW(Client().AssignWorker(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_ByWorker_ToInReview_ThrowsPermissionDenied) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::IN_REVIEW);

  UEXPECT_THROW(Client().UpdateTaskStatus(req, MakeAuthOptions("worker-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_ByWorker_ToVerified_ThrowsPermissionDenied) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::VERIFIED);

  UEXPECT_THROW(Client().UpdateTaskStatus(req, MakeAuthOptions("worker-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_ByChecker_ToInReview_Succeeds) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::IN_REVIEW);

  auto resp = Client().UpdateTaskStatus(req, MakeAuthOptions("checker-token"));
  EXPECT_EQ(resp.task().status(), aquarium::api::TaskStatus::IN_REVIEW);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_ByChecker_ToVerified_Succeeds) {
  aquarium::api::UpdateTaskStatusRequest req;
  req.set_id(1);
  req.set_status(aquarium::api::TaskStatus::VERIFIED);

  auto resp = Client().UpdateTaskStatus(req, MakeAuthOptions("checker-token"));
  EXPECT_EQ(resp.task().status(), aquarium::api::TaskStatus::VERIFIED);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_NoChecker_CompleteMapsToVerified) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("No Checker Task");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::ACTIVE);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(task_id);
  update_req.set_status(aquarium::api::TaskStatus::COMPLETE);

  auto update_resp =
      Client().UpdateTaskStatus(update_req, MakeAuthOptions("stranger-token"));
  EXPECT_EQ(update_resp.task().status(), aquarium::api::TaskStatus::VERIFIED);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_NoChecker_InReviewThrowsInvalidArgument) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("No Checker Task 2");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::ACTIVE);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(task_id);
  update_req.set_status(aquarium::api::TaskStatus::IN_REVIEW);

  UEXPECT_THROW(
      Client().UpdateTaskStatus(update_req, MakeAuthOptions("stranger-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AssignWorker_ByChecker_Succeeds) {
  aquarium::api::AssignWorkerRequest req;
  req.set_id(1);
  req.set_workerid(5);

  auto resp = Client().AssignWorker(req, MakeAuthOptions("checker-token"));
  EXPECT_THAT(resp.task().workerids(), testing::Contains(5));
}

UTEST_F(TaskServiceGrpcFixture, AssignWorker_NoChecker_ByStranger_Succeeds) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("Self Assign Task");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::PLAN);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::AssignWorkerRequest req;
  req.set_id(task_id);
  req.set_workerid(5);

  auto resp = Client().AssignWorker(req, MakeAuthOptions("stranger-token"));
  EXPECT_THAT(resp.task().workerids(), testing::Contains(5));
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_ByChecker_Succeeds) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_description("Checker comment: needs revision");

  auto resp = Client().UpdateTask(req, MakeAuthOptions("checker-token"));
  EXPECT_EQ(resp.task().description(), "Checker comment: needs revision");
}

UTEST_F(TaskServiceGrpcFixture, DeleteTask_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::DeleteTaskRequest req;
  req.set_id(1);

  UEXPECT_THROW(Client().DeleteTask(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, DeleteTask_ByAuthor_Succeeds) {
  aquarium::api::DeleteTaskRequest req;
  req.set_id(1);

  auto resp = Client().DeleteTask(req, MakeAuthOptions("author-token"));
  EXPECT_TRUE(resp.success());

  // Task 1 should no longer exist
  aquarium::api::GetTaskRequest get_req;
  get_req.set_id(1);
  UEXPECT_THROW(Client().GetTask(get_req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::NotFoundError);
}
