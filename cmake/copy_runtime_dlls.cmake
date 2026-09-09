if(NOT DEFINED TARGET_DIR OR NOT IS_DIRECTORY "${TARGET_DIR}")
    message(FATAL_ERROR "runtime DLL target directory is invalid")
endif()

if(NOT DEFINED DLLS OR DLLS STREQUAL "")
    return()
endif()

string(REPLACE "|" ";" dll_list "${DLLS}")
foreach(dll IN LISTS dll_list)
    if(NOT EXISTS "${dll}")
        message(FATAL_ERROR "runtime DLL is unavailable")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "${TARGET_DIR}"
        RESULT_VARIABLE copy_result)
    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "runtime DLL copy failed")
    endif()
endforeach()
