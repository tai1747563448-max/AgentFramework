#include "adapters/mcp/mcp_tool_gateway.h"

#include "adapters/json/value_json.h"
#include "domain/value.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace agent::mcp {

namespace {

Value to_value(const nlohmann::json& json) {
    if (auto parsed = agent::value_from_json(json); parsed.has_value()) {
        return std::move(parsed).value();
    }
    return Value::object({});
}

std::string tool_part(const std::string& qualified) {
    const auto sep = qualified.find("__");
    if (sep == std::string::npos) {
        return qualified;
    }
    return qualified.substr(sep + 2);
}

ToolResult fault_result(const std::string& call_id,
                        const std::string& code,
                        const std::string& message) {
    const nlohmann::json body{{"error", {{"code", code}, {"message", message}}}};
    return {call_id, body.dump(), true};
}

}  // namespace

McpToolGateway::McpToolGateway(ServerRegistry& registry)
    : registry_(registry) {
    for (const auto& tool : registry_.inventory()) {
        const auto sep = tool.name.find("__");
        const std::string server_name =
            sep == std::string::npos ? std::string{} : tool.name.substr(0, sep);
        server_for_tool_[tool.name] = server_name;
        tools_.emplace(tool.name, tool);
    }
}

McpToolGateway::~McpToolGateway() = default;

std::vector<ToolDefinition> McpToolGateway::definitions() const {
    std::vector<ToolDefinition> out;
    out.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        Value schema = to_value(tool.input_schema);
        if (!schema.is_object()) {
            schema = Value::object({{"type", "object"}});
        }
        out.push_back({name, tool.description, schema});
    }
    return out;
}

Result<ToolResult> McpToolGateway::execute(
    const ToolCall& call,
    const ToolExecutionContext& context) {
    (void)context;
    const auto found = tools_.find(call.name);
    if (found == tools_.end()) {
        return Result<ToolResult>::success(fault_result(
            call.id, "unknown_tool", "unknown mcp tool"));
    }
    const auto& server_name = server_for_tool_.at(call.name);
    auto* client = registry_.get_client(server_name);
    if (client == nullptr) {
        return Result<ToolResult>::success(fault_result(
            call.id, "missing_server",
            "mcp server is not registered: " + server_name));
    }
    auto result = client->call_tool(
        tool_part(call.name), agent::value_to_json(call.arguments));
    if (!result.has_value()) {
        return Result<ToolResult>::success(fault_result(
            call.id, "mcp_transport", result.error().message));
    }
    nlohmann::json body;
    body["server"] = server_name;
    body["tool"] = tool_part(call.name);
    body["is_error"] = result.value().is_error;
    body["content"] = result.value().content;
    return Result<ToolResult>::success(
        {call.id, body.dump(), result.value().is_error});
}

bool McpToolGateway::tool_is_concurrency_safe(
    const std::string& name) const {
    (void)name;
    return true;
}

bool McpToolGateway::tool_is_read_only(
    const std::string& name) const {
    (void)name;
    return false;
}

PermissionDecision McpToolGateway::tool_permission_decision(
    const ToolCall& call,
    const ToolExecutionContext& context) const {
    (void)call;
    (void)context;
    return PermissionDecision::Ask;
}

}  // namespace agent::mcp
