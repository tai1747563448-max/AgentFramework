if(NOT DEFINED AGENT_EXE OR NOT DEFINED EVENT_LOG OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "resume process test is missing an input")
endif()

set(task_id "task-0000000000000000000000000000000b")
set(runtime_root "${TEST_ROOT}/runtime")
set(task_root "${runtime_root}/tasks/${task_id}")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${task_root}")
configure_file("${EVENT_LOG}" "${task_root}/events.jsonl" COPYONLY)

set(ENV{AGENT_BASE_URL} "https://provider.invalid")
set(ENV{AGENT_MODEL} "offline-resume-model")
set(ENV{AGENT_API_KEY} "offline-resume-credential")
unset(ENV{AGENT_AUTH_TOKEN})
set(ENV{AGENT_RUNTIME_ROOT} "${runtime_root}")
unset(ENV{AGENT_ENABLE_BUILD_TOOLS})
unset(ENV{AGENT_ENABLE_RAG})

execute_process(
    COMMAND "${AGENT_EXE}" resume --task-id "${task_id}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE resume_result
    OUTPUT_VARIABLE resume_output
    ERROR_VARIABLE resume_error)

if(NOT resume_result EQUAL 0)
    message(FATAL_ERROR
        "terminal resume returned ${resume_result}: ${resume_error}")
endif()
if(NOT resume_output MATCHES "^verified[\r\n]+$")
    message(FATAL_ERROR "terminal resume output was unexpected")
endif()
if(NOT resume_error STREQUAL "")
    message(FATAL_ERROR "terminal resume wrote unexpected stderr")
endif()

file(READ "${task_root}/events.jsonl" resumed_log)
file(READ "${EVENT_LOG}" source_log)
if(NOT resumed_log STREQUAL source_log)
    message(FATAL_ERROR "terminal resume modified the durable event log")
endif()

set(mismatched_task_id "task-0000000000000000000000000000000c")
set(mismatched_root "${runtime_root}/tasks/${mismatched_task_id}")
file(MAKE_DIRECTORY "${mismatched_root}")
configure_file("${EVENT_LOG}" "${mismatched_root}/events.jsonl" COPYONLY)
execute_process(
    COMMAND "${AGENT_EXE}" resume --task-id "${mismatched_task_id}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE mismatch_result
    OUTPUT_VARIABLE mismatch_output
    ERROR_VARIABLE mismatch_error)
if(NOT mismatch_result EQUAL 6)
    message(FATAL_ERROR
        "mismatched task log returned ${mismatch_result}, expected 6")
endif()
if(NOT mismatch_output STREQUAL "")
    message(FATAL_ERROR "mismatched task log wrote unexpected stdout")
endif()
if(mismatch_error MATCHES "0000000b|0000000c")
    message(FATAL_ERROR "mismatched task log reflected a task identifier")
endif()

set(linked_runtime_root "${TEST_ROOT}/linked-runtime")
set(linked_task_root "${linked_runtime_root}/tasks/${task_id}")
file(MAKE_DIRECTORY "${linked_task_root}")
file(CREATE_LINK "${EVENT_LOG}" "${linked_task_root}/events.jsonl"
    RESULT link_result)
if(NOT link_result STREQUAL "0")
    message(FATAL_ERROR "failed to create mandatory resume hard-link fixture")
endif()
set(ENV{AGENT_RUNTIME_ROOT} "${linked_runtime_root}")
execute_process(
    COMMAND "${AGENT_EXE}" resume --task-id "${task_id}"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE linked_result
    OUTPUT_VARIABLE linked_output
    ERROR_VARIABLE linked_error)
if(NOT linked_result EQUAL 6)
    message(FATAL_ERROR
        "hard-linked resume log returned ${linked_result}, expected 6")
endif()
if(NOT linked_output STREQUAL "")
    message(FATAL_ERROR "hard-linked resume log wrote unexpected stdout")
endif()
if(linked_error MATCHES "${task_id}")
    message(FATAL_ERROR "hard-linked resume log reflected a task identifier")
endif()
