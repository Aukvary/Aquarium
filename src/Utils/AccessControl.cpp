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
    if (user->admin || user->id == taskInfo.AuthorId) {
        return true;
    }
    return (taskInfo.CheckerId != 0 && user->id == taskInfo.CheckerId);
}

bool CanChangeChecker(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    std::optional<std::int64_t> newCheckerId
) {
    if (!user) {
        return false;
    }
    if (newCheckerId.value_or(0) == taskInfo.CheckerId) {
        return true;
    }
    return user->admin || user->id == taskInfo.AuthorId;
}

bool CanUpdateTaskStatus(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo,
    aquarium::api::TaskStatus targetStatus
) {
    if (!user) {
        return false;
    }
    if (user->admin) {
        return true;
    }

    const bool isAuthor = (user->id == taskInfo.AuthorId);
    const bool isChecker =
        (taskInfo.CheckerId != 0 && user->id == taskInfo.CheckerId);
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
    if (user->admin || user->id == taskInfo.AuthorId) {
        return true;
    }
    return (taskInfo.CheckerId != 0 && user->id == taskInfo.CheckerId);
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

bool CanRejectTask(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
) {
    if (!user) {
        return false;
    }
    if (user->admin) {
        return true;
    }
    if (taskInfo.CheckerId != 0) {
        return user->id == taskInfo.CheckerId || user->id == taskInfo.AuthorId;
    }
    return user->id == taskInfo.AuthorId;
}

bool CanAddComment(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& taskInfo
) {
    if (!user) {
        return false;
    }
    if (user->admin || user->id == taskInfo.AuthorId) {
        return true;
    }
    if (taskInfo.CheckerId != 0 && user->id == taskInfo.CheckerId) {
        return true;
    }
    return std::ranges::find(taskInfo.WorkerIds, user->id) != taskInfo.WorkerIds.end();
}

bool CanViewComments(
    const Users::UserDbInfo* user,
    const Tasks::TaskDbInfo& /*taskInfo*/
) {
    return user != nullptr;
}

} // namespace Aquarium::AccessControl
