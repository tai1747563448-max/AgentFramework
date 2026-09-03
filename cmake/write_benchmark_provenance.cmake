if(NOT DEFINED AGENT_SOURCE_DIR OR NOT DEFINED AGENT_OUTPUT_FILE)
    message(FATAL_ERROR "benchmark provenance requires source and output paths")
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${AGENT_SOURCE_DIR}"
    RESULT_VARIABLE commit_result
    OUTPUT_VARIABLE git_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT commit_result EQUAL 0 OR git_commit STREQUAL "")
    set(git_commit "unknown")
endif()

execute_process(
    COMMAND git status --porcelain --
        CMakeLists.txt
        cmake
        src
        benchmarks/agent_runtime_benchmark.cpp
        tests/application/benchmark_statistics_test.cpp
        tests/benchmark
    WORKING_DIRECTORY "${AGENT_SOURCE_DIR}"
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(status_result EQUAL 0 AND git_status STREQUAL "")
    set(git_dirty 0)
else()
    set(git_dirty 1)
endif()

set(header_contents
    "#pragma once\n\n#define AGENT_GIT_COMMIT \"${git_commit}\"\n#define AGENT_GIT_DIRTY ${git_dirty}\n")
get_filename_component(output_directory "${AGENT_OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
if(EXISTS "${AGENT_OUTPUT_FILE}")
    file(READ "${AGENT_OUTPUT_FILE}" existing_contents)
else()
    set(existing_contents "")
endif()
if(NOT existing_contents STREQUAL header_contents)
    file(WRITE "${AGENT_OUTPUT_FILE}" "${header_contents}")
endif()
