cmake_minimum_required(VERSION 3.21)
project(AgentFramework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS_SSL OFF CACHE BOOL "" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.14.2
    GIT_SHALLOW TRUE)
FetchContent_Declare(
    dotenv_source
    GIT_REPOSITORY https://github.com/laserpants/dotenv-cpp.git
    GIT_TAG 9275210b8abf1a551fb81e0ba45866286d764acd
    SOURCE_SUBDIR _header_only)
FetchContent_MakeAvailable(nlohmann_json cpr dotenv_source)

add_library(dotenv_header_only INTERFACE)
target_include_directories(dotenv_header_only INTERFACE
    "${dotenv_source_SOURCE_DIR}/include/laserpants/dotenv")

add_library(agent_kernel STATIC
    src/domain/value.cpp
    src/domain/runtime_event.cpp
    src/application/state_reducer.cpp
    src/adapters/json/value_json.cpp
    src/adapters/persistence/event_json.cpp
    src/adapters/persistence/jsonl_event_store.cpp)
target_include_directories(agent_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(agent_kernel PUBLIC nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(agent_kernel PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
function(add_agent_test target source)
    add_executable(${target} tests/test_main.cpp ${source})
    target_link_libraries(${target} PRIVATE agent_kernel)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()
add_agent_test(domain_primitives_tests tests/domain/value_result_test.cpp)
add_agent_test(runtime_event_tests tests/domain/runtime_event_test.cpp)
add_agent_test(state_reducer_tests tests/application/state_reducer_test.cpp)
add_agent_test(jsonl_event_store_tests tests/adapters/jsonl_event_store_test.cpp)
