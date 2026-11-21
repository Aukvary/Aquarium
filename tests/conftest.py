import sys
from pathlib import Path
import pytest
from testsuite.databases.pgsql import discover

# Add build/proto directory to python path for proto imports
build_proto_dir = Path(__file__).resolve().parent.parent / 'build' / 'proto'
if str(build_proto_dir) not in sys.path:
    sys.path.insert(0, str(build_proto_dir))

pytest_plugins = [
    'pytest_userver.plugins.core',
    'pytest_userver.plugins.postgresql',
    'pytest_userver.plugins.grpc',
]

@pytest.fixture(scope='session')
def service_config_vars_path(service_source_dir):
    """Path to test config variables with safe test defaults"""
    return service_source_dir / 'configs' / 'config_vars.test.yaml'

@pytest.fixture(scope='session')
def pgsql_local(service_source_dir, pgsql_local_create):
    """Initializes PostgreSQL schemas from sql directory"""
    databases = discover.find_schemas(
        'aquarium',
        [service_source_dir.joinpath('sql')],
    )
    return pgsql_local_create(list(databases.values()))

@pytest.fixture
def user_client(grpc_channel):
    import UserStoreService_pb2_grpc as user_services
    return user_services.UserStoreServiceStub(grpc_channel)

@pytest.fixture
def task_client(grpc_channel):
    import TaskStoreService_pb2_grpc as task_services
    return task_services.TaskStoreServiceStub(grpc_channel)
