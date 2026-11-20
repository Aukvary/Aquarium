#pragma once

#include "./TaskStore.hpp"
#include "./Users.hpp"
#include <TaskStoreService.pb.h>

#include <cstdint>
#include <optional>

namespace Aquarium::AccessControl {

// User permissions
bool CanUpdateUser(
    const Users::UserDbInfo* user,
    std::int64_t targetUserId
);
bool CanDeleteUser(
    const Users::UserDbInfo* user,
    std::int64_t targetUserId
);

// Task permissions
bool CanCreateTask(
    const Users::UserDbInfo* user,
    std::int64_t authorId
);
bool CanUpdateTask(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
);
bool CanChangeChecker(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    std::optional<std::int64_t> newCheckerId
);
bool CanUpdateTaskStatus(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    aquarium::api::TaskStatus targetStatus
);
bool CanModifyTaskWorkers(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
);
bool CanDeleteTask(
    const Users::UserDbInfo* user,
    std::int64_t authorId
);

// Comment & Review permissions
bool CanRejectTask(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
);
bool CanAddComment(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
);
bool CanViewComments(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
);

} // namespace Aquarium::AccessControl
