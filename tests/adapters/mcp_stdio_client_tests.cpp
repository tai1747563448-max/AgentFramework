#include "adapters/mcp/stdio_client.h"
#include "adapters/process/direct_process_runner.h"
#include "domain/result.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace {

using agent::mcp::McpTool;
using agent::mcp::StdioClient;
using agent::Result;

std::filesystem::path fixture_script() {
    return std::filesystem::path(AGENT_MCP_FIXTURE_PATH);
}

TEST_CASE(stdio_client_initialise_then_list_tools) {
    agent::DirectProcessRunner runner;
    StdioClient client(
        "fixture", "python", {"-u", fixture_script().generic_u8string()},
        fixture_script().parent_path(), &runner);
    REQUIRE(client.initialize().has_value());

    Result<std::vector<McpTool>> listed = client.list_tools();
    REQUIRE(listed.has_value());
    REQUIRE(listed.value().size() == 2);
    REQUIRE(listed.value()[0].name == "echo");
    REQUIRE(listed.value()[1].name == "boom");
}

TEST_CASE(stdio_client_call_tool_echoes_arguments) {
    agent::DirectProcessRunner runner;
    StdioClient client(
        "fixture", "python", {"-u", fixture_script().generic_u8string()},
        fixture_script().parent_path(), &runner);
    REQUIRE(client.initialize().has_value());

    nlohmann::json args{{"hello", "world"}};
    auto result = client.call_tool("echo", args);
    REQUIRE(result.has_value());
    REQUIRE(!result.value().is_error);
    REQUIRE(result.value().content.size() == 1);
    REQUIRE(result.value().content[0].at("type") == "text");
    REQUIRE(result.value().content[0].at("text") ==
            std::string{"{\"hello\": \"world\"}"});
}

TEST_CASE(stdio_client_call_tool_propagates_server_error) {
    agent::DirectProcessRunner runner;
    StdioClient client(
        "fixture", "python", {"-u", fixture_script().generic_u8string()},
        fixture_script().parent_path(), &runner);
    REQUIRE(client.initialize().has_value());

    auto result = client.call_tool("boom", nlohmann::json::object());
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
    REQUIRE(result.value().content[0].at("text") ==
            std::string{"synthetic boom"});
}

TEST_CASE(stdio_client_rejects_list_tools_before_initialise) {
    agent::DirectProcessRunner runner;
    StdioClient client(
        "fixture", "python", {"-u", fixture_script().generic_u8string()},
        fixture_script().parent_path(), &runner);
    auto listed = client.list_tools();
    REQUIRE(!listed.has_value());
    REQUIRE(listed.error().code == agent::ErrorCode::InvalidTransition);
}

}  // namespace
