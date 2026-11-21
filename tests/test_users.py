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


async def test_invite_key_constant_time_edge_cases(user_client):
    request = user_protos.AddUserRequest(name='Alice')

    # Prefix of valid key
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Invite secret_invite_key_valu')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # Appended character
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Invite secret_invite_key_valueX')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # Same length, 1 character difference at start
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Invite xecret_invite_key_value')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # Same length, 1 character difference at end
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            request,
            metadata=[('invite-key', 'Invite secret_invite_key_valuz')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_internal_key_constant_time_edge_cases(user_client):
    req = user_protos.GetAllUsersRequest()
    valid_key = 'test_internal_key_minimum_32_characters_long_12345'

    # Prefix
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            req,
            metadata=[('authorization', f'Bearer {valid_key[:-1]}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # Appended char
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            req,
            metadata=[('authorization', f'Bearer {valid_key}X')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED

    # Mismatched char
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            req,
            metadata=[('authorization', f'Bearer x{valid_key[1:]}')],
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
            metadata=[('authorization', 'Bearer test_internal_key_minimum_32_characters_long_12345')],
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

    # 5. Verify deleted user's token is immediately revoked from auth cache
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetAll(
            user_protos.GetAllUsersRequest(),
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_cache_invalidation_after_update_and_delete(user_client):
    # 1. Create user
    add_resp = await user_client.AddUser(
        user_protos.AddUserRequest(name='DaveCache'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    user_id = add_resp.user.id
    user_key = add_resp.user.key

    # 2. Update profile name
    await user_client.UpdateUser(
        user_protos.UpdateUserRequest(id=user_id, newName='DaveCache_Renamed'),
        metadata=[('authorization', f'Bearer {user_key}')],
    )

    # 3. Query user info to verify cache picked up the updated name
    get_resp = await user_client.GetUser(
        user_protos.GetUserRequest(id=user_id),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert get_resp.user.name == 'DaveCache_Renamed'

    # 4. Delete user
    del_resp = await user_client.DeleteUser(
        user_protos.DeleteUserRequest(id=user_id),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert del_resp.success is True

    # 5. Verify deleted token is revoked
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetUser(
            user_protos.GetUserRequest(id=user_id),
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.UNAUTHENTICATED


async def test_soft_delete_and_active_user_filtering(user_client):
    admin_token = 'test_internal_key_minimum_32_characters_long_12345'

    # 1. Create a user
    add_resp = await user_client.AddUser(
        user_protos.AddUserRequest(name='EveSoftDelete'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    user_id = add_resp.user.id
    user_key = add_resp.user.key

    # 2. Verify user exists in GetAll and GetUser
    get_resp = await user_client.GetUser(
        user_protos.GetUserRequest(id=user_id),
        metadata=[('authorization', f'Bearer {admin_token}')],
    )
    assert get_resp.user.id == user_id

    all_resp = await user_client.GetAll(
        user_protos.GetAllUsersRequest(),
        metadata=[('authorization', f'Bearer {admin_token}')],
    )
    assert any(u.id == user_id for u in all_resp.users)

    # 3. Soft delete the user
    del_resp = await user_client.DeleteUser(
        user_protos.DeleteUserRequest(id=user_id),
        metadata=[('authorization', f'Bearer {user_key}')],
    )
    assert del_resp.success is True

    # 4. Inactive user is omitted from GetAll
    all_after = await user_client.GetAll(
        user_protos.GetAllUsersRequest(),
        metadata=[('authorization', f'Bearer {admin_token}')],
    )
    assert not any(u.id == user_id for u in all_after.users)

    # 5. Inactive user returns NOT_FOUND in GetUser
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.GetUser(
            user_protos.GetUserRequest(id=user_id),
            metadata=[('authorization', f'Bearer {admin_token}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.NOT_FOUND

    # 6. Deleting already deactivated user returns success=False
    del_again_resp = await user_client.DeleteUser(
        user_protos.DeleteUserRequest(id=user_id),
        metadata=[('authorization', f'Bearer {admin_token}')],
    )
    assert del_again_resp.success is False


async def test_user_input_validation(user_client):
    # 1. AddUser validations
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            user_protos.AddUserRequest(name=''),
            metadata=[('invite-key', 'Invite secret_invite_key_value')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            user_protos.AddUserRequest(name='   \t  \n '),
            metadata=[('invite-key', 'Invite secret_invite_key_value')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.AddUser(
            user_protos.AddUserRequest(name='x' * 101),
            metadata=[('invite-key', 'Invite secret_invite_key_value')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # Create valid user to test UpdateUser
    add_resp = await user_client.AddUser(
        user_protos.AddUserRequest(name='ValidUserForUpdate'),
        metadata=[('invite-key', 'Invite secret_invite_key_value')],
    )
    user_id = add_resp.user.id
    user_key = add_resp.user.key

    # 2. UpdateUser validations
    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.UpdateUser(
            user_protos.UpdateUserRequest(id=user_id, newName=''),
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.UpdateUser(
            user_protos.UpdateUserRequest(id=user_id, newName='   \n  '),
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    with pytest.raises(grpc.RpcError) as exc_info:
        await user_client.UpdateUser(
            user_protos.UpdateUserRequest(id=user_id, newName='u' * 101),
            metadata=[('authorization', f'Bearer {user_key}')],
        )
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT



