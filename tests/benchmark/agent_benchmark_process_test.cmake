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
string(JSON baseline_schema GET "${baseline_json}" schema_version)
string(JSON baseline_kind GET "${baseline_json}" benchmark_kind)
string(JSON batch_size GET "${baseline_json}" parameters batch_size)
string(JSON total_operations GET "${baseline_json}" parameters total_operations)
string(JSON scenario_count LENGTH "${baseline_json}" scenarios)
if(NOT baseline_schema EQUAL 2 OR
   NOT baseline_kind STREQUAL "controlled_offline_agent_runtime" OR
   NOT batch_size EQUAL 1000 OR NOT total_operations EQUAL 5000 OR
   NOT scenario_count EQUAL 3)
    message(FATAL_ERROR "benchmark report header contract was unexpected")
endif()
foreach(index RANGE 0 2)
    string(JSON sample_count GET "${baseline_json}"
        scenarios ${index} summary sample_count)
    string(JSON operation_count GET "${baseline_json}"
        scenarios ${index} summary operation_count)
    string(JSON success_count GET "${baseline_json}"
        scenarios ${index} summary success_count)
    string(JSON error_count GET "${baseline_json}"
        scenarios ${index} summary error_count)
    string(JSON total_duration GET "${baseline_json}"
        scenarios ${index} summary total_duration_us)
    string(JSON mean_latency GET "${baseline_json}"
        scenarios ${index} summary mean_latency_us)
    string(JSON p50_latency GET "${baseline_json}"
        scenarios ${index} summary p50_latency_us)
    string(JSON p95_latency GET "${baseline_json}"
        scenarios ${index} summary p95_latency_us)
    string(JSON p99_latency GET "${baseline_json}"
        scenarios ${index} summary p99_latency_us)
    string(JSON max_latency GET "${baseline_json}"
        scenarios ${index} summary max_latency_us)
    string(JSON throughput GET "${baseline_json}"
        scenarios ${index} summary throughput_ops_per_second)
    string(JSON success_rate GET "${baseline_json}"
        scenarios ${index} summary success_rate)
    if(NOT sample_count EQUAL 5 OR NOT operation_count EQUAL 5000 OR
       NOT success_count EQUAL 5000 OR NOT error_count EQUAL 0 OR
       total_duration STREQUAL "" OR mean_latency STREQUAL "" OR
       p50_latency STREQUAL "" OR p95_latency STREQUAL "" OR
       p99_latency STREQUAL "" OR max_latency STREQUAL "" OR
       throughput STREQUAL "" OR NOT success_rate EQUAL 1)
        message(FATAL_ERROR
            "benchmark scenario ${index} metric contract was unexpected")
    endif()
endforeach()
string(JSON scripted_provider GET "${baseline_json}"
    methodology scripted_provider)
string(JSON external_network GET "${baseline_json}"
    methodology external_network)
string(JSON working_tree_dirty GET "${baseline_json}"
    environment working_tree_dirty)
string(JSON limitations_count LENGTH "${baseline_json}" limitations)
if(NOT scripted_provider OR external_network OR limitations_count LESS 3 OR
   working_tree_dirty STREQUAL "")
    message(FATAL_ERROR "benchmark methodology contract was unexpected")
endif()
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
string(JSON requested GET "${current_json}" comparison requested)
string(JSON passed GET "${current_json}" comparison passed)
string(JSON compatibility_validated GET "${current_json}"
    comparison compatibility_validated)
string(JSON baseline_sha256 GET "${current_json}" comparison baseline sha256)
file(SHA256 "${BASELINE}" expected_baseline_sha256)
if(NOT requested OR NOT passed OR NOT compatibility_validated OR
   NOT baseline_sha256 STREQUAL expected_baseline_sha256)
    message(FATAL_ERROR "comparison report did not record a passing baseline")
endif()

set(REGRESSION_BASELINE "${TEST_ROOT}/regression-baseline.json")
set(regression_json "${baseline_json}")
foreach(index RANGE 0 2)
    foreach(metric min_latency_us mean_latency_us p50_latency_us
                   p95_latency_us p99_latency_us max_latency_us)
        string(JSON regression_json SET "${regression_json}"
            scenarios ${index} summary ${metric} 0.001)
    endforeach()
    string(JSON regression_json SET "${regression_json}"
        scenarios ${index} summary total_duration_us 5.0)
    string(JSON regression_json SET "${regression_json}"
        scenarios ${index} summary throughput_ops_per_second 1000000000.0)
endforeach()
file(WRITE "${REGRESSION_BASELINE}" "${regression_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 5
        --baseline "${REGRESSION_BASELINE}"
        --max-regression-percent 15
        --output "${TEST_ROOT}/regression-current.json"
    RESULT_VARIABLE regression_result
    OUTPUT_VARIABLE regression_output
    ERROR_VARIABLE regression_error)
if(NOT regression_result EQUAL 3 OR NOT regression_error STREQUAL "")
    message(FATAL_ERROR
        "deterministic regression returned ${regression_result}, expected 3")
