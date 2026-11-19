import pytest
import grpc

import UserStoreService_pb2 as user_protos


async def test_get_all_unauthenticated(user_client):
    request = user_protos.GetAllUsersRequest()
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(request)
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_get_all_invalid_auth_format(user_client):
    request = user_protos.GetAllUsersRequest()
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            request,
            metadata=[('authorization', 'Token invalid_format_token')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_get_all_unknown_token(user_client):
    request = user_protos.GetAllUsersRequest()
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            request,
            metadata=[('authorization', 'Bearer unknown_user_token')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_add_user_missing_invite_key(user_client):
    request = user_protos.AddUserRequest(name='Alice')
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(request)
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_add_user_invalid_invite_format(user_client):
    request = user_protos.AddUserRequest(name='Alice')
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Bearer secret_invite_key_value')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_add_user_wrong_invite_key(user_client):
    request = user_protos.AddUserRequest(name='Alice')
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Invite wrong_invite_key')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_add_user_success(user_client):
    request = user_protos.AddUserRequest(name='Bob')
    response = await user_client.AddUser(
        request,
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    assert response.user.id > 0
    assert response.user.name == 'Bob'
    assert len(response.user.key) > 0


async def test_get_user_nonexistent(user_client):
    request = user_protos.GetUserRequest(id=999999)
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetUser(
            request,
            metadata=[('authorization', 'Bearer zxc')],
        )
    assert exc_info.value.code() == grpc.StatusCode.NOT_FOUND


async def test_user_lifecycle(user_client):
    # 1. Create a new user
    add_req = user_protos.AddUserRequest(name='Charlie')
    add_resp = await user_client.AddUser(
        add_req,
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    user_id = add_resp.user.id
    user_key = add_resp.user.key

    # 2. Update self name
    update_req = user_protos.UpdateUserRequest(id=user_id, newName='Charlie_Updated')
    update_resp = await user_client.UpdateUser(
        update_req,
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert update_resp.user.name == 'Charlie_Updated'

    # 3. Non-admin attempting to delete another user (e.g., id 1)
    delete_forbidden_req = user_protos.DeleteUserRequest(id=1)
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.DeleteUser(
            delete_forbidden_req,
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.PERMISSION_DENIED

    # 4. User deleting self
    delete_self_req = user_protos.DeleteUserRequest(id=user_id)
    delete_resp = await user_client.DeleteUser(
        delete_self_req,
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert delete_resp.success is True
