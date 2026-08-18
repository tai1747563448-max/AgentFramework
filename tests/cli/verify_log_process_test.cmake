if(NOT DEFINED AGENT_EXE OR NOT DEFINED EVENT_LOG OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "process verification test is missing an input")
endif()

unset(ENV{AGENT_BASE_URL})
unset(ENV{AGENT_MODEL})
unset(ENV{AGENT_API_KEY})
unset(ENV{AGENT_AUTH_TOKEN})
unset(ENV{AGENT_RUNTIME_ROOT})
unset(ENV{AGENT_SYSTEM_PROMPT})

file(MAKE_DIRECTORY "${TEST_ROOT}")
set(poison_env "${TEST_ROOT}/.env")
file(WRITE "${poison_env}"
    "this invalid dotenv syntax must never be parsed\n")

execute_process(
    COMMAND "${AGENT_EXE}" verify-log --events "${EVENT_LOG}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_output
    ERROR_VARIABLE verify_error)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR
        "credential-free verify-log returned ${verify_result}: ${verify_error}")
endif()
if(NOT verify_output MATCHES
       "^task_id=task-0000000000000000000000000000000b status=Completed last_sequence=6")
    message(FATAL_ERROR "credential-free verify-log summary was unexpected")
endif()
if(NOT verify_error STREQUAL "")
    message(FATAL_ERROR "credential-free verify-log wrote unexpected stderr")
endif()

execute_process(
    COMMAND "${AGENT_EXE}" verify-log --events "${EVENT_LOG}"
            --env-file "${poison_env}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE combined_result
    OUTPUT_VARIABLE combined_output
    ERROR_VARIABLE combined_error)
if(NOT combined_result EQUAL 2)
    message(FATAL_ERROR
        "verify-log plus --env-file returned ${combined_result}, expected 2")
endif()
if(NOT combined_output STREQUAL "")
    message(FATAL_ERROR "invalid combined command wrote unexpected stdout")
endif()
