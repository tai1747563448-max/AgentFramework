cmake_minimum_required(VERSION 3.21)

foreach(required SOURCE_DIR POWERSHELL_EXE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Knowledge Pack build contract is missing ${required}")
    endif()
endforeach()

set(script "${SOURCE_DIR}/scripts/build_ecfr_knowledge_pack.ps1")
if(NOT EXISTS "${script}")
    message(FATAL_ERROR "Knowledge Pack build script is missing")
endif()

function(expect_preflight_failure label expected)
    execute_process(
        COMMAND "${POWERSHELL_EXE}" -NoProfile -ExecutionPolicy Bypass
            -File "${script}" ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 15)
    if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
        message(FATAL_ERROR "Knowledge Pack preflight accepted ${label}")
    endif()
endfunction()

expect_preflight_failure("a relative root" "KnowledgeRoot must be absolute"
    -KnowledgeRoot relative-pack -Snapshot 2026-09-03 -DocumentCount 30000)
expect_preflight_failure("a repository-local root" "KnowledgeRoot must be outside"
    -KnowledgeRoot "${SOURCE_DIR}/pack" -Snapshot 2026-09-03
    -DocumentCount 30000)
expect_preflight_failure("the wrong snapshot" "ValidateSet"
    -KnowledgeRoot "${SOURCE_DIR}/../pack" -Snapshot 2026-09-04
    -DocumentCount 30000)
expect_preflight_failure("the wrong document count" "ValidateSet"
    -KnowledgeRoot "${SOURCE_DIR}/../pack" -Snapshot 2026-09-03
    -DocumentCount 29999)

file(READ "${script}" script_text LIMIT 1048576)
foreach(required_text
        "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip"
        "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b"
        "5617a9f61b028005a4858fdac845db406aefb181"
        "runtime.lock.json"
        "model.lock.json"
        "active-pack.json"
        "verify_complete_pack"
        "PRAGMA integrity_check")
    string(FIND "${script_text}" "${required_text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Knowledge Pack script omits ${required_text}")
    endif()
endforeach()

message(STATUS "Knowledge Pack build script preflight contract passed")
