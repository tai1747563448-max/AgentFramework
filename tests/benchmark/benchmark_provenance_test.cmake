if(NOT DEFINED PROVENANCE_SCRIPT OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "benchmark provenance test requires script and root")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY
    "${TEST_ROOT}/src"
    "${TEST_ROOT}/benchmarks"
    "${TEST_ROOT}/tests/application"
    "${TEST_ROOT}/tests/benchmark")
file(WRITE "${TEST_ROOT}/CMakeLists.txt" "project(provenance)\n")
file(WRITE "${TEST_ROOT}/src/value.cpp" "int value = 1;\n")
file(WRITE "${TEST_ROOT}/benchmarks/agent_runtime_benchmark.cpp"
    "int main() { return 0; }\n")
file(WRITE "${TEST_ROOT}/tests/application/benchmark_statistics_test.cpp"
    "test\n")
file(WRITE "${TEST_ROOT}/tests/benchmark/agent_benchmark_process_test.cmake"
    "test\n")

execute_process(
    COMMAND git init
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE git_init_result
    OUTPUT_QUIET ERROR_VARIABLE git_init_error)
execute_process(
    COMMAND git config user.email benchmark-test@example.invalid
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE git_email_result)
execute_process(
    COMMAND git config user.name "Benchmark Test"
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE git_name_result)
execute_process(
    COMMAND git add .
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE git_add_result)
execute_process(
    COMMAND git -c commit.gpgsign=false commit -m initial
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE git_commit_result
    OUTPUT_QUIET ERROR_VARIABLE git_commit_error)
if(NOT git_init_result EQUAL 0 OR NOT git_email_result EQUAL 0 OR
   NOT git_name_result EQUAL 0 OR NOT git_add_result EQUAL 0 OR
   NOT git_commit_result EQUAL 0)
    message(FATAL_ERROR
        "could not create provenance fixture: ${git_init_error}${git_commit_error}")
endif()

set(HEADER "${TEST_ROOT}/generated/benchmark_provenance.h")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DAGENT_SOURCE_DIR=${TEST_ROOT}"
        "-DAGENT_OUTPUT_FILE=${HEADER}"
        -P "${PROVENANCE_SCRIPT}"
    RESULT_VARIABLE clean_generation_result)
if(NOT clean_generation_result EQUAL 0 OR NOT EXISTS "${HEADER}")
    message(FATAL_ERROR "clean provenance header was not generated")
endif()
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${TEST_ROOT}"
    OUTPUT_VARIABLE first_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE)
file(READ "${HEADER}" clean_header)
string(FIND "${clean_header}"
    "#define AGENT_GIT_COMMIT \"${first_commit}\"" clean_commit_found)
string(FIND "${clean_header}" "#define AGENT_GIT_DIRTY 0" clean_dirty_found)
if(clean_commit_found EQUAL -1 OR clean_dirty_found EQUAL -1)
    message(FATAL_ERROR "clean provenance header was inaccurate")
endif()

file(APPEND "${TEST_ROOT}/src/value.cpp" "int changed = 2;\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DAGENT_SOURCE_DIR=${TEST_ROOT}"
        "-DAGENT_OUTPUT_FILE=${HEADER}"
        -P "${PROVENANCE_SCRIPT}"
    RESULT_VARIABLE dirty_generation_result)
file(READ "${HEADER}" dirty_header)
string(FIND "${dirty_header}" "#define AGENT_GIT_DIRTY 1" dirty_found)
if(NOT dirty_generation_result EQUAL 0 OR dirty_found EQUAL -1)
    message(FATAL_ERROR "dirty provenance header was inaccurate")
endif()

execute_process(
    COMMAND git add src/value.cpp
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE changed_add_result)
execute_process(
    COMMAND git -c commit.gpgsign=false commit -m changed
    WORKING_DIRECTORY "${TEST_ROOT}"
    RESULT_VARIABLE changed_commit_result
    OUTPUT_QUIET ERROR_VARIABLE changed_commit_error)
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${TEST_ROOT}"
    OUTPUT_VARIABLE second_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DAGENT_SOURCE_DIR=${TEST_ROOT}"
        "-DAGENT_OUTPUT_FILE=${HEADER}"
        -P "${PROVENANCE_SCRIPT}"
    RESULT_VARIABLE second_generation_result)
file(READ "${HEADER}" second_header)
string(FIND "${second_header}"
    "#define AGENT_GIT_COMMIT \"${second_commit}\"" second_commit_found)
string(FIND "${second_header}" "#define AGENT_GIT_DIRTY 0" second_clean_found)
if(NOT changed_add_result EQUAL 0 OR NOT changed_commit_result EQUAL 0 OR
   NOT second_generation_result EQUAL 0 OR
   first_commit STREQUAL second_commit OR second_commit_found EQUAL -1 OR
   second_clean_found EQUAL -1)
    message(FATAL_ERROR
        "updated provenance header was inaccurate: ${changed_commit_error}")
endif()