endif()
string(FIND "${regression_output}" "comparison=fail" regression_failed)
file(READ "${TEST_ROOT}/regression-current.json" regression_current_json)
string(JSON regression_passed GET "${regression_current_json}"
    comparison passed)
if(regression_failed EQUAL -1 OR regression_passed)
    message(FATAL_ERROR "regression report did not record the failed gate")
endif()

string(JSON wrong_schema_json SET "${baseline_json}" schema_version 999)
file(WRITE "${TEST_ROOT}/wrong-schema.json" "${wrong_schema_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/wrong-schema.json"
        --output "${TEST_ROOT}/wrong-schema-current.json"
    RESULT_VARIABLE wrong_schema_result)
if(NOT wrong_schema_result EQUAL 2)
    message(FATAL_ERROR
        "unknown baseline schema returned ${wrong_schema_result}, expected 2")
endif()

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5 --batch-size 999
        --baseline "${BASELINE}"
        --output "${TEST_ROOT}/wrong-batch-current.json"
    RESULT_VARIABLE wrong_batch_result)
if(NOT wrong_batch_result EQUAL 2)
    message(FATAL_ERROR
        "incompatible batch returned ${wrong_batch_result}, expected 2")
endif()

string(JSON missing_scenario_json REMOVE "${baseline_json}" scenarios 2)
file(WRITE "${TEST_ROOT}/missing-scenario.json" "${missing_scenario_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/missing-scenario.json"
        --output "${TEST_ROOT}/missing-scenario-current.json"
    RESULT_VARIABLE missing_scenario_result)
if(NOT missing_scenario_result EQUAL 2)
    message(FATAL_ERROR
        "missing scenario returned ${missing_scenario_result}, expected 2")
endif()

string(JSON first_scenario GET "${baseline_json}" scenarios 0)
set(duplicate_scenario_json "${baseline_json}")
string(JSON duplicate_scenario_json SET "${duplicate_scenario_json}"
    scenarios 2 "${first_scenario}")
file(WRITE "${TEST_ROOT}/duplicate-scenario.json"
    "${duplicate_scenario_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/duplicate-scenario.json"
        --output "${TEST_ROOT}/duplicate-scenario-current.json"
    RESULT_VARIABLE duplicate_scenario_result)
if(NOT duplicate_scenario_result EQUAL 2)
    message(FATAL_ERROR
        "duplicate scenario returned ${duplicate_scenario_result}, expected 2")
endif()

set(zero_throughput_json "${baseline_json}")
string(JSON zero_throughput_json SET "${zero_throughput_json}"
    scenarios 0 summary throughput_ops_per_second 0.0)
file(WRITE "${TEST_ROOT}/zero-throughput.json" "${zero_throughput_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/zero-throughput.json"
        --output "${TEST_ROOT}/zero-throughput-current.json"
    RESULT_VARIABLE zero_throughput_result)
if(NOT zero_throughput_result EQUAL 2)
    message(FATAL_ERROR
        "zero throughput returned ${zero_throughput_result}, expected 2")
endif()

set(fractional_count_json "${baseline_json}")
string(JSON fractional_count_json SET "${fractional_count_json}"
    scenarios 0 summary operation_count 5000.5)
file(WRITE "${TEST_ROOT}/fractional-count.json" "${fractional_count_json}\n")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/fractional-count.json"
        --output "${TEST_ROOT}/fractional-count-current.json"
    RESULT_VARIABLE fractional_count_result)
if(NOT fractional_count_result EQUAL 2)
    message(FATAL_ERROR
        "fractional operation count returned ${fractional_count_result}, expected 2")
endif()

file(WRITE "${TEST_ROOT}/malformed.json" "{not-json")
execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1 --iterations 5
        --baseline "${TEST_ROOT}/malformed.json"
        --output "${TEST_ROOT}/malformed-current.json"
    RESULT_VARIABLE malformed_result)
if(NOT malformed_result EQUAL 2)
    message(FATAL_ERROR
        "malformed baseline returned ${malformed_result}, expected 2")
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

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 5
        --batch-size 0
        --output "${TEST_ROOT}/invalid-batch.json"
    RESULT_VARIABLE invalid_batch_result
    OUTPUT_VARIABLE invalid_batch_output
    ERROR_VARIABLE invalid_batch_error)
if(NOT invalid_batch_result EQUAL 2 OR NOT invalid_batch_output STREQUAL "")
    message(FATAL_ERROR "zero batch size was not rejected")
endif()
string(FIND "${invalid_batch_error}" "invalid benchmark arguments"
    invalid_batch_message)
if(invalid_batch_message EQUAL -1)
    message(FATAL_ERROR "invalid batch size did not use its fixed error message")
endif()

execute_process(
    COMMAND "${AGENT_BENCHMARK_EXE}"
        --warmup 1
        --iterations 18446744073709551615
        --batch-size 2
        --output "${TEST_ROOT}/overflow.json"
    RESULT_VARIABLE overflow_result)
if(NOT overflow_result EQUAL 2)
    message(FATAL_ERROR
        "iteration multiplication overflow returned ${overflow_result}, expected 2")
endif()
