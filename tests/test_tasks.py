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
