if(NOT DEFINED AGENT_BENCHMARK_EXE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "benchmark process test requires executable and root")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(BASELINE "${TEST_ROOT}/baseline.json")
set(CURRENT "${TEST_ROOT}/current.json")

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 5
        --output "${BASELINE}"
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_output
    ERROR_VARIABLE baseline_error)
if(NOT baseline_result EQUAL 0)
    message(FATAL_ERROR
        "initial benchmark returned ${baseline_result}: ${baseline_error}")
endif()
if(NOT EXISTS "${BASELINE}")
    message(FATAL_ERROR "initial benchmark did not create its JSON report")
endif()
file(READ "${BASELINE}" baseline_json)
foreach(required
        "\"schema_version\": 1"
        "\"benchmark_kind\": \"controlled_offline_agent_runtime\""
        "\"text_completion_runtime\""
        "\"tool_round_trip_runtime\""
        "\"event_log_evaluation\""
        "\"p95_latency_us\""
        "\"throughput_ops_per_second\""
        "\"success_rate\""
        "\"limitations\""
        "\"working_tree_dirty\""
        "\"scripted_provider\": true")
    string(FIND "${baseline_json}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "benchmark report is missing ${required}")
    endif()
endforeach()
string(FIND "${baseline_output}" "comparison=not_requested" no_comparison)
if(no_comparison EQUAL -1 OR NOT baseline_error STREQUAL "")
    message(FATAL_ERROR "initial benchmark summary was unexpected")
endif()

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 5
        --baseline "${BASELINE}"
        --max-regression-percent 1000000
        --output "${CURRENT}"
    RESULT_VARIABLE comparison_result
    OUTPUT_VARIABLE comparison_output
    ERROR_VARIABLE comparison_error)
if(NOT comparison_result EQUAL 0)
    message(FATAL_ERROR
        "baseline comparison returned ${comparison_result}: ${comparison_error}")
endif()
string(FIND "${comparison_output}" "comparison=pass" comparison_pass)
if(comparison_pass EQUAL -1 OR NOT comparison_error STREQUAL "")
    message(FATAL_ERROR "benchmark comparison summary was unexpected")
endif()
file(READ "${CURRENT}" current_json)
string(FIND "${current_json}" "\"requested\": true" requested)
string(FIND "${current_json}" "\"passed\": true" passed)
if(requested EQUAL -1 OR passed EQUAL -1)
    message(FATAL_ERROR "comparison report did not record a passing baseline")
endif()

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 0
        --output "${TEST_ROOT}/invalid.json"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_output
    ERROR_VARIABLE invalid_error)
if(NOT invalid_result EQUAL 2)
    message(FATAL_ERROR "zero iterations returned ${invalid_result}, expected 2")
endif()
if(NOT invalid_output STREQUAL "")
    message(FATAL_ERROR "invalid benchmark wrote unexpected stdout")
endif()
string(FIND "${invalid_error}" "invalid benchmark arguments" invalid_message)
if(invalid_message EQUAL -1)
    message(FATAL_ERROR "invalid benchmark did not use its fixed error message")
endif()
