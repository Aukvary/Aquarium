#include "../AccessControl.hpp"

#include <algorithm>
#include <ranges>

namespace Aquarium::AccessControl {

bool CanUpdateUser(
    const Users::UserDbInfo* user,
    std::int64_t targetUserId
) {
    if (!user) {
        return false;
    }
    return user->admin || user->id == targetUserId;
}

bool CanDeleteUser(
    const Users::UserDbInfo* user,
    std::int64_t targetUserId
) {
    if (!user) {
        return false;
    }
    return user->admin || user->id == targetUserId;
}

bool CanCreateTask(
    const Users::UserDbInfo* user,
    std::int64_t authorId
) {
    if (!user) {
        return false;
    }
    return user->admin || user->id == authorId;
}

bool CanUpdateTask(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
) {
    if (!user) {
        return false;
    }
    if (user->admin || taskInfo.CheckerId == 0) {
        return true;
    }
    return (user->id == taskInfo.AuthorId || user->id == taskInfo.CheckerId);
}

bool CanChangeChecker(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    std::optional<std::int64_t> newCheckerId
) {
    if (newCheckerId.value_or(0) == taskInfo.CheckerId) {
        return true;
    }
    if (!user) {
        return false;
    }
    if (user->admin || taskInfo.CheckerId == 0) {
        return true;
    }
    return user->id == taskInfo.AuthorId;
}

bool CanUpdateTaskStatus(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    aquarium::api::TaskStatus targetStatus
) {
    if (!user) {
        return false;
    }
    if (user->admin || taskInfo.CheckerId == 0) {
        return true;
    }

    const bool isAuthor = (user->id == taskInfo.AuthorId);
    const bool isChecker = (user->id == taskInfo.CheckerId);
    const bool isWorker = std::ranges::find(taskInfo.WorkerIds, user->id) !=
                          taskInfo.WorkerIds.end();

    if (!isAuthor && !isChecker && !isWorker) {
        return false;
    }

    if (isWorker && !isAuthor && !isChecker) {
        if (targetStatus == aquarium::api::TaskStatus::IN_REVIEW ||
            targetStatus == aquarium::api::TaskStatus::VERIFIED ||
            targetStatus == aquarium::api::TaskStatus::ARCHIVE) {
            return false;
        }
    }

    return true;
}

bool CanModifyTaskWorkers(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
) {
    if (!user) {
        return false;
    }
    if (user->admin || taskInfo.CheckerId == 0) {
        return true;
    }
    return (user->id == taskInfo.AuthorId || user->id == taskInfo.CheckerId);
}

bool CanDeleteTask(
    const Users::UserDbInfo* user,
    std::int64_t authorId
) {
    if (!user) {
        return false;
    }
    return user->admin || user->id == authorId;
}

} // namespace Aquarium::AccessControl
