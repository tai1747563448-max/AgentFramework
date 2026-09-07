if(NOT DEFINED AGENT_EXE OR NOT DEFINED RUNTIME_DLLS OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "AGENT_EXE, RUNTIME_DLLS, and TEST_ROOT are required")
endif()

# Hostile inherited configuration must not override this test's synthetic
# configuration. The launch harness below is responsible for removing it.
set(ENV{AGENT_MODEL} "HOSTILE_INHERITED_MODEL")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(deployment "${TEST_ROOT}/deployment")
set(working_directory "${TEST_ROOT}/working-directory")
file(MAKE_DIRECTORY "${deployment}" "${working_directory}")
get_filename_component(exe_name "${AGENT_EXE}" NAME)
set(isolated_exe "${deployment}/${exe_name}")
configure_file("${AGENT_EXE}" "${isolated_exe}" COPYONLY)
string(REPLACE "|" ";" runtime_dlls "${RUNTIME_DLLS}")
foreach(dll IN LISTS runtime_dlls)
    get_filename_component(dll_name "${dll}" NAME)
    configure_file("${dll}" "${deployment}/${dll_name}" COPYONLY)
endforeach()
set(runtime_root "${TEST_ROOT}/runtime")
file(TO_CMAKE_PATH "${runtime_root}" runtime_root_env)
file(WRITE "${deployment}/.env"
    "AGENT_BASE_URL=https://provider.invalid\n"
    "AGENT_MODEL=MiniMax-M3\n"
    "AGENT_API_KEY=PROCESS_TEST_SECRET\n"
    "AGENT_RUNTIME_ROOT=${runtime_root_env}\n")
file(WRITE "${TEST_ROOT}/first-input.txt" "\n/exit\n")

# Build a value-free list of every inherited AGENT_* variable name and remove
# each one from the child. Values are neither logged nor forwarded.
execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
    OUTPUT_VARIABLE inherited_environment
    ERROR_QUIET)
string(REPLACE "\r\n" "\n" inherited_environment "${inherited_environment}")
string(REGEX MATCHALL "(^|\n)AGENT_[A-Za-z0-9_]*="
    inherited_agent_assignments "${inherited_environment}")
set(agent_env_unsets)
foreach(assignment IN LISTS inherited_agent_assignments)
    string(REGEX REPLACE "^\n" "" name "${assignment}")
    string(REGEX REPLACE "=$" "" name "${name}")
    list(APPEND agent_env_unsets "--unset=${name}")
endforeach()
unset(inherited_environment)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${agent_env_unsets} "${isolated_exe}"
    WORKING_DIRECTORY "${working_directory}"
    INPUT_FILE "${TEST_ROOT}/first-input.txt"
    OUTPUT_VARIABLE first_output
    ERROR_VARIABLE first_error
    RESULT_VARIABLE first_result
    TIMEOUT 20)

if(NOT first_result EQUAL 0)
    message(FATAL_ERROR
        "first interactive startup failed: ${first_result}\n${first_error}")
endif()
if(NOT first_output MATCHES "AgentFramework")
    message(FATAL_ERROR "interactive banner was not printed")
endif()
if(NOT first_output MATCHES "Model: MiniMax-M3")
    message(FATAL_ERROR "interactive model was not printed")
endif()
if(first_error)
    message(FATAL_ERROR "unexpected first startup stderr: ${first_error}")
endif()

file(GLOB session_logs "${runtime_root}/sessions/session-*/events.jsonl")
list(LENGTH session_logs session_count)
if(NOT session_count EQUAL 1)
    message(FATAL_ERROR "expected one durable session, got ${session_count}")
endif()
list(GET session_logs 0 session_log)
file(READ "${session_log}" session_bytes)
if(NOT session_bytes MATCHES "session_started")
    message(FATAL_ERROR "session_started was not persisted")
endif()
if(session_bytes MATCHES "PROCESS_TEST_SECRET")
    message(FATAL_ERROR "credential leaked into session log")
endif()
string(REGEX MATCH "Session: (session-[0-9a-f]+)" first_session_match
    "${first_output}")
set(first_session "${CMAKE_MATCH_1}")
if(NOT first_session)
    message(FATAL_ERROR "first session ID was not printed")
endif()

file(WRITE "${TEST_ROOT}/second-input.txt" "/status\n/exit\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${agent_env_unsets} "${isolated_exe}"
    WORKING_DIRECTORY "${working_directory}"
    INPUT_FILE "${TEST_ROOT}/second-input.txt"
    OUTPUT_VARIABLE second_output
    ERROR_VARIABLE second_error
    RESULT_VARIABLE second_result
    TIMEOUT 20)

if(NOT second_result EQUAL 0)
    message(FATAL_ERROR
        "second interactive startup failed: ${second_result}\n${second_error}")
endif()
if(NOT second_output MATCHES "Session: ${first_session}")
    message(FATAL_ERROR "second startup did not resume latest session")
endif()
if(NOT second_output MATCHES "Turns: 0")
    message(FATAL_ERROR "resumed session status was not printed")
endif()
if(second_error)
    message(FATAL_ERROR "unexpected second startup stderr: ${second_error}")
