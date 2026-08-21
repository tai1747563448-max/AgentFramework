#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace fixtures {

agent::ToolCall call(std::string id,
                     std::string name,
                     agent::Value::Object arguments) {
    return {std::move(id), std::move(name),
            agent::Value::object(std::move(arguments))};
}

nlohmann::json content_json(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    return nlohmann::json::parse(result.value().content);
}

std::string error_code(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
    return content_json(result).at("error").at("code").get<std::string>();
}

}  // namespace fixtures

TEST_CASE(real_cmake_gateway_configures_builds_tests_and_returns_failure_evidence) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"真实构建-🙂"));
    temp.write_text(
        "CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(GatewayFixture LANGUAGES CXX)\n"
        "enable_testing()\n"
        "add_executable(tiny_test tiny_test.cpp)\n"
        "add_test(NAME \"tiny.pass[1]\" COMMAND tiny_test)\n");
    temp.write_text("tiny_test.cpp", "int main() { return 0; }\n");

    agent::DirectProcessRunner process;
    agent::CMakeToolGateway build_gateway(process, 300'000);
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    auto result = build_gateway.execute(
        fixtures::call("configure", "configure_project",
                       {{"configuration", "Debug"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(fixtures::content_json(result).at("exit_code") == 0);

    result = build_gateway.execute(
        fixtures::call("build-pass", "build_project",
                       {{"configuration", "Debug"},
                        {"target", "tiny_test"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(fixtures::content_json(result).at("exit_code") == 0);

    result = build_gateway.execute(
        fixtures::call("test-pass", "run_tests",
                       {{"configuration", "Debug"},
                        {"test_name", "tiny.pass[1]"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(fixtures::content_json(result).at("exit_code") == 0);

    temp.write_text("tiny_test.cpp", "int main() { return 7; }\n");
    result = build_gateway.execute(
        fixtures::call("build-fail", "build_project",
                       {{"configuration", "Debug"},
                        {"target", "tiny_test"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);

    result = build_gateway.execute(
        fixtures::call("test-fail", "run_tests",
                       {{"configuration", "Debug"},
                        {"test_name", "tiny.pass[1]"}}),
        context);
    REQUIRE(result.has_value() && result.value().is_error);
    REQUIRE(result.value().content.size() <= 65'536);
    const auto failed = fixtures::content_json(result);
    REQUIRE(failed.at("operation") == "test");
    REQUIRE(failed.at("exit_code").get<std::int64_t>() != 0);
    REQUIRE(failed.at("timed_out") == false);

    agent::WorkspaceToolGateway files(temp.path() / "runtime_data");
    const auto listed = files.execute(
        fixtures::call("list", "list_files", {{"path", "."}}), context);
    REQUIRE(listed.has_value() && !listed.value().is_error);
    const auto listed_json = fixtures::content_json(listed);
    for (const auto& entry : listed_json.at("entries")) {
        REQUIRE(entry.at("path").get<std::string>().rfind(".agent", 0) != 0);
    }

    const auto read_private = files.execute(
        fixtures::call("read-private", "read_file",
                       {{"path", ".agent/cmake-build/CMakeCache.txt"}}),
        context);
    REQUIRE(fixtures::error_code(read_private) == "access_denied");
}
