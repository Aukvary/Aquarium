cmake_minimum_required(VERSION 3.20)
project(aquarium CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(USERVER_USE_STATIC_LIBS
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_PATCH_LIBPQ
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FORCE_DOWNLOAD_PROTOBUF
    ON
    CACHE BOOL "" FORCE)
set(USERVER_FORCE_DOWNLOAD_GRPC
    ON
    CACHE BOOL "" FORCE)

set(USERVER_FEATURE_GRPC
    ON
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_POSTGRESQL
    ON
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_MONGODB
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_CLICKHOUSE
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_RABBITMQ
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_KAFKA
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_MYSQL
    OFF
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_GRPC_REFLECTION
    ON
    CACHE BOOL "" FORCE)
set(USERVER_FEATURE_STACKTRACE
    OFF
    CACHE BOOL "" FORCE)

find_package(
  userver
  COMPONENTS core
             grpc
             postgresql
             utest
             grpc-utest
  QUIET)

if(NOT userver_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    userver
    GIT_REPOSITORY https://github.com/userver-framework/userver.git
    GIT_TAG v3.2)
  FetchContent_MakeAvailable(userver)
endif()

userver_setup_environment()

userver_add_grpc_library(${PROJECT_NAME}-proto PROTOS UserStoreService.proto
                         UtilsService.proto TaskStoreService.proto)

add_library(
  ${PROJECT_NAME}_objs src/Utils/HealthService.cpp
                       src/Utils/AccessControl.cpp
                       src/Users/UserStoreService.cpp src/Users/Auth.cpp
                       src/Tasks/TaskService.cpp)

target_include_directories(${PROJECT_NAME}_objs
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
  ${PROJECT_NAME}_objs PUBLIC ${PROJECT_NAME}-proto userver::core userver::grpc
                              userver::postgresql)

add_executable(${PROJECT_NAME} src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(
  ${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_objs ${PROJECT_NAME}-proto
                          userver::core userver::grpc userver::postgresql)

# Unit & Coroutine Tests
enable_testing()

add_executable(
  ${PROJECT_NAME}-unittest
  tests/unittest/test_user_service.cpp
  tests/unittest/test_task_service.cpp
)

target_include_directories(${PROJECT_NAME}-unittest PRIVATE src)
target_link_libraries(
  ${PROJECT_NAME}-unittest PRIVATE
  ${PROJECT_NAME}_objs
  ${PROJECT_NAME}-proto
  userver::utest
  userver::grpc-utest
  userver::postgresql
)

add_google_tests(${PROJECT_NAME}-unittest)
set_tests_properties(${PROJECT_NAME}-unittest PROPERTIES ENVIRONMENT "USERVER_ENABLE_STACK_USAGE_MONITOR=0")
