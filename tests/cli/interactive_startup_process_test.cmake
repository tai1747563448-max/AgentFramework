if(NOT DEFINED AGENT_EXE OR NOT DEFINED RUNTIME_DLLS OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "AGENT_EXE, RUNTIME_DLLS, and TEST_ROOT are required")
endif()

# Remove inherited config before launching with synthetic values.
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

# Remove inherited AGENT_* variables without logging their values.
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
