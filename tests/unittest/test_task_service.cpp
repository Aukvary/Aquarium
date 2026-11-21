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

  void SetUser(const Aquarium::Users::UserDbInfo &user) {
    users_[user.id] = user;
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

    if (checker_id != 0) {
      auto it = users_.find(checker_id);
      if (it != users_.end() && !it->second.active) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "Cannot assign inactive user as checker"));
      }
    }

    for (const auto id : worker_ids) {
      auto it = users_.find(id);
      if (it != users_.end() && !it->second.active) {
        throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            "Cannot assign inactive user as worker"));
      }
    }

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

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_CREATED);
    event.set_taskid(task_info.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(task_info.CreatedAt);
    PublishEvent(std::move(event));

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
    if (request.has_status()) {
      Aquarium::Tasks::ValidateTaskStatus(request.status());
    }
    if (request.has_authorid() && request.authorid() <= 0) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT, "Invalid author id"));
    }
    if (request.has_workerid()) {
      Aquarium::Tasks::ValidateWorkerId(request.workerid());
    }

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

    aquarium::api::GetTasksResponse response;
    response.set_totalcount(static_cast<std::int64_t>(matched.size()));

    std::size_t start =
        std::min(static_cast<std::size_t>(offset), matched.size());
    std::size_t end =
        std::min(start + static_cast<std::size_t>(limit), matched.size());

    for (std::size_t i = start; i < end; ++i) {
      Aquarium::Tasks::FillProtoTask(response.add_tasks(), matched[i]);
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

    if (request.has_title()) {
      Aquarium::Tasks::ValidateTaskTitle(request.title());
      it->second.Title = request.title();
    }
    if (request.has_description()) {
      Aquarium::Tasks::ValidateTaskDescription(request.description());
      it->second.Description = request.description();
    }
    if (request.has_checkerid()) {
      it->second.CheckerId = request.checkerid();
      if (request.checkerid() != 0) {
        auto it_user = users_.find(request.checkerid());
        if (it_user != users_.end() && !it_user->second.active) {
          throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
              grpc::StatusCode::FAILED_PRECONDITION,
              "Cannot assign inactive user as checker"));
        }
      }
    }
    if (request.has_duedate()) {
      it->second.DueDate =
          Aquarium::Tasks::FromProtoTimestamp(request.duedate());
    }
    it->second.UpdatedAt = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};

    aquarium::api::UpdateTaskResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_UPDATED);
    event.set_taskid(it->second.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(it->second.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::STATUS_CHANGED);
    event.set_taskid(it->second.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(it->second.UpdatedAt);
    PublishEvent(std::move(event));

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

    auto it_user = users_.find(request.workerid());
    if (it_user != users_.end() && !it_user->second.active) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::FAILED_PRECONDITION,
          "Cannot assign inactive user as worker"));
    }

    auto &w = it->second.WorkerIds;
    if (std::find(w.begin(), w.end(), request.workerid()) == w.end()) {
      w.push_back(request.workerid());
    }

    aquarium::api::AssignWorkerResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::WORKER_ASSIGNED);
    event.set_taskid(it->second.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(it->second.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::WORKER_REMOVED);
    event.set_taskid(it->second.Id);
    *event.mutable_task() = response.task();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(it->second.UpdatedAt);
    PublishEvent(std::move(event));

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

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_DELETED);
    event.set_taskid(request.id());
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(
        userver::storages::postgres::TimePointTz{std::chrono::system_clock::now()});
    PublishEvent(std::move(event));

    aquarium::api::DeleteTaskResponse response;
    response.set_success(true);
    return response;
  }

  AddCommentResult
  AddComment(CallContext &ctx,
             aquarium::api::AddCommentRequest &&request) override {
    Aquarium::Tasks::ValidateAddCommentRequest(request);

    auto it = tasks_.find(request.taskid());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckAddCommentPermission(user, it->second)) {
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

    std::int64_t next_id = comments_.empty() ? 1 : comments_.rbegin()->first + 1;
    auto now = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};

    Aquarium::Tasks::CommentDbInfo comment_info{
        next_id,
        request.taskid(),
        authorId,
        request.content(),
        static_cast<std::int16_t>(request.type()),
        now};
    comments_[next_id] = comment_info;

    aquarium::api::AddCommentResponse response;
    Aquarium::Tasks::FillProtoComment(response.mutable_comment(), comment_info);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::COMMENT_ADDED);
    event.set_taskid(comment_info.TaskId);
    *event.mutable_comment() = response.comment();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(comment_info.CreatedAt);
    PublishEvent(std::move(event));

    return response;
  }

  GetCommentsResult
  GetComments(CallContext &ctx,
              aquarium::api::GetCommentsRequest &&request) override {
    Aquarium::Tasks::ValidateTaskId(request.taskid());

    auto it = tasks_.find(request.taskid());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckViewCommentsPermission(user, it->second)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Access denied to task comments"));
    }

    aquarium::api::GetCommentsResponse response;
    for (const auto &[id, comment] : comments_) {
      if (comment.TaskId == request.taskid()) {
        Aquarium::Tasks::FillProtoComment(response.add_comments(), comment);
      }
    }
    return response;
  }

  RejectTaskResult
  RejectTask(CallContext &ctx,
             aquarium::api::RejectTaskRequest &&request) override {
    Aquarium::Tasks::ValidateRejectTaskRequest(request);

    auto it = tasks_.find(request.taskid());
    if (it == tasks_.end()) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::NOT_FOUND, "Task not found"));
    }

    const auto *user =
        ctx.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
    if (!Aquarium::Tasks::CheckRejectTaskPermission(user, it->second)) {
      throw userver::ugrpc::server::ErrorWithStatus(
          grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                       "Only the checker, author or an admin can reject the task"));
    }

    auto currentStatus = static_cast<aquarium::api::TaskStatus>(it->second.status);
    if (currentStatus != aquarium::api::TaskStatus::COMPLETE &&
        currentStatus != aquarium::api::TaskStatus::IN_REVIEW) {
      throw userver::ugrpc::server::ErrorWithStatus(grpc::Status(
          grpc::StatusCode::INVALID_ARGUMENT,
          "Task can only be rejected from COMPLETE or IN_REVIEW status"));
    }

    it->second.status = static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE);
    auto now = userver::storages::postgres::TimePointTz{
        std::chrono::system_clock::now()};
    it->second.UpdatedAt = now;

    std::int64_t authorId = user ? user->id : 0;
    std::int64_t next_id = comments_.empty() ? 1 : comments_.rbegin()->first + 1;

    Aquarium::Tasks::CommentDbInfo comment_info{
        next_id,
        request.taskid(),
        authorId,
        request.reason(),
        static_cast<std::int16_t>(aquarium::api::CommentType::COMMENT_REVISION_REASON),
        now};
    comments_[next_id] = comment_info;

    aquarium::api::RejectTaskResponse response;
    Aquarium::Tasks::FillProtoTask(response.mutable_task(), it->second);
    Aquarium::Tasks::FillProtoComment(response.mutable_comment(), comment_info);

    aquarium::api::TaskEvent event;
    event.set_type(aquarium::api::TaskEvent::TASK_REJECTED);
    event.set_taskid(it->second.Id);
    *event.mutable_task() = response.task();
    *event.mutable_comment() = response.comment();
    event.set_actoruserid(user ? user->id : 0);
    *event.mutable_timestamp() = Aquarium::Tasks::ToProtoTimestamp(comment_info.CreatedAt);
    PublishEvent(std::move(event));

    return response;
  }

  void AddCommentDirect(const Aquarium::Tasks::CommentDbInfo &comment) {
    comments_[comment.Id] = comment;
  }

  void PublishEvent(aquarium::api::TaskEvent event) {
    try {
      event_channel_.SendEvent(event);
    } catch (...) {
    }
  }

  SubscribeEventsResult
  SubscribeEvents(CallContext &context,
                  aquarium::api::SubscribeEventsRequest && /*request*/,
                  SubscribeEventsWriter &writer) override {
    const auto *user =
        context.GetStorageContext().GetOptional(Aquarium::Users::kAuthUser);
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
    auto sub = event_channel_.AddListener(listener.get(), "TestTaskEventsSubscriber",
                                          &SubscriberListener::OnEvent);

    while (!context.GetServerContext().IsCancelled() &&
           !userver::engine::current_task::ShouldCancel()) {
      aquarium::api::TaskEvent event;
      if (consumer.Pop(event, userver::engine::Deadline::FromDuration(
                                  std::chrono::milliseconds(100)))) {
        try {
          writer.Write(std::move(event));
        } catch (...) {
          break;
        }
      }
    }

    sub.Unsubscribe();
    return grpc::Status::OK;
  }

