cmake_minimum_required(VERSION 3.21)
project(AgentFramework VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(Python3 3.10 COMPONENTS Interpreter QUIET)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AGENT_ENABLE_LIVE_TESTS "Enable credentialed provider smoke test" OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS_SSL OFF CACHE BOOL "" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.14.2
    GIT_SHALLOW TRUE)
FetchContent_Declare(
    dotenv_source
    GIT_REPOSITORY https://github.com/laserpants/dotenv-cpp.git
    GIT_TAG 9275210b8abf1a551fb81e0ba45866286d764acd
    SOURCE_SUBDIR _header_only)
FetchContent_MakeAvailable(nlohmann_json cpr dotenv_source)

add_library(dotenv_header_only INTERFACE)
target_include_directories(dotenv_header_only INTERFACE
    "${dotenv_source_SOURCE_DIR}/include/laserpants/dotenv")

add_library(agent_kernel STATIC
    src/domain/value.cpp
    src/domain/runtime_event.cpp
    src/application/state_reducer.cpp
    src/application/runtime_engine.cpp
    src/adapters/json/value_json.cpp
    src/adapters/anthropic/cpr_http_transport.cpp
    src/adapters/anthropic/anthropic_messages_client.cpp
    src/adapters/persistence/event_json.cpp
    src/adapters/persistence/jsonl_event_store.cpp)
target_include_directories(agent_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agent_kernel PUBLIC nlohmann_json::nlohmann_json PRIVATE cpr::cpr)
if(MSVC)
    target_compile_options(agent_kernel PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(agent_runtime STATIC
    src/adapters/empty/empty_tool_gateway.cpp
    src/adapters/empty/empty_knowledge_provider.cpp
    src/adapters/workspace/workspace_text.cpp
    src/adapters/workspace/workspace_path_policy.cpp
    src/adapters/workspace/workspace_file_ops.cpp
    src/adapters/workspace/workspace_tool_gateway.cpp
    src/adapters/process/direct_process_runner.cpp
    src/adapters/build/cmake_tool_gateway.cpp
    src/adapters/tools/composite_tool_gateway.cpp
    src/adapters/system/system_clock.cpp
    src/adapters/system/random_id_generator.cpp
    src/adapters/system/signal_cancellation.cpp
    src/config/runtime_config.cpp
    src/cli/cli_app.cpp)
target_include_directories(agent_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agent_runtime
    PUBLIC agent_kernel Threads::Threads
    PRIVATE dotenv_header_only)
if(MSVC)
    target_compile_options(agent_runtime PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(agent_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(agent src/main.cpp)
target_link_libraries(agent PRIVATE agent_runtime)
if(WIN32)
    add_custom_command(TARGET agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:agent> $<TARGET_FILE_DIR:agent>
        COMMAND_EXPAND_LISTS)
endif()
if(MSVC)
    target_compile_options(agent PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
function(add_agent_test target source)
    add_executable(${target} tests/test_main.cpp ${source})
    target_link_libraries(${target} PRIVATE agent_kernel)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()
add_agent_test(domain_primitives_tests tests/domain/value_result_test.cpp)
add_agent_test(runtime_event_tests tests/domain/runtime_event_test.cpp)
add_agent_test(state_reducer_tests tests/application/state_reducer_test.cpp)
add_agent_test(runtime_engine_tests tests/application/runtime_engine_test.cpp)
add_agent_test(jsonl_event_store_tests tests/adapters/jsonl_event_store_test.cpp)
add_agent_test(anthropic_adapter_tests tests/adapters/anthropic_messages_client_test.cpp)
target_link_libraries(anthropic_adapter_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(anthropic_adapter_tests PRIVATE ws2_32)
endif()
add_agent_test(cli_tests tests/cli/cli_app_test.cpp)
target_link_libraries(cli_tests PRIVATE agent_runtime)
add_agent_test(runtime_integration_tests tests/integration/runtime_integration_test.cpp)
target_link_libraries(runtime_integration_tests PRIVATE agent_runtime)
add_agent_test(workspace_primitives_tests tests/adapters/workspace_primitives_test.cpp)
target_link_libraries(workspace_primitives_tests PRIVATE agent_runtime)
add_agent_test(workspace_tool_gateway_tests tests/adapters/workspace_tool_gateway_test.cpp)
target_link_libraries(workspace_tool_gateway_tests PRIVATE agent_runtime)
add_executable(process_fixture tests/process/process_fixture.cpp)
target_link_libraries(process_fixture PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
if(MSVC)
    target_compile_options(process_fixture PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(process_fixture PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_agent_test(direct_process_runner_tests tests/adapters/direct_process_runner_test.cpp)
target_link_libraries(direct_process_runner_tests PRIVATE agent_runtime Threads::Threads)
add_dependencies(direct_process_runner_tests process_fixture)
target_compile_definitions(direct_process_runner_tests PRIVATE
    AGENT_PROCESS_FIXTURE_PATH="$<TARGET_FILE:process_fixture>"
    _CRT_SECURE_NO_WARNINGS)
add_agent_test(composite_tool_gateway_tests tests/adapters/composite_tool_gateway_test.cpp)
target_link_libraries(composite_tool_gateway_tests PRIVATE agent_runtime)
add_agent_test(cmake_tool_gateway_tests tests/adapters/cmake_tool_gateway_test.cpp)
target_link_libraries(cmake_tool_gateway_tests PRIVATE agent_runtime)
add_agent_test(build_tool_integration_tests tests/integration/build_tool_integration_test.cpp)
target_link_libraries(build_tool_integration_tests PRIVATE agent_runtime)
add_test(
    NAME verify_log_process_credential_free
    COMMAND ${CMAKE_COMMAND}
        "-DAGENT_EXE=$<TARGET_FILE:agent>"
        "-DEVENT_LOG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/valid-completed-events.jsonl"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/verify-log-process"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/verify_log_process_test.cmake")
if(Python3_Interpreter_FOUND)
    add_test(
        NAME python_rag_tests
        COMMAND "${Python3_EXECUTABLE}" -E -s -X utf8 -m unittest
            discover -s "${CMAKE_CURRENT_SOURCE_DIR}/rag/tests" -v)
    set_tests_properties(python_rag_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(AGENT_ENABLE_LIVE_TESTS)
    add_executable(anthropic_live_smoke tests/live/anthropic_live_smoke.cpp)
    target_link_libraries(anthropic_live_smoke PRIVATE agent_runtime)
    if(WIN32)
        add_custom_command(TARGET anthropic_live_smoke POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:anthropic_live_smoke>
                $<TARGET_FILE_DIR:anthropic_live_smoke>
            COMMAND_EXPAND_LISTS)
    endif()
    if(MSVC)
        target_compile_options(anthropic_live_smoke PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME anthropic_live_smoke COMMAND anthropic_live_smoke)
endif()
