cmake_minimum_required(VERSION 3.21)

foreach(required SOURCE_DIR BINARY_DIR AGENT_EXE RUNTIME_DLLS PROCESS_TEST_EXE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Ready package contract test is missing ${required}")
    endif()
endforeach()

set(stage_script "${SOURCE_DIR}/cmake/stage_ready_package.cmake")
set(verify_script "${SOURCE_DIR}/cmake/verify_ready_package.cmake")
if(NOT EXISTS "${stage_script}" OR NOT EXISTS "${verify_script}")
    message(FATAL_ERROR "Ready package support is missing: staging and verification scripts are required")
endif()

string(REPLACE "|" ";" dlls "${RUNTIME_DLLS}")
list(LENGTH dlls runtime_dll_count)
if(NOT runtime_dll_count EQUAL 2)
    message(FATAL_ERROR
        "Ready must have exactly two runtime DLLs; got ${runtime_dll_count}")
endif()

# Modify only unique synthetic test data.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef fixture_id)
set(fixture_root "${BINARY_DIR}/ready-package-contract-${fixture_id}")
set(fixture "${fixture_root}/package")
file(MAKE_DIRECTORY "${fixture}")
file(MAKE_DIRECTORY "${fixture_root}/forbidden-destination")
file(WRITE "${fixture_root}/forbidden-destination/sentinel.txt" "preserve this synthetic file\n")
file(WRITE "${fixture_root}/chosen.cfg" "SYNTHETIC_PACKAGE_CONFIG=1\n")

function(expect_stage_failure label expected_message)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DAGENT_READY_EXE=${AGENT_EXE}"
        "-DAGENT_READY_DLLS=${RUNTIME_DLLS}"
        "-DAGENT_READY_CONFIG=Release"
        "-DAGENT_READY_ENV_FILE=${fixture_root}/chosen.cfg"
        ${ARGN} -P "${stage_script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_message}")
        message(FATAL_ERROR "Ready staging preflight did not reject ${label}")
    endif()
endfunction()

expect_stage_failure("an unrelated deletion target" "exact Ready staging directory"
    "-DAGENT_READY_DIR=${fixture_root}/forbidden-destination")
if(NOT EXISTS "${fixture_root}/forbidden-destination/sentinel.txt")
    message(FATAL_ERROR "Rejected staging path was modified")
endif()
expect_stage_failure("a missing environment file" "environment file is required"
    "-DAGENT_READY_ENV_FILE=${fixture_root}/absent.cfg")
expect_stage_failure("a Debug artifact" "Release build is required"
    "-DAGENT_READY_CONFIG=Debug")

# Verify staging preserves unmanaged runtime state.
set(synthetic_source "${fixture_root}/synthetic-source")
set(synthetic_stage "${synthetic_source}/out/AgentFramework-Ready")
file(MAKE_DIRECTORY "${synthetic_source}/cmake")
file(MAKE_DIRECTORY "${synthetic_stage}/runtime_data")
configure_file("${stage_script}"
    "${synthetic_source}/cmake/stage_ready_package.cmake" COPYONLY)
file(WRITE "${synthetic_stage}/runtime_data/user-state.jsonl"
    "synthetic user-owned runtime state\n")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DAGENT_READY_EXE=${AGENT_EXE}"
    "-DAGENT_READY_DLLS=${RUNTIME_DLLS}"
    "-DAGENT_READY_CONFIG=Release"
    "-DAGENT_READY_ENV_FILE=${fixture_root}/chosen.cfg"
    -P "${synthetic_source}/cmake/stage_ready_package.cmake"
    RESULT_VARIABLE sentinel_result
    OUTPUT_VARIABLE sentinel_output
    ERROR_VARIABLE sentinel_error
    TIMEOUT 10)
if(sentinel_result EQUAL 0)
    message(FATAL_ERROR
        "Ready staging replaced a directory containing unmanaged runtime data")
endif()
if(NOT EXISTS "${synthetic_stage}/runtime_data/user-state.jsonl")
    message(FATAL_ERROR "Ready staging removed unmanaged runtime data")
