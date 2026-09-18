#include "adapters/mcp/stdio_client.h"

#include "domain/latency_trace.h"
#include "ports/process_runner.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace agent::mcp {

StdioClient::StdioClient(std::string server_name,
                         std::string server_program,
                         std::vector<std::string> server_args,
                         std::filesystem::path server_working_directory,
                         ProcessRunner* runner)
    : server_name_(std::move(server_name)),
      server_program_(std::move(server_program)),
      server_args_(std::move(server_args)),
      server_working_directory_(std::move(server_working_directory)),
      runner_(runner) {}

StdioClient::~StdioClient() = default;

Result<nlohmann::json> StdioClient::send_request(
    const std::string& method,
    const nlohmann::json& params) {
    static std::atomic<std::int64_t> id_source{1};
    JsonRpcRequest request{id_source.fetch_add(1), method, params};
    const auto encoded = encode_request(request);
    ProcessRequest process;
    process.program = server_program_;
    process.arguments = server_args_;
    process.working_directory = server_working_directory_;
    process.timeout_ms = 30'000;
    process.max_stdout_bytes = 4 * 1024 * 1024;
    process.max_stderr_bytes = 1 * 1024 * 1024;
    process.stdin_utf8 = encoded;
    const auto response = runner_->run(process);
    if (!response.has_value()) {
        return Result<nlohmann::json>::failure(
            {ErrorCode::DependencyUnavailable, response.error().message,
             true});
    }
    if (response.value().timed_out) {
        return Result<nlohmann::json>::failure(
            {ErrorCode::DependencyUnavailable,
             "mcp server request timed out", true});
    }
    const auto decoded = decode_envelope(response.value().stdout_utf8);
    if (decoded.is_object() && decoded.contains("error")) {
        const auto& error = decoded.at("error");
        return Result<nlohmann::json>::failure(
            {ErrorCode::ProtocolFailure,
             error.value("message", "mcp error"),
             false});
    }
    if (decoded.is_object() && decoded.contains("result")) {
        return Result<nlohmann::json>::success(decoded.at("result"));
    }
    return Result<nlohmann::json>::failure(
        {ErrorCode::ProtocolFailure,
         "mcp server reply is missing both result and error", false});
}

Result<nlohmann::json> StdioClient::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return Result<nlohmann::json>::success({});
    }
    const auto reply = send_request(
        "initialize",
        {{"protocolVersion", "2024-11-05"},
         {"clientInfo",
          {{"name", "agentframework"}, {"version", "0.1.0"}}}});
    if (!reply.has_value()) {
        return reply;
    }
    initialized_ = true;
    return reply;
}

Result<std::vector<McpTool>> StdioClient::list_tools() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Result<std::vector<McpTool>>::failure(
            {ErrorCode::InvalidTransition,
             "mcp client must be initialized before listing tools", false});
    }
    const auto reply = send_request("tools/list", nlohmann::json::object());
    if (!reply.has_value()) {
        return Result<std::vector<McpTool>>::failure(reply.error());
    }
    const auto& tools = reply.value().at("tools");
    std::vector<McpTool> out;
    out.reserve(tools.size());
    for (const auto& entry : tools) {
        McpTool tool;
        tool.name = entry.at("name").get<std::string>();
        if (entry.contains("description") &&
            entry.at("description").is_string()) {
            tool.description = entry.at("description").get<std::string>();
        }
        tool.input_schema =
            entry.value("inputSchema", nlohmann::json::object());
        out.push_back(std::move(tool));
    }
    return Result<std::vector<McpTool>>::success(std::move(out));
}

Result<McpToolResult> StdioClient::call_tool(
    const std::string& name,
    const nlohmann::json& arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return Result<McpToolResult>::failure(
            {ErrorCode::InvalidTransition,
             "mcp client must be initialized before calling tools", false});
    }
    const auto reply = send_request(
        "tools/call", {{"name", name}, {"arguments", arguments}});
    if (!reply.has_value()) {
        return Result<McpToolResult>::failure(reply.error());
    }
    McpToolResult result;
    result.is_error = reply.value().value("isError", false);
    if (reply.value().contains("content") &&
        reply.value().at("content").is_array()) {
        result.content.reserve(reply.value().at("content").size());
        for (const auto& entry : reply.value().at("content")) {
            result.content.push_back(entry);
        }
    }
    return Result<McpToolResult>::success(std::move(result));
}

}  // namespace agent::mcp
