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
set(REPROC++ ON CACHE BOOL "" FORCE)
set(REPROC_TEST OFF CACHE BOOL "" FORCE)
set(REPROC_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPROC_OBJECT_LIBRARIES ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
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
FetchContent_Declare(
    reproc
    GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
    GIT_TAG 06034a7fca1ec46eddb4997f7764db89380c5216)
FetchContent_MakeAvailable(nlohmann_json cpr dotenv_source reproc)

add_library(dotenv_header_only INTERFACE)
target_include_directories(dotenv_header_only INTERFACE
    "${dotenv_source_SOURCE_DIR}/include/laserpants/dotenv")

add_library(agent_kernel STATIC
    src/domain/value.cpp
    src/domain/evidence_validation.cpp
    src/domain/latency_trace.cpp
    src/domain/runtime_event.cpp
    src/domain/session_event.cpp
    src/domain/memory_event.cpp
    src/application/state_reducer.cpp
    src/application/session_reducer.cpp
    src/application/memory_reducer.cpp
    src/application/memory_policy.cpp
    src/application/memory_retriever.cpp
    src/application/memory_engine.cpp
    src/application/memory_maintenance_scheduler.cpp
    src/application/model_context_compactor.cpp
    src/application/model_memory_consolidator.cpp
    src/application/retrieval_policy.cpp
    src/application/session_engine.cpp
    src/application/runtime_engine.cpp
    src/application/task_evaluator.cpp
    src/application/benchmark_statistics.cpp
    src/adapters/json/value_json.cpp
    src/adapters/json/strict_json.cpp
    src/adapters/rag/task_evidence_cache.cpp
    src/adapters/anthropic/cpr_http_transport.cpp
    src/adapters/anthropic/sse_decoder.cpp
    src/adapters/anthropic/anthropic_stream_assembler.cpp
    src/adapters/anthropic/anthropic_messages_client.cpp
    src/adapters/persistence/event_json.cpp
    src/adapters/persistence/session_event_json.cpp
    src/adapters/persistence/memory_event_json.cpp
    src/adapters/persistence/jsonl_memory_store.cpp
    src/adapters/persistence/jsonl_event_store.cpp
    src/adapters/persistence/jsonl_session_store.cpp)
target_include_directories(agent_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agent_kernel PUBLIC nlohmann_json::nlohmann_json
    Threads::Threads PRIVATE cpr::cpr)
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
    src/adapters/process/reproc_jsonl_process.cpp
    src/adapters/rag/rag_protocol.cpp
    src/adapters/rag/verified_pack_lease.cpp
    src/adapters/rag/native_rag_pack_verifier.cpp
    src/adapters/rag/persistent_rag_knowledge_provider.cpp
    src/adapters/build/cmake_tool_gateway.cpp
    src/adapters/tools/composite_tool_gateway.cpp
    src/ports/tool_gateway.cpp
    src/adapters/system/system_clock.cpp
    src/adapters/system/random_id_generator.cpp
    src/adapters/system/signal_cancellation.cpp
    src/config/runtime_config.cpp
    src/config/setting_source.cpp
    src/cli/terminal_text.cpp
    src/cli/streaming_terminal_text.cpp
    src/cli/terminal_capabilities.cpp
    src/cli/terminal_presenter.cpp
    src/cli/interactive_cli.cpp
    src/cli/cli_app.cpp)
target_include_directories(agent_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agent_runtime
    PUBLIC agent_kernel Threads::Threads
    PRIVATE dotenv_header_only reproc++)
if(WIN32)
    target_link_libraries(agent_runtime PRIVATE bcrypt)