endif()

# A double-click deployment discovers only the sibling active-pack pointer. It
# must not depend on the shell's working directory or copy knowledge assets into
# Ready. These fixtures never invoke the fake sidecar because the scripted
# interaction exits before a model turn.
set(rag_fixture_root "${TEST_ROOT}/rag-pointer")
set(rag_ready "${rag_fixture_root}/out/AgentFramework-Ready")
set(rag_pointer_dir "${rag_fixture_root}/out/AgentFramework-Knowledge")
set(rag_pack "${rag_fixture_root}/external-pack")
set(rag_cwd "${rag_fixture_root}/foreign-working-directory")
set(rag_runtime "${rag_fixture_root}/runtime-data")
file(MAKE_DIRECTORY "${rag_ready}" "${rag_cwd}")
configure_file("${AGENT_EXE}" "${rag_ready}/${exe_name}" COPYONLY)
foreach(dll IN LISTS runtime_dlls)
    get_filename_component(dll_name "${dll}" NAME)
    configure_file("${dll}" "${rag_ready}/${dll_name}" COPYONLY)
endforeach()
file(TO_CMAKE_PATH "${rag_runtime}" rag_runtime_env)
file(WRITE "${rag_ready}/.env"
    "AGENT_BASE_URL=https://provider.invalid\n"
    "AGENT_MODEL=MiniMax-M3\n"
    "AGENT_API_KEY=PROCESS_TEST_SECRET\n"
    "AGENT_RUNTIME_ROOT=${rag_runtime_env}\n"
    "AGENT_ENABLE_RAG=1\n"
    "AGENT_RAG_MODE=hybrid\n"
    "AGENT_RAG_DEVICE=cpu\n")
file(WRITE "${rag_fixture_root}/exit-input.txt" "\n/exit\n")

function(run_rag_startup expected_result expected_error)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${agent_env_unsets}
            "${rag_ready}/${exe_name}"
        WORKING_DIRECTORY "${rag_cwd}"
        INPUT_FILE "${rag_fixture_root}/exit-input.txt"
        OUTPUT_VARIABLE rag_output
        ERROR_VARIABLE rag_error
        RESULT_VARIABLE rag_result
        TIMEOUT 20)
    if(NOT rag_result EQUAL expected_result)
        message(FATAL_ERROR
            "RAG startup returned ${rag_result}, expected ${expected_result}")
    endif()
    if(expected_error)
        string(STRIP "${rag_error}" rag_error)
        if(NOT rag_error STREQUAL expected_error)
            message(FATAL_ERROR "RAG startup error was not stable: ${rag_error}")
        endif()
    elseif(rag_error OR NOT rag_output MATCHES "AgentFramework")
        message(FATAL_ERROR "valid sibling RAG pointer did not start cleanly")
    endif()
    if("${rag_output}${rag_error}" MATCHES "PROCESS_TEST_SECRET")
        message(FATAL_ERROR "credential leaked from RAG startup")
    endif()
endfunction()

run_rag_startup(2 "enabled rag requires an external knowledge pack")
file(MAKE_DIRECTORY "${rag_pointer_dir}")
file(WRITE "${rag_pointer_dir}/active-pack.json" "{corrupt")
run_rag_startup(2 "rag active pack pointer is invalid")

file(MAKE_DIRECTORY "${rag_pack}/runtime" "${rag_pack}/sidecar")
file(WRITE "${rag_pack}/runtime/python.exe" "synthetic runtime")
file(WRITE "${rag_pack}/sidecar/agent_rag_cli.py" "synthetic sidecar")
file(WRITE "${rag_pack}/pack.json"
    "{\"schema_version\":2,\"pack_id\":\"pack-0123456789abcdef0123456789abcdef\","
    "\"snapshot_date\":\"2026-09-03\",\"document_count\":30000,"
    "\"chunk_count\":30000,\"embedding_model\":\"BAAI/bge-m3\","
    "\"embedding_revision\":\"5617a9f61b028005a4858fdac845db406aefb181\","
    "\"embedding_dimensions\":1024,\"relevance_dense_min\":0.0,"
    "\"complete\":true,\"files\":[]}")
file(TO_CMAKE_PATH "${rag_pack}" rag_pack_json)
file(WRITE "${rag_pointer_dir}/active-pack.json"
    "{\"schema_version\":1,\"pack_root\":\"${rag_pack_json}\"}")
run_rag_startup(0 "")

file(GLOB rag_ready_entries RELATIVE "${rag_ready}"
    "${rag_ready}/*" "${rag_ready}/.*")
list(LENGTH rag_ready_entries rag_ready_count)
if(NOT rag_ready_count EQUAL 4 OR
   EXISTS "${rag_ready}/pack.json" OR
   EXISTS "${rag_ready}/runtime" OR
   EXISTS "${rag_ready}/sidecar" OR
   EXISTS "${rag_ready}/corpus" OR
   EXISTS "${rag_ready}/index" OR
   EXISTS "${rag_ready}/model")
    message(FATAL_ERROR "Knowledge Pack assets contaminated the four-file Ready directory")
endif()
