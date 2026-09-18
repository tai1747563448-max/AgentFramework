#include "adapters/mcp/mcp_tool_gateway.h"
#include "adapters/mcp/server_registry.h"
#include "adapters/process/direct_process_runner.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <filesystem>
#include <string>

namespace {

using agent::mcp::McpToolGateway;
using agent::mcp::ServerConfig;
using agent::mcp::ServerRegistry;

std::filesystem::path fixture_script() {
    return std::filesystem::path(AGENT_MCP_FIXTURE_PATH);
}

TEST_CASE(server_registry_bootstrap_then_inventory_is_qualified) {
    agent::DirectProcessRunner runner;
    ServerRegistry registry;
    registry.set_runner(&runner);
    ServerConfig config;
    config.name = "fixture";
    config.program = "python";
    config.arguments = {"-u", fixture_script().generic_u8string()};
    config.working_directory = fixture_script().parent_path();
    registry.add_server(config);

    auto bootstrap = registry.bootstrap_all();
    REQUIRE(bootstrap.has_value());
    REQUIRE(bootstrap.value().size() == 2);
    REQUIRE(bootstrap.value()[0].name == "fixture__echo");
    REQUIRE(bootstrap.value()[1].name == "fixture__boom");
    REQUIRE(registry.inventory().size() == 2);
}

TEST_CASE(mcp_tool_gateway_dispatches_qualified_call_to_server) {
    agent::DirectProcessRunner runner;
    ServerRegistry registry;
    registry.set_runner(&runner);
    ServerConfig config;
    config.name = "fixture";
    config.program = "python";
    config.arguments = {"-u", fixture_script().generic_u8string()};
    config.working_directory = fixture_script().parent_path();
    registry.add_server(config);
    REQUIRE(registry.bootstrap_all().has_value());

    McpToolGateway gateway(registry);
    const auto defs = gateway.definitions();
    REQUIRE(defs.size() == 2);

    agent::ToolCall call;
    call.id = "call-1";
    call.name = "fixture__echo";
    call.arguments = agent::Value::object(
        {{"path", agent::Value("src/main.cpp")}});
    const auto result = gateway.execute(
        call, agent::ToolExecutionContext{"/workspace"});
    REQUIRE(result.has_value());
    REQUIRE(!result.value().is_error);
    REQUIRE(result.value().content.find("src/main.cpp") !=
            std::string::npos);
}

TEST_CASE(mcp_tool_gateway_returns_is_error_for_server_error) {
    agent::DirectProcessRunner runner;
    ServerRegistry registry;
    registry.set_runner(&runner);
    ServerConfig config;
    config.name = "fixture";
    config.program = "python";
    config.arguments = {"-u", fixture_script().generic_u8string()};
    config.working_directory = fixture_script().parent_path();
    registry.add_server(config);
    REQUIRE(registry.bootstrap_all().has_value());

    McpToolGateway gateway(registry);
    agent::ToolCall call;
    call.id = "call-1";
    call.name = "fixture__boom";
    call.arguments = agent::Value::object({});
    const auto result = gateway.execute(
        call, agent::ToolExecutionContext{"/workspace"});
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
}

TEST_CASE(mcp_tool_gateway_synthesises_fault_for_unknown_tool) {
    agent::DirectProcessRunner runner;
    ServerRegistry registry;
    registry.set_runner(&runner);
    ServerConfig config;
    config.name = "fixture";
    config.program = "python";
    config.arguments = {"-u", fixture_script().generic_u8string()};
    config.working_directory = fixture_script().parent_path();
    registry.add_server(config);
    REQUIRE(registry.bootstrap_all().has_value());

    McpToolGateway gateway(registry);
    agent::ToolCall call;
    call.id = "call-1";
    call.name = "fixture__missing";
    call.arguments = agent::Value::object({});
    const auto result = gateway.execute(
        call, agent::ToolExecutionContext{"/workspace"});
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
    REQUIRE(result.value().content.find("unknown mcp tool") !=
            std::string::npos);
}

}  // namespace