endif()
file(RENAME "${synthetic_stage}/runtime_data"
    "${fixture_root}/preserved-runtime-data")

# Replace hard-link entries without overwriting their shared targets.
set(external_managed_target "${fixture_root}/external-managed-target.bin")
file(WRITE "${external_managed_target}" "preserve linked external bytes\n")
file(CREATE_LINK "${external_managed_target}"
    "${synthetic_stage}/AgentFramework.exe" RESULT hardlink_result)
if(hardlink_result)
    message(FATAL_ERROR "failed to create synthetic Ready hard-link fixture")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DAGENT_READY_EXE=${AGENT_EXE}"
    "-DAGENT_READY_DLLS=${RUNTIME_DLLS}"
    "-DAGENT_READY_CONFIG=Release"
    "-DAGENT_READY_ENV_FILE=${fixture_root}/chosen.cfg"
    -P "${synthetic_source}/cmake/stage_ready_package.cmake"
    RESULT_VARIABLE hardlink_stage_result
    OUTPUT_VARIABLE hardlink_stage_output
    ERROR_VARIABLE hardlink_stage_error
    TIMEOUT 10)
if(NOT hardlink_stage_result EQUAL 0)
    message(FATAL_ERROR
        "Ready staging rejected a replaceable managed hard link: "
        "${hardlink_stage_output}${hardlink_stage_error}")
endif()
file(READ "${external_managed_target}" external_managed_bytes)
if(NOT external_managed_bytes STREQUAL "preserve linked external bytes\n")
    message(FATAL_ERROR "Ready staging overwrote a managed hard-link target")
endif()

get_filename_component(exe_name "${AGENT_EXE}" NAME)
configure_file("${AGENT_EXE}" "${fixture}/${exe_name}" COPYONLY)
foreach(dll IN LISTS dlls)
    get_filename_component(dll_name "${dll}" NAME)
    configure_file("${dll}" "${fixture}/${dll_name}" COPYONLY)
endforeach()
configure_file("${fixture_root}/chosen.cfg" "${fixture}/.env" COPYONLY)

function(verify_fixture should_pass)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DAGENT_READY_EXE=${AGENT_EXE}"
        "-DAGENT_READY_DLLS=${RUNTIME_DLLS}"
        "-DAGENT_READY_CONFIG=Release"
        "-DAGENT_READY_DIR=${fixture}"
        "-DAGENT_PROCESS_TEST_EXE=${PROCESS_TEST_EXE}"
        -P "${verify_script}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(should_pass AND NOT result EQUAL 0)
        message(FATAL_ERROR "Credential-free package startup verification failed: ${output}${error}")
    elseif(NOT should_pass AND result EQUAL 0)
        message(FATAL_ERROR "Package verification accepted a damaged or contaminated package")
    endif()
endfunction()

verify_fixture(TRUE)
file(WRITE "${fixture}/fifth-unmanaged-file.bin" "must be rejected")
verify_fixture(FALSE)
file(REMOVE "${fixture}/fifth-unmanaged-file.bin")
file(MAKE_DIRECTORY "${fixture}/runtime_data")
verify_fixture(FALSE)
file(RENAME "${fixture}/runtime_data" "${fixture_root}/rejected-runtime-data")
file(RENAME "${fixture}/.env" "${fixture_root}/removed-env")
verify_fixture(FALSE)
file(RENAME "${fixture_root}/removed-env" "${fixture}/.env")
file(APPEND "${fixture}/${exe_name}" "synthetic-byte-mismatch")
verify_fixture(FALSE)
configure_file("${AGENT_EXE}" "${fixture}/${exe_name}" COPYONLY)
list(GET dlls 0 first_dll)
get_filename_component(dll_name "${first_dll}" NAME)
file(RENAME "${fixture}/${dll_name}" "${fixture_root}/${dll_name}")
verify_fixture(FALSE)

message(STATUS "Ready package contract passed: preflight, exact inventory, hashes, missing env/DLL, and isolated startup")