private:
  std::map<std::int64_t, Aquarium::Tasks::TaskDbInfo> tasks_;
  std::map<std::int64_t, Aquarium::Tasks::CommentDbInfo> comments_;
  std::map<std::int64_t, Aquarium::Users::UserDbInfo> users_;
  userver::concurrent::AsyncEventChannel<const aquarium::api::TaskEvent &>
      event_channel_{"test_task_events"};
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
    Aquarium::Users::UserDbInfo inactive_user{
        6, "inactive-token", "InactiveUser", {}, false, false};

    token_to_user_[admin_user.key] = admin_user;
    token_to_user_[author_user.key] = author_user;
    token_to_user_[checker_user.key] = checker_user;
    token_to_user_[worker_user.key] = worker_user;
    token_to_user_[stranger_user.key] = stranger_user;
    token_to_user_[inactive_user.key] = inactive_user;

    service_.SetUser(admin_user);
    service_.SetUser(author_user);
    service_.SetUser(checker_user);
    service_.SetUser(worker_user);
    service_.SetUser(stranger_user);
    service_.SetUser(inactive_user);

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

UTEST(TaskTimestampTests, FillProtoComment_AllFields) {
  auto now = userver::storages::postgres::TimePointTz{
      std::chrono::system_clock::now()};
  Aquarium::Tasks::CommentDbInfo info{
      77,
      12,
      34,
      "Please revise this implementation",
      static_cast<std::int16_t>(aquarium::api::CommentType::COMMENT_REVISION_REASON),
      now};

  aquarium::api::Comment comment;
  Aquarium::Tasks::FillProtoComment(&comment, info);

  EXPECT_EQ(comment.id(), 77);
  EXPECT_EQ(comment.taskid(), 12);
  EXPECT_EQ(comment.authorid(), 34);
  EXPECT_EQ(comment.content(), "Please revise this implementation");
  EXPECT_EQ(comment.type(), aquarium::api::CommentType::COMMENT_REVISION_REASON);
  EXPECT_TRUE(comment.has_createdat());
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

UTEST(TaskValidationTests, ValidateAddCommentRequest_Checks) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("A valid comment");
  req.set_type(aquarium::api::CommentType::COMMENT_GENERAL);
  EXPECT_NO_THROW(Aquarium::Tasks::ValidateAddCommentRequest(req));

  // Invalid task ID
  req.set_taskid(0);
  UEXPECT_THROW(Aquarium::Tasks::ValidateAddCommentRequest(req),
                userver::ugrpc::server::ErrorWithStatus);

  // Empty content
  req.set_taskid(1);
  req.set_content("");
  UEXPECT_THROW(Aquarium::Tasks::ValidateAddCommentRequest(req),
                userver::ugrpc::server::ErrorWithStatus);

  // Invalid type
  req.set_content("content");
  req.set_type(static_cast<aquarium::api::CommentType>(999));
  UEXPECT_THROW(Aquarium::Tasks::ValidateAddCommentRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
}

UTEST(TaskValidationTests, ValidateRejectTaskRequest_Checks) {
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("Needs more tests");
  EXPECT_NO_THROW(Aquarium::Tasks::ValidateRejectTaskRequest(req));

  // Invalid task ID
  req.set_taskid(0);
  UEXPECT_THROW(Aquarium::Tasks::ValidateRejectTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);

  // Empty reason
  req.set_taskid(1);
  req.set_reason("");
  UEXPECT_THROW(Aquarium::Tasks::ValidateRejectTaskRequest(req),
                userver::ugrpc::server::ErrorWithStatus);
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
      Aquarium::Tasks::CheckTaskUpdatePermission(&author, taskNoChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&admin, taskNoChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&worker, taskNoChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskUpdatePermission(&stranger, taskNoChecker));
}

UTEST(TaskPermissionTests, ChangeCheckerPermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&author, taskNoChecker, 30));
  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&admin, taskNoChecker, 30));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(&worker, taskNoChecker, 30));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(&stranger, taskNoChecker, 30));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(nullptr, taskNoChecker, 30));

  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&author, taskNoChecker, 0));
  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&admin, taskNoChecker, 0));

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&author, taskWithChecker, 35));
  EXPECT_TRUE(
      Aquarium::AccessControl::CanChangeChecker(&admin, taskWithChecker, 35));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(&checker, taskWithChecker, 35));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(&worker, taskWithChecker, 35));
  EXPECT_FALSE(
      Aquarium::AccessControl::CanChangeChecker(&stranger, taskWithChecker, 35));
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

  // Without checker: author/admin can update status, worker can advance (e.g. ACTIVE),
  // but stranger has no rights and worker cannot VERIFIED/ARCHIVE/IN_REVIEW
  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &author, taskNoChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &admin, taskNoChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_TRUE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskNoChecker, aquarium::api::TaskStatus::ACTIVE));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskNoChecker, aquarium::api::TaskStatus::VERIFIED));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &worker, taskNoChecker, aquarium::api::TaskStatus::IN_REVIEW));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &stranger, taskNoChecker, aquarium::api::TaskStatus::ACTIVE));
  EXPECT_FALSE(Aquarium::Tasks::CheckTaskStatusUpdatePermission(
      &stranger, taskNoChecker, aquarium::api::TaskStatus::VERIFIED));
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

  // Without checker: only author and admin can assign/remove workers
  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&author, taskNoChecker));
  EXPECT_TRUE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&admin, taskNoChecker));
  EXPECT_FALSE(
      Aquarium::Tasks::CheckTaskWorkerPermission(&worker, taskNoChecker));
  EXPECT_FALSE(
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

UTEST(TaskPermissionTests, RejectPermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40};

  EXPECT_TRUE(Aquarium::Tasks::CheckRejectTaskPermission(&checker, taskWithChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckRejectTaskPermission(&author, taskWithChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckRejectTaskPermission(&admin, taskWithChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckRejectTaskPermission(&worker, taskWithChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckRejectTaskPermission(&stranger, taskWithChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckRejectTaskPermission(nullptr, taskWithChecker));

  Aquarium::Tasks::TaskDbInfo taskNoChecker{};
  taskNoChecker.AuthorId = 10;
  taskNoChecker.CheckerId = 0;
  taskNoChecker.WorkerIds = {40};

  EXPECT_TRUE(Aquarium::Tasks::CheckRejectTaskPermission(&author, taskNoChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckRejectTaskPermission(&admin, taskNoChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckRejectTaskPermission(&worker, taskNoChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckRejectTaskPermission(&stranger, taskNoChecker));
}

UTEST(TaskPermissionTests, AddCommentPermission) {
  Aquarium::Users::UserDbInfo author{10, "key", "Author", {}, true, false};
  Aquarium::Users::UserDbInfo admin{1, "key", "Admin", {}, true, true};
  Aquarium::Users::UserDbInfo checker{30, "key", "Checker", {}, true, false};
  Aquarium::Users::UserDbInfo worker{40, "key", "Worker", {}, true, false};
  Aquarium::Users::UserDbInfo stranger{20, "key", "Stranger", {}, true, false};

  Aquarium::Tasks::TaskDbInfo taskWithChecker{};
  taskWithChecker.AuthorId = 10;
  taskWithChecker.CheckerId = 30;
  taskWithChecker.WorkerIds = {40};

  EXPECT_TRUE(Aquarium::Tasks::CheckAddCommentPermission(&author, taskWithChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckAddCommentPermission(&checker, taskWithChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckAddCommentPermission(&worker, taskWithChecker));
  EXPECT_TRUE(Aquarium::Tasks::CheckAddCommentPermission(&admin, taskWithChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckAddCommentPermission(&stranger, taskWithChecker));
  EXPECT_FALSE(Aquarium::Tasks::CheckAddCommentPermission(nullptr, taskWithChecker));
}

UTEST(TaskPermissionTests, ViewCommentsPermission) {
  Aquarium::Users::UserDbInfo user{10, "key", "User", {}, true, false};
  Aquarium::Tasks::TaskDbInfo task{};
  EXPECT_TRUE(Aquarium::Tasks::CheckViewCommentsPermission(&user, task));
  EXPECT_FALSE(Aquarium::Tasks::CheckViewCommentsPermission(nullptr, task));
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
  EXPECT_EQ(resp.totalcount(), 1);
}

UTEST_F(TaskServiceGrpcFixture, GetTasks_Pagination_PagesAndTotalCount) {
  auto now = userver::storages::postgres::TimePointTz{
      std::chrono::system_clock::now()};

  // Seed tasks 2..25 (author: Alice (2), worker: Charlie (4), status: ACTIVE)
  for (std::int64_t i = 2; i <= 25; ++i) {
    service_.AddTaskDirect({i,
                            "Task " + std::to_string(i),
                            "Desc",
                            2,
                            {4},
                            3,
                            static_cast<std::int16_t>(aquarium::api::TaskStatus::ACTIVE),
                            now,
                            now,
                            std::nullopt});
  }
  // Total tasks: 1 (PLAN) + 24 (ACTIVE) = 25 tasks

  // 1. Page 1: limit 10, offset 0 (no filter)
  {
    aquarium::api::GetTasksRequest req;
    req.set_limit(10);
    req.set_offset(0);
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 25);
    ASSERT_EQ(resp.tasks_size(), 10);
    EXPECT_EQ(resp.tasks(0).id(), 1);
    EXPECT_EQ(resp.tasks(9).id(), 10);
  }

  // 2. Page 2: limit 10, offset 10
  {
    aquarium::api::GetTasksRequest req;
    req.set_limit(10);
    req.set_offset(10);
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 25);
    ASSERT_EQ(resp.tasks_size(), 10);
    EXPECT_EQ(resp.tasks(0).id(), 11);
    EXPECT_EQ(resp.tasks(9).id(), 20);
  }

  // 3. Page 3: limit 10, offset 20
  {
    aquarium::api::GetTasksRequest req;
    req.set_limit(10);
    req.set_offset(20);
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 25);
    ASSERT_EQ(resp.tasks_size(), 5);
    EXPECT_EQ(resp.tasks(0).id(), 21);
    EXPECT_EQ(resp.tasks(4).id(), 25);
  }

  // 4. Filter with pagination: status = ACTIVE (24 tasks total)
  {
    aquarium::api::GetTasksRequest req;
    req.set_status(aquarium::api::TaskStatus::ACTIVE);
    req.set_limit(10);
    req.set_offset(20);
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 24);
    ASSERT_EQ(resp.tasks_size(), 4);
    EXPECT_EQ(resp.tasks(0).id(), 22);
    EXPECT_EQ(resp.tasks(3).id(), 25);
  }
}

UTEST_F(TaskServiceGrpcFixture, GetTasks_Pagination_BeyondEnd) {
  aquarium::api::GetTasksRequest req;
  req.set_limit(10);
  req.set_offset(100);

  auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
  EXPECT_EQ(resp.totalcount(), 1);
  EXPECT_EQ(resp.tasks_size(), 0);
}

UTEST_F(TaskServiceGrpcFixture, GetTasks_LimitDefaultAndMax) {
  // Negative or 0 limit defaults to 50
  {
    aquarium::api::GetTasksRequest req;
    req.set_limit(0);
    req.set_offset(-5); // negative offset should be treated as 0
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 1);
    EXPECT_EQ(resp.tasks_size(), 1);
  }

  // Large limit (> 100) gets clamped to 100
  {
    aquarium::api::GetTasksRequest req;
    req.set_limit(500);
    auto resp = Client().GetTasks(req, MakeAuthOptions("author-token"));
    EXPECT_EQ(resp.totalcount(), 1);
    EXPECT_EQ(resp.tasks_size(), 1);
  }
}

UTEST_F(TaskServiceGrpcFixture, GetTasks_InvalidFilters_ThrowsInvalidArgument) {
  {
    aquarium::api::GetTasksRequest req;
    req.set_authorid(-1);
    UEXPECT_THROW(Client().GetTasks(req, MakeAuthOptions("author-token")),
                  userver::ugrpc::client::InvalidArgumentError);
  }
  {
    aquarium::api::GetTasksRequest req;
    req.set_workerid(0);
    UEXPECT_THROW(Client().GetTasks(req, MakeAuthOptions("author-token")),
                  userver::ugrpc::client::InvalidArgumentError);
  }
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
      Client().UpdateTaskStatus(update_req, MakeAuthOptions("author-token"));
  EXPECT_EQ(update_resp.task().status(), aquarium::api::TaskStatus::VERIFIED);
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTaskStatus_NoChecker_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("No Checker Task Stranger Denied");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::ACTIVE);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(task_id);
  update_req.set_status(aquarium::api::TaskStatus::ACTIVE);

  UEXPECT_THROW(
      Client().UpdateTaskStatus(update_req, MakeAuthOptions("stranger-token")),
      userver::ugrpc::client::PermissionDeniedError);
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
      Client().UpdateTaskStatus(update_req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AssignWorker_ByChecker_Succeeds) {
  aquarium::api::AssignWorkerRequest req;
  req.set_id(1);
  req.set_workerid(5);

  auto resp = Client().AssignWorker(req, MakeAuthOptions("checker-token"));
  EXPECT_THAT(resp.task().workerids(), testing::Contains(5));
}

UTEST_F(TaskServiceGrpcFixture,
        AssignWorker_NoChecker_ByStranger_ThrowsPermissionDenied) {
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

  UEXPECT_THROW(Client().AssignWorker(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, AssignWorker_NoChecker_ByAuthor_Succeeds) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("Author Assign Task");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::PLAN);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::AssignWorkerRequest req;
  req.set_id(task_id);
  req.set_workerid(5);

  auto resp = Client().AssignWorker(req, MakeAuthOptions("author-token"));
  EXPECT_THAT(resp.task().workerids(), testing::Contains(5));
}

UTEST_F(TaskServiceGrpcFixture,
        UpdateTask_NoChecker_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("Update Task Without Checker");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::PLAN);

  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::UpdateTaskRequest req;
  req.set_id(task_id);
  req.set_title("Stranger Hacked Title");

  UEXPECT_THROW(Client().UpdateTask(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
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

// ============================================================================
// TASK-1: Review & Comments Tests
// ============================================================================

UTEST_F(TaskServiceGrpcFixture, AddComment_WithoutAuth_ThrowsUnauthenticated) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("Unauthenticated comment");

  UEXPECT_THROW(Client().AddComment(req),
                userver::ugrpc::client::UnauthenticatedError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_EmptyContent_ThrowsInvalidArgument) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("");

  UEXPECT_THROW(Client().AddComment(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_InvalidTaskId_ThrowsInvalidArgument) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(0);
  req.set_content("Valid content");

  UEXPECT_THROW(Client().AddComment(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_NonExistentTask_ThrowsNotFound) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(999999);
  req.set_content("Valid content");

  UEXPECT_THROW(Client().AddComment(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::NotFoundError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_ByStranger_ThrowsPermissionDenied) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("Stranger trying to comment");

  UEXPECT_THROW(Client().AddComment(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture,
        AddComment_OnBehalfOfOther_ByNonAdmin_ThrowsPermissionDenied) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("Worker spoofing author");
  req.set_authorid(2); // Alice is author (2), caller is Charlie (4)

  UEXPECT_THROW(Client().AddComment(req, MakeAuthOptions("worker-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_ByParticipants_Succeeds) {
  // 1. Author comments
  aquarium::api::AddCommentRequest author_req;
  author_req.set_taskid(1);
  author_req.set_content("Author comment");
  auto author_resp =
      Client().AddComment(author_req, MakeAuthOptions("author-token"));
  EXPECT_EQ(author_resp.comment().content(), "Author comment");
  EXPECT_EQ(author_resp.comment().authorid(), 2);
  EXPECT_EQ(author_resp.comment().type(),
            aquarium::api::CommentType::COMMENT_GENERAL);

  // 2. Checker comments
  aquarium::api::AddCommentRequest checker_req;
  checker_req.set_taskid(1);
  checker_req.set_content("Checker comment");
  auto checker_resp =
      Client().AddComment(checker_req, MakeAuthOptions("checker-token"));
  EXPECT_EQ(checker_resp.comment().content(), "Checker comment");
  EXPECT_EQ(checker_resp.comment().authorid(), 3);

  // 3. Worker comments
  aquarium::api::AddCommentRequest worker_req;
  worker_req.set_taskid(1);
  worker_req.set_content("Worker comment");
  auto worker_resp =
      Client().AddComment(worker_req, MakeAuthOptions("worker-token"));
  EXPECT_EQ(worker_resp.comment().content(), "Worker comment");
  EXPECT_EQ(worker_resp.comment().authorid(), 4);

  // 4. Admin comments
  aquarium::api::AddCommentRequest admin_req;
  admin_req.set_taskid(1);
  admin_req.set_content("Admin comment");
  auto admin_resp =
      Client().AddComment(admin_req, MakeAuthOptions("admin-token"));
  EXPECT_EQ(admin_resp.comment().content(), "Admin comment");
  EXPECT_EQ(admin_resp.comment().authorid(), 1);

  // 5. GetComments returns all 4 comments
  aquarium::api::GetCommentsRequest get_req;
  get_req.set_taskid(1);
  auto get_resp =
      Client().GetComments(get_req, MakeAuthOptions("stranger-token"));
  EXPECT_EQ(get_resp.comments_size(), 4);
}

UTEST_F(TaskServiceGrpcFixture, GetComments_NonExistentTask_ThrowsNotFound) {
  aquarium::api::GetCommentsRequest req;
  req.set_taskid(999999);

  UEXPECT_THROW(Client().GetComments(req, MakeAuthOptions("author-token")),
                userver::ugrpc::client::NotFoundError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_WithoutAuth_ThrowsUnauthenticated) {
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("Needs rework");

  UEXPECT_THROW(Client().RejectTask(req),
                userver::ugrpc::client::UnauthenticatedError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_EmptyReason_ThrowsInvalidArgument) {
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("");

  UEXPECT_THROW(Client().RejectTask(req, MakeAuthOptions("checker-token")),
                userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture,
        RejectTask_InvalidStatus_ThrowsInvalidArgument) {
  // Task 1 starts in PLAN status. Rejecting must fail.
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("Invalid status rejection");

  UEXPECT_THROW(Client().RejectTask(req, MakeAuthOptions("checker-token")),
                userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ByWorker_ThrowsPermissionDenied) {
  // Put task 1 into COMPLETE status
  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(1);
  update_req.set_status(aquarium::api::TaskStatus::COMPLETE);
  Client().UpdateTaskStatus(update_req, MakeAuthOptions("worker-token"));

  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("Worker cannot reject");

  UEXPECT_THROW(Client().RejectTask(req, MakeAuthOptions("worker-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ByStranger_ThrowsPermissionDenied) {
  // Task 1 is updated to COMPLETE
  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(1);
  update_req.set_status(aquarium::api::TaskStatus::COMPLETE);
  Client().UpdateTaskStatus(update_req, MakeAuthOptions("author-token"));

  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("Stranger cannot reject");

  UEXPECT_THROW(Client().RejectTask(req, MakeAuthOptions("stranger-token")),
                userver::ugrpc::client::PermissionDeniedError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ByChecker_FromComplete_Succeeds) {
  // 1. Move task to COMPLETE
  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(1);
  update_req.set_status(aquarium::api::TaskStatus::COMPLETE);
  Client().UpdateTaskStatus(update_req, MakeAuthOptions("worker-token"));

  // 2. Checker rejects task with reason
  aquarium::api::RejectTaskRequest reject_req;
  reject_req.set_taskid(1);
  reject_req.set_reason("Unit tests are missing");

  auto reject_resp =
      Client().RejectTask(reject_req, MakeAuthOptions("checker-token"));

  // Status must be ACTIVE
  EXPECT_EQ(reject_resp.task().status(), aquarium::api::TaskStatus::ACTIVE);

  // Comment must have REVISION_REASON type and checker as author
  EXPECT_EQ(reject_resp.comment().content(), "Unit tests are missing");
  EXPECT_EQ(reject_resp.comment().authorid(), 3);
  EXPECT_EQ(reject_resp.comment().type(),
            aquarium::api::CommentType::COMMENT_REVISION_REASON);
  EXPECT_EQ(reject_resp.comment().taskid(), 1);

  // Verify task in service is indeed ACTIVE
  aquarium::api::GetTaskRequest get_req;
  get_req.set_id(1);
  auto get_resp = Client().GetTask(get_req, MakeAuthOptions("author-token"));
  EXPECT_EQ(get_resp.task().status(), aquarium::api::TaskStatus::ACTIVE);

  // Verify comments list includes the rejection comment
  aquarium::api::GetCommentsRequest comments_req;
  comments_req.set_taskid(1);
  auto comments_resp =
      Client().GetComments(comments_req, MakeAuthOptions("author-token"));
  ASSERT_EQ(comments_resp.comments_size(), 1);
  EXPECT_EQ(comments_resp.comments(0).content(), "Unit tests are missing");
  EXPECT_EQ(comments_resp.comments(0).type(),
            aquarium::api::CommentType::COMMENT_REVISION_REASON);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ByAuthor_FromInReview_Succeeds) {
  // 1. Move task to IN_REVIEW
  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(1);
  update_req.set_status(aquarium::api::TaskStatus::IN_REVIEW);
  Client().UpdateTaskStatus(update_req, MakeAuthOptions("checker-token"));

  // 2. Author rejects task
  aquarium::api::RejectTaskRequest reject_req;
  reject_req.set_taskid(1);
  reject_req.set_reason("Author noticed a bug");

  auto reject_resp =
      Client().RejectTask(reject_req, MakeAuthOptions("author-token"));
  EXPECT_EQ(reject_resp.task().status(), aquarium::api::TaskStatus::ACTIVE);
  EXPECT_EQ(reject_resp.comment().authorid(), 2);
  EXPECT_EQ(reject_resp.comment().type(),
            aquarium::api::CommentType::COMMENT_REVISION_REASON);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ByAdmin_Succeeds) {
  // 1. Move task to COMPLETE
  aquarium::api::UpdateTaskStatusRequest update_req;
  update_req.set_id(1);
  update_req.set_status(aquarium::api::TaskStatus::COMPLETE);
  Client().UpdateTaskStatus(update_req, MakeAuthOptions("author-token"));

  // 2. Admin rejects task
  aquarium::api::RejectTaskRequest reject_req;
  reject_req.set_taskid(1);
  reject_req.set_reason("Admin rejected");

  auto reject_resp =
      Client().RejectTask(reject_req, MakeAuthOptions("admin-token"));
  EXPECT_EQ(reject_resp.task().status(), aquarium::api::TaskStatus::ACTIVE);
  EXPECT_EQ(reject_resp.comment().authorid(), 1);
}

// ============================================================================
// Unit Tests: TASK-3 SubscribeEvents Real-time Streaming
// ============================================================================

UTEST_F(TaskServiceGrpcFixture, SubscribeEvents_Unauthenticated_Throws) {
  auto stream = Client().SubscribeEvents({}, {});
  aquarium::api::TaskEvent event;
  UEXPECT_THROW((void)stream.Read(event),
                userver::ugrpc::client::UnauthenticatedError);
}

UTEST_F(TaskServiceGrpcFixture, SubscribeEvents_ReceivesAllEventTypes) {
  auto stream =
      Client().SubscribeEvents({}, MakeAuthOptions("author-token"));

  // 1. CreateTask -> TASK_CREATED
  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("Stream Task");
  create_req.set_description("Testing streaming");
  create_req.set_authorid(2);
  create_req.set_checkerid(3);
  create_req.set_status(aquarium::api::TaskStatus::PLAN);
  auto create_resp =
      Client().CreateTask(create_req, MakeAuthOptions("author-token"));
  const auto task_id = create_resp.task().id();

  aquarium::api::TaskEvent event1;
  ASSERT_TRUE(stream.Read(event1));
  EXPECT_EQ(event1.type(), aquarium::api::TaskEvent::TASK_CREATED);
  EXPECT_EQ(event1.taskid(), task_id);
  EXPECT_EQ(event1.task().title(), "Stream Task");
  EXPECT_EQ(event1.actoruserid(), 2);

  // 2. UpdateTask -> TASK_UPDATED
  aquarium::api::UpdateTaskRequest update_req;
  update_req.set_id(task_id);
  update_req.set_title("Stream Task Updated");
  Client().UpdateTask(update_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event2;
  ASSERT_TRUE(stream.Read(event2));
  EXPECT_EQ(event2.type(), aquarium::api::TaskEvent::TASK_UPDATED);
  EXPECT_EQ(event2.taskid(), task_id);
  EXPECT_EQ(event2.task().title(), "Stream Task Updated");
  EXPECT_EQ(event2.actoruserid(), 2);

  // 3. UpdateTaskStatus -> STATUS_CHANGED
  aquarium::api::UpdateTaskStatusRequest status_req;
  status_req.set_id(task_id);
  status_req.set_status(aquarium::api::TaskStatus::ACTIVE);
  Client().UpdateTaskStatus(status_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event3;
  ASSERT_TRUE(stream.Read(event3));
  EXPECT_EQ(event3.type(), aquarium::api::TaskEvent::STATUS_CHANGED);
  EXPECT_EQ(event3.taskid(), task_id);
  EXPECT_EQ(event3.task().status(), aquarium::api::TaskStatus::ACTIVE);
  EXPECT_EQ(event3.actoruserid(), 2);

  // 4. AssignWorker -> WORKER_ASSIGNED
  aquarium::api::AssignWorkerRequest assign_req;
  assign_req.set_id(task_id);
  assign_req.set_workerid(4);
  Client().AssignWorker(assign_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event4;
  ASSERT_TRUE(stream.Read(event4));
  EXPECT_EQ(event4.type(), aquarium::api::TaskEvent::WORKER_ASSIGNED);
  EXPECT_EQ(event4.taskid(), task_id);
  ASSERT_EQ(event4.task().workerids_size(), 1);
  EXPECT_EQ(event4.task().workerids(0), 4);
  EXPECT_EQ(event4.actoruserid(), 2);

  // 5. RemoveWorker -> WORKER_REMOVED
  aquarium::api::RemoveWorkerRequest remove_req;
  remove_req.set_id(task_id);
  remove_req.set_workerid(4);
  Client().RemoveWorker(remove_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event5;
  ASSERT_TRUE(stream.Read(event5));
  EXPECT_EQ(event5.type(), aquarium::api::TaskEvent::WORKER_REMOVED);
  EXPECT_EQ(event5.taskid(), task_id);
  EXPECT_EQ(event5.task().workerids_size(), 0);
  EXPECT_EQ(event5.actoruserid(), 2);

  // 6. AddComment -> COMMENT_ADDED
  aquarium::api::AddCommentRequest comment_req;
  comment_req.set_taskid(task_id);
  comment_req.set_content("Looking good");
  comment_req.set_type(aquarium::api::CommentType::COMMENT_GENERAL);
  Client().AddComment(comment_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event6;
  ASSERT_TRUE(stream.Read(event6));
  EXPECT_EQ(event6.type(), aquarium::api::TaskEvent::COMMENT_ADDED);
  EXPECT_EQ(event6.taskid(), task_id);
  EXPECT_EQ(event6.comment().content(), "Looking good");
  EXPECT_EQ(event6.comment().type(),
            aquarium::api::CommentType::COMMENT_GENERAL);
  EXPECT_EQ(event6.actoruserid(), 2);

  // 7. Move to COMPLETE and RejectTask -> TASK_REJECTED
  aquarium::api::UpdateTaskStatusRequest complete_req;
  complete_req.set_id(task_id);
  complete_req.set_status(aquarium::api::TaskStatus::COMPLETE);
  Client().UpdateTaskStatus(complete_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event_status;
  ASSERT_TRUE(stream.Read(event_status));
  EXPECT_EQ(event_status.type(), aquarium::api::TaskEvent::STATUS_CHANGED);

  aquarium::api::RejectTaskRequest reject_req;
  reject_req.set_taskid(task_id);
  reject_req.set_reason("Needs more polish");
  Client().RejectTask(reject_req, MakeAuthOptions("admin-token"));

  aquarium::api::TaskEvent event7;
  ASSERT_TRUE(stream.Read(event7));
  EXPECT_EQ(event7.type(), aquarium::api::TaskEvent::TASK_REJECTED);
  EXPECT_EQ(event7.taskid(), task_id);
  EXPECT_EQ(event7.task().status(), aquarium::api::TaskStatus::ACTIVE);
  EXPECT_EQ(event7.comment().content(), "Needs more polish");
  EXPECT_EQ(event7.comment().type(),
            aquarium::api::CommentType::COMMENT_REVISION_REASON);
  EXPECT_EQ(event7.actoruserid(), 1);

  // 8. DeleteTask -> TASK_DELETED
  aquarium::api::DeleteTaskRequest delete_req;
  delete_req.set_id(task_id);
  Client().DeleteTask(delete_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event8;
  ASSERT_TRUE(stream.Read(event8));
  EXPECT_EQ(event8.type(), aquarium::api::TaskEvent::TASK_DELETED);
  EXPECT_EQ(event8.taskid(), task_id);
  EXPECT_EQ(event8.actoruserid(), 2);
}

UTEST_F(TaskServiceGrpcFixture, SubscribeEvents_MultipleSubscribers_ReceiveBroadcast) {
  auto stream1 =
      Client().SubscribeEvents({}, MakeAuthOptions("author-token"));
  auto stream2 =
      Client().SubscribeEvents({}, MakeAuthOptions("checker-token"));

  aquarium::api::CreateTaskRequest create_req;
  create_req.set_title("Broadcast Task");
  create_req.set_authorid(2);
  create_req.set_status(aquarium::api::TaskStatus::PLAN);
  auto resp = Client().CreateTask(create_req, MakeAuthOptions("author-token"));

  aquarium::api::TaskEvent event1;
  ASSERT_TRUE(stream1.Read(event1));
  EXPECT_EQ(event1.type(), aquarium::api::TaskEvent::TASK_CREATED);
  EXPECT_EQ(event1.taskid(), resp.task().id());
  EXPECT_EQ(event1.task().title(), "Broadcast Task");

  aquarium::api::TaskEvent event2;
  ASSERT_TRUE(stream2.Read(event2));
  EXPECT_EQ(event2.type(), aquarium::api::TaskEvent::TASK_CREATED);
  EXPECT_EQ(event2.taskid(), resp.task().id());
  EXPECT_EQ(event2.task().title(), "Broadcast Task");
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_InactiveChecker_ThrowsFailedPrecondition) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Task with Inactive Checker");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);
  req.set_checkerid(6); // User 6 is inactive

  UEXPECT_THROW(
      Client().CreateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::FailedPreconditionError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_InactiveWorker_ThrowsFailedPrecondition) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Task with Inactive Worker");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);
  req.add_workerids(6); // User 6 is inactive

  UEXPECT_THROW(
      Client().CreateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::FailedPreconditionError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_InactiveChecker_ThrowsFailedPrecondition) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_checkerid(6); // User 6 is inactive

  UEXPECT_THROW(
      Client().UpdateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::FailedPreconditionError);
}

UTEST_F(TaskServiceGrpcFixture, AssignWorker_InactiveWorker_ThrowsFailedPrecondition) {
  aquarium::api::AssignWorkerRequest req;
  req.set_id(1);
  req.set_workerid(6); // User 6 is inactive

  UEXPECT_THROW(
      Client().AssignWorker(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::FailedPreconditionError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_WhitespaceTitle_ThrowsInvalidArgument) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("   \t  \n ");
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(
      Client().CreateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_TitleExceeds255Chars_ThrowsInvalidArgument) {
  aquarium::api::CreateTaskRequest req;
  req.set_title(std::string(256, 't'));
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(
      Client().CreateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, CreateTask_DescriptionExceeds64KB_ThrowsInvalidArgument) {
  aquarium::api::CreateTaskRequest req;
  req.set_title("Valid Title");
  req.set_description(std::string(65537, 'd'));
  req.set_authorid(2);
  req.set_status(aquarium::api::TaskStatus::PLAN);

  UEXPECT_THROW(
      Client().CreateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_EmptyTitle_ThrowsInvalidArgument) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title("");

  UEXPECT_THROW(
      Client().UpdateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_WhitespaceTitle_ThrowsInvalidArgument) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title("   \t  ");

  UEXPECT_THROW(
      Client().UpdateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_TitleExceeds255Chars_ThrowsInvalidArgument) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_title(std::string(256, 'u'));

  UEXPECT_THROW(
      Client().UpdateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, UpdateTask_DescriptionExceeds64KB_ThrowsInvalidArgument) {
  aquarium::api::UpdateTaskRequest req;
  req.set_id(1);
  req.set_description(std::string(65537, 'x'));

  UEXPECT_THROW(
      Client().UpdateTask(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_WhitespaceContent_ThrowsInvalidArgument) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content("    \t  \n ");
  req.set_type(aquarium::api::CommentType::COMMENT_GENERAL);

  UEXPECT_THROW(
      Client().AddComment(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, AddComment_ContentExceeds16KB_ThrowsInvalidArgument) {
  aquarium::api::AddCommentRequest req;
  req.set_taskid(1);
  req.set_content(std::string(16385, 'c'));
  req.set_type(aquarium::api::CommentType::COMMENT_GENERAL);

  UEXPECT_THROW(
      Client().AddComment(req, MakeAuthOptions("author-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_WhitespaceReason_ThrowsInvalidArgument) {
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason("   \t  ");

  UEXPECT_THROW(
      Client().RejectTask(req, MakeAuthOptions("checker-token")),
      userver::ugrpc::client::InvalidArgumentError);
}

UTEST_F(TaskServiceGrpcFixture, RejectTask_ReasonExceeds16KB_ThrowsInvalidArgument) {
  aquarium::api::RejectTaskRequest req;
  req.set_taskid(1);
  req.set_reason(std::string(16385, 'r'));

  UEXPECT_THROW(
      Client().RejectTask(req, MakeAuthOptions("checker-token")),
      userver::ugrpc::client::InvalidArgumentError);
}