endif()
if(MSVC)
    target_compile_options(agent_runtime PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(agent_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(agent src/main.cpp)
target_link_libraries(agent PRIVATE agent_runtime)
set_target_properties(agent PROPERTIES OUTPUT_NAME AgentFramework)
if(WIN32)
    add_custom_command(TARGET agent POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DDLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:agent>,|>"
            "-DTARGET_DIR=$<TARGET_FILE_DIR:agent>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake"
        VERBATIM)
endif()
if(MSVC)
    target_compile_options(agent PRIVATE /W4 /permissive- /utf-8)
endif()

if(WIN32)
    set(AGENT_READY_ENV_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.env" CACHE FILEPATH
        "Existing local environment file to copy into the explicitly requested Ready package")
    add_custom_target(agent_ready_package
        COMMAND ${CMAKE_COMMAND}
            "-DAGENT_READY_EXE=$<TARGET_FILE:agent>"
            "-DAGENT_READY_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:agent>,|>"
            "-DAGENT_READY_CONFIG=$<CONFIG>"
            "-DAGENT_READY_ENV_FILE=${AGENT_READY_ENV_FILE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_ready_package.cmake"
        DEPENDS agent
        VERBATIM)
endif()

set(AGENT_BENCHMARK_PROVENANCE_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/generated/benchmark_provenance.h")
add_custom_target(agent_benchmark_provenance
    COMMAND ${CMAKE_COMMAND}
        "-DAGENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DAGENT_OUTPUT_FILE=${AGENT_BENCHMARK_PROVENANCE_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_benchmark_provenance.cmake"
    BYPRODUCTS "${AGENT_BENCHMARK_PROVENANCE_HEADER}"
    VERBATIM)
set_source_files_properties(
    "${AGENT_BENCHMARK_PROVENANCE_HEADER}" PROPERTIES GENERATED TRUE)
add_executable(agent_benchmark
    benchmarks/agent_runtime_benchmark.cpp
    "${AGENT_BENCHMARK_PROVENANCE_HEADER}")
add_dependencies(agent_benchmark agent_benchmark_provenance)
target_link_libraries(agent_benchmark PRIVATE agent_runtime)
target_include_directories(agent_benchmark PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(agent_benchmark PRIVATE
    AGENT_BUILD_CONFIG="$<CONFIG>")
if(WIN32)
    add_custom_command(TARGET agent_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:agent_benchmark>
            $<TARGET_FILE_DIR:agent_benchmark>
        COMMAND_EXPAND_LISTS)
endif()
if(MSVC)
    target_compile_options(agent_benchmark PRIVATE /W4 /permissive- /utf-8)
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
add_agent_test(evidence_validation_tests tests/domain/evidence_validation_test.cpp)
add_agent_test(runtime_event_tests tests/domain/runtime_event_test.cpp)
add_agent_test(operation_context_tests tests/ports/operation_context_test.cpp)
add_agent_test(state_reducer_tests tests/application/state_reducer_test.cpp)
add_agent_test(session_reducer_tests tests/application/session_reducer_test.cpp)
add_agent_test(memory_reducer_tests tests/application/memory_reducer_test.cpp)
add_agent_test(memory_policy_tests tests/application/memory_policy_test.cpp)
add_agent_test(memory_retriever_tests tests/application/memory_retriever_test.cpp)
add_agent_test(memory_engine_tests tests/application/memory_engine_test.cpp)
add_agent_test(memory_maintenance_scheduler_tests
    tests/application/memory_maintenance_scheduler_test.cpp)
add_agent_test(model_context_compactor_tests tests/application/model_context_compactor_test.cpp)
add_agent_test(model_memory_consolidator_tests tests/application/model_memory_consolidator_test.cpp)
add_agent_test(memory_event_json_tests tests/adapters/memory_event_json_test.cpp)
add_agent_test(retrieval_policy_tests tests/application/retrieval_policy_test.cpp)
add_agent_test(jsonl_memory_store_tests tests/adapters/jsonl_memory_store_test.cpp)
add_agent_test(strict_json_tests tests/adapters/strict_json_test.cpp)
add_agent_test(session_engine_tests tests/application/session_engine_test.cpp)
add_agent_test(runtime_engine_tests tests/application/runtime_engine_test.cpp)
add_agent_test(task_evaluator_tests tests/application/task_evaluator_test.cpp)
add_agent_test(benchmark_statistics_tests
    tests/application/benchmark_statistics_test.cpp)
add_agent_test(jsonl_event_store_tests tests/adapters/jsonl_event_store_test.cpp)
add_agent_test(session_event_json_tests
    tests/adapters/session_event_json_test.cpp)
add_agent_test(jsonl_session_store_tests
    tests/adapters/jsonl_session_store_test.cpp)
add_agent_test(anthropic_adapter_tests tests/adapters/anthropic_messages_client_test.cpp)
target_link_libraries(anthropic_adapter_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(anthropic_adapter_tests PRIVATE ws2_32)
endif()
add_agent_test(cli_tests tests/cli/cli_app_test.cpp)
target_link_libraries(cli_tests PRIVATE agent_runtime)
add_agent_test(rag_runtime_config_tests tests/config/rag_runtime_config_test.cpp)
target_link_libraries(rag_runtime_config_tests PRIVATE agent_runtime)
add_agent_test(interactive_cli_tests tests/cli/interactive_cli_test.cpp)
target_link_libraries(interactive_cli_tests PRIVATE agent_runtime)
add_agent_test(terminal_presenter_tests tests/cli/terminal_presenter_test.cpp)
target_link_libraries(terminal_presenter_tests PRIVATE agent_runtime)
add_agent_test(signal_cancellation_tests tests/adapters/signal_cancellation_test.cpp)
target_link_libraries(signal_cancellation_tests PRIVATE agent_runtime)
add_agent_test(runtime_config_tests tests/config/runtime_config_test.cpp)
target_link_libraries(runtime_config_tests PRIVATE agent_runtime)
add_agent_test(interactive_process_tests
    tests/cli/interactive_process_test.cpp)
target_link_libraries(interactive_process_tests PRIVATE
    agent_runtime Threads::Threads)
add_dependencies(interactive_process_tests agent)
target_compile_definitions(interactive_process_tests PRIVATE
    AGENT_INTERACTIVE_EXE_PATH="$<TARGET_FILE:agent>"
    _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    target_link_libraries(interactive_process_tests PRIVATE ws2_32)
    add_test(NAME ready_package_contract CONFIGURATIONS Release
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            "-DAGENT_EXE=$<TARGET_FILE:agent>"
            "-DRUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:agent>,|>"
            "-DPROCESS_TEST_EXE=$<TARGET_FILE:interactive_process_tests>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/ready_package_contract_test.cmake")
    add_custom_target(agent_ready_package_verify
        COMMAND ${CMAKE_COMMAND}
            "-DAGENT_READY_EXE=$<TARGET_FILE:agent>"
            "-DAGENT_READY_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:agent>,|>"
            "-DAGENT_READY_CONFIG=$<CONFIG>"
            "-DAGENT_READY_DIR=${CMAKE_CURRENT_SOURCE_DIR}/out/AgentFramework-Ready"
            "-DAGENT_PROCESS_TEST_EXE=$<TARGET_FILE:interactive_process_tests>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_ready_package.cmake"
        DEPENDS agent_ready_package interactive_process_tests
        VERBATIM)
endif()
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
add_agent_test(reproc_jsonl_process_tests
    tests/adapters/reproc_jsonl_process_test.cpp)
target_link_libraries(reproc_jsonl_process_tests PRIVATE
    agent_runtime reproc++ Threads::Threads)
target_compile_definitions(reproc_jsonl_process_tests PRIVATE
    AGENT_JSONL_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
    AGENT_JSONL_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/rag/tests/jsonl_sidecar_fixture.py")
add_agent_test(rag_protocol_tests tests/adapters/rag_protocol_test.cpp)
target_link_libraries(rag_protocol_tests PRIVATE agent_runtime)
add_agent_test(native_rag_pack_verifier_tests
    tests/adapters/native_rag_pack_verifier_test.cpp)
target_link_libraries(native_rag_pack_verifier_tests PRIVATE agent_runtime)
add_agent_test(verified_pack_lease_tests
    tests/adapters/verified_pack_lease_test.cpp)
target_link_libraries(verified_pack_lease_tests PRIVATE agent_runtime)
add_agent_test(persistent_rag_knowledge_provider_tests
    tests/adapters/persistent_rag_knowledge_provider_test.cpp)
add_agent_test(task_evidence_cache_tests
    tests/adapters/task_evidence_cache_test.cpp)
target_link_libraries(persistent_rag_knowledge_provider_tests PRIVATE agent_runtime)
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
add_test(
    NAME resume_terminal_process_local
    COMMAND ${CMAKE_COMMAND}
        "-DAGENT_EXE=$<TARGET_FILE:agent>"
        "-DEVENT_LOG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/valid-completed-events.jsonl"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/resume-process"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/resume_process_test.cmake")
add_test(
    NAME interactive_startup_process
    COMMAND ${CMAKE_COMMAND}
        "-DAGENT_EXE=$<TARGET_FILE:agent>"
        "-DRUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:agent>,|>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/interactive-startup-process"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/interactive_startup_process_test.cmake")
if(WIN32)
    add_test(
        NAME knowledge_pack_build_script_contract
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPOWERSHELL_EXE=powershell"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rag/knowledge_pack_build_script_contract_test.cmake")
endif()
add_test(
    NAME agent_benchmark_process
    COMMAND ${CMAKE_COMMAND}
        "-DAGENT_BENCHMARK_EXE=$<TARGET_FILE:agent_benchmark>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/agent-benchmark-process"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark/agent_benchmark_process_test.cmake")
add_test(
    NAME benchmark_provenance
    COMMAND ${CMAKE_COMMAND}
        "-DPROVENANCE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_benchmark_provenance.cmake"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/benchmark-provenance"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark/benchmark_provenance_test.cmake")
add_agent_test(autonomous_issue_workflow_tests
    tests/integration/autonomous_issue_workflow_test.cpp)
target_link_libraries(autonomous_issue_workflow_tests PRIVATE agent_runtime)
if(Python3_Interpreter_FOUND)
    add_agent_test(rag_integration_tests
        tests/integration/rag_integration_test.cpp)
    target_link_libraries(rag_integration_tests PRIVATE agent_runtime)
    target_compile_definitions(rag_integration_tests PRIVATE
        AGENT_RAG_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        AGENT_RAG_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/rag/tests/persistent_rag_integration_fixture.py")
    add_test(
        NAME python_rag_tests
        COMMAND "${Python3_EXECUTABLE}" -E -s -X utf8 -m pytest
            -q -ra "${CMAKE_CURRENT_SOURCE_DIR}/rag/tests")
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
