#pragma once

#include "./Users.hpp"
#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/concurrent/async_event_channel.hpp>
#include <userver/concurrent/queue.hpp>
#include <userver/storages/postgres/postgres_fwd.hpp>

#include <TaskStoreService.grpc.pb.h>
#include <TaskStoreService.pb.h>
#include <TaskStoreService_service.usrv.pb.hpp>
#include <UserStoreService_service.usrv.pb.hpp>
#include <cstdint>
#include <google/protobuf/timestamp.pb.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aquarium::Tasks {
struct TaskDbInfo {
  std::int64_t Id;
  std::string Title;
  std::string Description;
  std::int64_t AuthorId;
  std::vector<std::int64_t> WorkerIds;
  std::int64_t CheckerId;
  std::int16_t status;
  userver::storages::postgres::TimePointTz CreatedAt;
  userver::storages::postgres::TimePointTz UpdatedAt;
  std::optional<userver::storages::postgres::TimePointTz> DueDate;
};

struct CommentDbInfo {
  std::int64_t Id;
  std::int64_t TaskId;
  std::int64_t AuthorId;
  std::string Content;
  std::int16_t Type;
  userver::storages::postgres::TimePointTz CreatedAt;
};

class TaskStoreService final
    : public aquarium::api::TaskStoreServiceBase::Component {
  using Base = aquarium::api::TaskStoreServiceBase::Component;

private:
  userver::storages::postgres::ClusterPtr _pgCluster;
  Users::AuthCache &_authCache;
  userver::concurrent::AsyncEventChannel<const aquarium::api::TaskEvent &>
      _eventChannel;

public:
  static constexpr std::string_view kName = "task-store-service";

  TaskStoreService(const userver::components::ComponentConfig &cfg,
                   const userver::components::ComponentContext &ctx);

  void PublishEvent(aquarium::api::TaskEvent event);

  CreateTaskResult
  CreateTask(CallContext &ctx,
             aquarium::api::CreateTaskRequest &&request) override;

  GetTaskResult GetTask(CallContext &ctx,
                        aquarium::api::GetTaskRequest &&request) override;

  GetTasksResult GetTasks(CallContext &ctx,
                          aquarium::api::GetTasksRequest &&request) override;

  UpdateTaskResult
  UpdateTask(CallContext &ctx,
             aquarium::api::UpdateTaskRequest &&request) override;

  UpdateTaskStatusResult
  UpdateTaskStatus(CallContext &ctx,
                   aquarium::api::UpdateTaskStatusRequest &&request) override;

  AssignWorkerResult
  AssignWorker(CallContext &ctx,
               aquarium::api::AssignWorkerRequest &&request) override;

  RemoveWorkerResult
  RemoveWorker(CallContext &ctx,
               aquarium::api::RemoveWorkerRequest &&request) override;

  DeleteTaskResult
  DeleteTask(CallContext &ctx,
             aquarium::api::DeleteTaskRequest &&request) override;

  AddCommentResult
  AddComment(CallContext &ctx,
             aquarium::api::AddCommentRequest &&request) override;

  GetCommentsResult
  GetComments(CallContext &ctx,
              aquarium::api::GetCommentsRequest &&request) override;

  RejectTaskResult
  RejectTask(CallContext &ctx,
             aquarium::api::RejectTaskRequest &&request) override;

  SubscribeEventsResult
  SubscribeEvents(CallContext &ctx,
                  aquarium::api::SubscribeEventsRequest &&request,
                  SubscribeEventsWriter &writer) override;
};

google::protobuf::Timestamp
ToProtoTimestamp(const userver::storages::postgres::TimePointTz &tptz);

userver::storages::postgres::TimePointTz
FromProtoTimestamp(const google::protobuf::Timestamp &ts);

void FillProtoTask(aquarium::api::Task *task, const TaskDbInfo &taskInfo);
void FillProtoComment(aquarium::api::Comment *comment,
                      const CommentDbInfo &commentInfo);

constexpr std::size_t kMaxTaskTitleLength = 255;
constexpr std::size_t kMaxTaskDescriptionLength = 65536;
constexpr std::size_t kMaxCommentContentLength = 16384;

bool IsBlank(std::string_view str) noexcept;
void ValidateTaskTitle(std::string_view title);
void ValidateTaskDescription(std::string_view description);
void ValidateCommentContent(std::string_view content);

void ValidateCreateTaskRequest(const aquarium::api::CreateTaskRequest &request);
void ValidateTaskId(std::int64_t id);
void ValidateWorkerId(std::int64_t workerId);
void ValidateTaskStatus(aquarium::api::TaskStatus status);
void ValidateAddCommentRequest(const aquarium::api::AddCommentRequest &request);
void ValidateRejectTaskRequest(const aquarium::api::RejectTaskRequest &request);

bool CheckTaskUpdatePermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo);

bool CheckTaskStatusUpdatePermission(const Users::UserDbInfo *user,
                                     const TaskDbInfo &taskInfo,
                                     aquarium::api::TaskStatus targetStatus =
                                         aquarium::api::TaskStatus::COMPLETE);

bool CheckTaskWorkerPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo);

bool CheckTaskDeletePermission(const Users::UserDbInfo *user,
                               std::int64_t authorId);

bool CheckRejectTaskPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo);

bool CheckAddCommentPermission(const Users::UserDbInfo *user,
                               const TaskDbInfo &taskInfo);

bool CheckViewCommentsPermission(const Users::UserDbInfo *user,
                                const TaskDbInfo &taskInfo);

std::vector<TaskDbInfo>
FilterTasks(const std::vector<TaskDbInfo> &tasks,
            std::optional<aquarium::api::TaskStatus> status,
            std::optional<std::int64_t> authorId,
            std::optional<std::int64_t> workerId);

} // namespace Aquarium::Tasks
