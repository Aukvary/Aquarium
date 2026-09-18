cmake_minimum_required(VERSION 3.20)
project(aquarium CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(USERVER_USE_STATIC_LIBS OFF CACHE BOOL "" FORCE)

set(USERVER_FEATURE_PATCH_LIBPQ OFF CACHE BOOL "" FORCE)

set(USERVER_FORCE_DOWNLOAD_PROTOBUF ON CACHE BOOL "" FORCE)
set(USERVER_FORCE_DOWNLOAD_GRPC ON CACHE BOOL "" FORCE)

set(USERVER_FEATURE_GRPC ON CACHE BOOL "" FORCE)
set(USERVER_FEATURE_POSTGRESQL ON CACHE BOOL "" FORCE)
set(USERVER_FEATURE_MONGODB OFF CACHE BOOL "" FORCE)
set(USERVER_FEATURE_CLICKHOUSE OFF CACHE BOOL "" FORCE)
set(USERVER_FEATURE_RABBITMQ OFF CACHE BOOL "" FORCE)
set(USERVER_FEATURE_KAFKA OFF CACHE BOOL "" FORCE)
set(USERVER_FEATURE_MYSQL OFF CACHE BOOL "" FORCE)
set(USERVER_FEATURE_GRPC_REFLECTION ON CACHE BOOL "" FORCE)

set(USERVER_FEATURE_STACKTRACE OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    userver
    GIT_REPOSITORY https://github.com/userver-framework/userver.git
    GIT_TAG        v3.2
)
FetchContent_MakeAvailable(userver)

userver_setup_environment()

add_library(
    ${PROJECT_NAME}_objs
    src/CheckHealthHandler.cpp
    src/CheckHealthService.cpp
)

target_link_libraries(${PROJECT_NAME}_objs PUBLIC userver::grpc)
target_include_directories(${PROJECT_NAME}_objs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
userver_add_grpc_library(${PROJECT_NAME}-proto PROTOS aquarium.proto)
target_link_libraries(${PROJECT_NAME}_objs PUBLIC ${PROJECT_NAME}-proto)

add_executable(
    ${PROJECT_NAME}
    src/main.cpp
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        ${PROJECT_NAME}-proto
        userver::grpc
        userver::postgresql
)

target_include_directories(${PROJECT_NAME} PRIVATE src)
