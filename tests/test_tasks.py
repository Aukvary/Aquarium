import pytest
import grpc

import TaskStoreService_pb2 as task_protos
import UserStoreService_pb2 as user_protos


async def test_create_task_unauthenticated(task_client):
    request = task_protos.CreateTaskRequest(
        title='Sample Task',
        description='Desc',
        authorId=1,
        status=task_protos.TaskStatus.PLAN,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.CreateTask(request)
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_create_task_empty_title(task_client):
    request = task_protos.CreateTaskRequest(
        title='',
        description='Desc',
        authorId=1,
        status=task_protos.TaskStatus.PLAN,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.CreateTask(
            request,
            metadata=[('authorization', 'Bearer zxc')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


async def test_create_task_invalid_author_id(task_client):
    request = task_protos.CreateTaskRequest(
        title='Valid Title',
        description='Desc',
        authorId=0,
        status=task_protos.TaskStatus.PLAN,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.CreateTask(
            request,
            metadata=[('authorization', 'Bearer zxc')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


async def test_get_task_invalid_id(task_client):
    request = task_protos.GetTaskRequest(id=0)
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.GetTask(
            request,
            metadata=[('authorization', 'Bearer zxc')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


async def test_get_task_not_found(task_client):
    request = task_protos.GetTaskRequest(id=999999)
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.GetTask(
            request,
            metadata=[('authorization', 'Bearer zxc')],
        )
    assert exc_info.value.code() == grpc.StatusCode.NOT_FOUND


async def test_task_lifecycle_and_permissions(task_client, user_client):
    # Setup users: author and stranger via internal key
    add_author = await user_client.AddUser(
        user_protos.AddUserRequest(name='AuthorUser'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    author_id = add_author.user.id
    author_key = add_author.user.key

    add_stranger = await user_client.AddUser(
        user_protos.AddUserRequest(name='StrangerUser'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    stranger_key = add_stranger.user.key

    # 1. Author creates task
    create_req = task_protos.CreateTaskRequest(
        title='Feature X',
        description='Implement Feature X',
        authorId=author_id,
        status=task_protos.TaskStatus.PLAN,
    )
    create_resp = await task_client.CreateTask(
        create_req,
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    task_id = create_resp.task.id
    assert create_resp.task.title == 'Feature X'
    assert create_resp.task.status == task_protos.TaskStatus.PLAN

    # 2. Get task
    get_resp = await task_client.GetTask(
        task_protos.GetTaskRequest(id=task_id),
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    assert get_resp.task.id == task_id
    assert get_resp.task.title == 'Feature X'

    # 3. Stranger attempts to update task details -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.UpdateTask(
            task_protos.UpdateTaskRequest(id=task_id, title='Hacked Title'),
            metadata=[('authorization', f'Bearer {stranger_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 4. Author updates task details -> OK
    update_resp = await task_client.UpdateTask(
        task_protos.UpdateTaskRequest(id=task_id, title='Feature X - Updated'),
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    assert update_resp.task.title == 'Feature X - Updated'

    # 5. Stranger attempts to update status -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.UpdateTaskStatus(
            task_protos.UpdateTaskStatusRequest(
                id=task_id,
                status=task_protos.TaskStatus.ACTIVE,
            ),
            metadata=[('authorization', f'Bearer {stranger_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 6. Author updates status -> OK
    status_resp = await task_client.UpdateTaskStatus(
        task_protos.UpdateTaskStatusRequest(
            id=task_id,
            status=task_protos.TaskStatus.ACTIVE,
        ),
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    assert status_resp.task.status == task_protos.TaskStatus.ACTIVE

    # 7. Stranger attempts to delete task -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.DeleteTask(
            task_protos.DeleteTaskRequest(id=task_id),
            metadata=[('authorization', f'Bearer {stranger_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 8. Author deletes task -> OK
    delete_resp = await task_client.DeleteTask(
        task_protos.DeleteTaskRequest(id=task_id),
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    assert delete_resp.success is True


async def test_comments_and_rejection_flow(task_client, user_client):
    # Setup users: author Alice, checker Bob, worker Charlie, stranger Dave
    add_author = await user_client.AddUser(
        user_protos.AddUserRequest(name='Alice'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    author_id = add_author.user.id
    author_key = add_author.user.key

    add_checker = await user_client.AddUser(
        user_protos.AddUserRequest(name='Bob'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    checker_id = add_checker.user.id
    checker_key = add_checker.user.key

    add_worker = await user_client.AddUser(
        user_protos.AddUserRequest(name='Charlie'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    worker_id = add_worker.user.id
    worker_key = add_worker.user.key

    add_stranger = await user_client.AddUser(
        user_protos.AddUserRequest(name='Dave'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    stranger_key = add_stranger.user.key

    # 1. Create task with checker and worker
    create_req = task_protos.CreateTaskRequest(
        title='Reviewable Task',
        description='Needs review',
        authorId=author_id,
        checkerId=checker_id,
        workerIds=[worker_id],
        status=task_protos.TaskStatus.ACTIVE,
    )
    create_resp = await task_client.CreateTask(
        create_req,
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    task_id = create_resp.task.id

    # 2. Add comment unauthenticated -> UNAUTHENTICATED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.AddComment(
            task_protos.AddCommentRequest(
                taskId=task_id,
                content='Unauthenticated comment',
            ),
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # 3. Add comment with empty content -> INVALID_ARGUMENT
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.AddComment(
            task_protos.AddCommentRequest(
                taskId=task_id,
                content='',
            ),
            metadata=[('authorization', f'Bearer {worker_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # 4. Stranger tries to add comment -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.AddComment(
            task_protos.AddCommentRequest(
                taskId=task_id,
                content='Stranger comment',
            ),
            metadata=[('authorization', f'Bearer {stranger_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 5. Worker and Checker add comments -> OK
    c1 = await task_client.AddComment(
        task_protos.AddCommentRequest(
            taskId=task_id,
            content='Work is ready for review',
            type=task_protos.CommentType.COMMENT_GENERAL,
        ),
        metadata=[('authorization', f'Bearer {worker_key}')],
    )
    assert c1.comment.authorId == worker_id
    assert c1.comment.content == 'Work is ready for review'

    # 6. Worker moves task to COMPLETE
    await task_client.UpdateTaskStatus(
        task_protos.UpdateTaskStatusRequest(
            id=task_id,
            status=task_protos.TaskStatus.COMPLETE,
        ),
        metadata=[('authorization', f'Bearer {worker_key}')],
    )

    # 7. Stranger attempts to reject task -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.RejectTask(
            task_protos.RejectTaskRequest(
                taskId=task_id,
                reason='Stranger rejection',
            ),
            metadata=[('authorization', f'Bearer {stranger_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 8. Worker attempts to reject task -> PERMISSION_DENIED
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.RejectTask(
            task_protos.RejectTaskRequest(
                taskId=task_id,
                reason='Worker rejection',
            ),
            metadata=[('authorization', f'Bearer {worker_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 9. Checker rejects with empty reason -> INVALID_ARGUMENT
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.RejectTask(
            task_protos.RejectTaskRequest(
                taskId=task_id,
                reason='',
            ),
            metadata=[('authorization', f'Bearer {checker_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # 10. Checker rejects with valid reason -> OK (status ACTIVE, comment added)
    reject_resp = await task_client.RejectTask(
        task_protos.RejectTaskRequest(
            taskId=task_id,
            reason='Please add tests and fix styling',
        ),
        metadata=[('authorization', f'Bearer {checker_key}')],
    )
    assert reject_resp.task.status == task_protos.TaskStatus.ACTIVE
    assert reject_resp.comment.authorId == checker_id
    assert reject_resp.comment.type == task_protos.CommentType.COMMENT_REVISION_REASON
    assert reject_resp.comment.content == 'Please add tests and fix styling'

    # 11. Trying to reject an ACTIVE task -> INVALID_ARGUMENT
    with pytest.raises(grpc.RpcError) as exc_info:
        await task_client.RejectTask(
            task_protos.RejectTaskRequest(
                taskId=task_id,
                reason='Already active',
            ),
            metadata=[('authorization', f'Bearer {checker_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # 12. GetComments returns both the general comment and the revision comment
    comments_resp = await task_client.GetComments(
        task_protos.GetCommentsRequest(taskId=task_id),
        metadata=[('authorization', f'Bearer {author_key}')],
    )
    assert len(comments_resp.comments) == 2
    assert comments_resp.comments[0].type == task_protos.CommentType.COMMENT_GENERAL
    assert comments_resp.comments[1].type == task_protos.CommentType.COMMENT_REVISION_REASON


async def test_get_tasks_pagination_and_filtering(task_client, user_client):
    # 1. Create a dedicated user
    add_user = await user_client.AddUser(
        user_protos.AddUserRequest(name='PaginationTester'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    user_id = add_user.user.id
    user_key = add_user.user.key

    # 2. Create 5 tasks for this user
    for i in range(5):
        await task_client.CreateTask(
            task_protos.CreateTaskRequest(
                title=f'Pagination Task {i}',
                description='Desc',
                authorId=user_id,
                status=task_protos.TaskStatus.PLAN,
            ),
            metadata=[('authorization', f'Bearer {user_key}')],
        )

    # 3. Query page 1: limit=2, offset=0
    resp_page1 = await task_client.GetTasks(
        task_protos.GetTasksRequest(
            authorId=user_id,
            limit=2,
            offset=0,
        ),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert resp_page1.totalCount == 5
    assert len(resp_page1.tasks) == 2

    # 4. Query page 2: limit=2, offset=2
    resp_page2 = await task_client.GetTasks(
        task_protos.GetTasksRequest(
            authorId=user_id,
            limit=2,
            offset=2,
        ),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert resp_page2.totalCount == 5
    assert len(resp_page2.tasks) == 2
    assert resp_page2.tasks[0].id != resp_page1.tasks[0].id

    # 5. Query page 3: limit=2, offset=4
    resp_page3 = await task_client.GetTasks(
        task_protos.GetTasksRequest(
            authorId=user_id,
            limit=2,
            offset=4,
        ),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert resp_page3.totalCount == 5
    assert len(resp_page3.tasks) == 1

    # 6. Query beyond totalCount
    resp_empty = await task_client.GetTasks(
        task_protos.GetTasksRequest(
            authorId=user_id,
            limit=2,
            offset=100,
        ),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert resp_empty.totalCount == 5
    assert len(resp_empty.tasks) == 0


async def test_subscribe_events_streaming(task_client, user_client):
    import asyncio

    # 1. Create user
    add_author = await user_client.AddUser(
        user_protos.AddUserRequest(name='StreamAuthor'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    author_id = add_author.user.id
    author_key = add_author.user.key

    # 2. Subscribe to events stream
    stream = task_client.SubscribeEvents(
        task_protos.SubscribeEventsRequest(),
        metadata=[('authorization', f'Bearer {author_key}')],
    )

    # 3. Create a task
    create_task_task = asyncio.create_task(
        task_client.CreateTask(
            task_protos.CreateTaskRequest(
                title='Realtime Task',
                description='Test streaming',
                authorId=author_id,
                status=task_protos.TaskStatus.PLAN,
            ),
            metadata=[('authorization', f'Bearer {author_key}')],
        )
    )

    # Read event from stream
    read_event_task = asyncio.create_task(stream.read())
    created_resp, first_event = await asyncio.gather(
        create_task_task,
        read_event_task,
    )

    assert first_event.type == task_protos.TaskEvent.TASK_CREATED
    assert first_event.taskId == created_resp.task.id
    assert first_event.task.title == 'Realtime Task'
    assert first_event.actorUserId == author_id

    # 4. Cancel stream
    stream.cancel()


