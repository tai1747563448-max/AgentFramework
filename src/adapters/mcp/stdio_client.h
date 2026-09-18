#pragma once

#include "adapters/mcp/json_rpc.h"
#include "domain/result.h"
#include "ports/process_runner.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent::mcp {

struct McpTool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

struct McpToolResult {
    bool is_error{false};
    std::vector<nlohmann::json> content;
};

// T14 (v2 §3): StdioClient owns the lifecycle of a single MCP server
// invocation. Calls are synchronous on top of ProcessRunner and
// thread-safe via an internal mutex so the composite gateway can
// share one client across multiple tool invocations.
//
// Per-request spawn model: each request runs the server subprocess
// to completion, sending one JSON-RPC envelope on stdin and reading
// the reply on stdout. This matches the stateless MCP servers
// (filesystem, fetch, ...) the v2 roadmap targets and keeps the
// transport simple. A future commit can lift this to a persistent
// long-running pipe once we need it.
class StdioClient final {
public:
    StdioClient(std::string server_name,
                std::string server_program,
                std::vector<std::string> server_args,
                std::filesystem::path server_working_directory,
                ProcessRunner* runner);
    ~StdioClient();

    StdioClient(const StdioClient&) = delete;
    StdioClient& operator=(const StdioClient&) = delete;

    // Drive the MCP handshake. Returns the server-reported
    // capabilities on success. Idempotent: subsequent calls are
    // no-ops.
    Result<nlohmann::json> initialize();

    // List the tools the server exposes. Returns the parsed tool
    // array - the registry maps each entry into a McpTool.
    Result<std::vector<McpTool>> list_tools();

    // Call a tool by name with the supplied arguments. The result
    // carries the MCP content array (text / image / resource blocks)
    // and an is_error flag the gateway forwards to the runtime.
    Result<McpToolResult> call_tool(
        const std::string& name,
        const nlohmann::json& arguments);

    const std::string& server_name() const noexcept { return server_name_; }

private:
    Result<nlohmann::json> send_request(
        const std::string& method,
        const nlohmann::json& params);

    std::string server_name_;
    std::string server_program_;
    std::vector<std::string> server_args_;
    std::filesystem::path server_working_directory_;
    ProcessRunner* runner_;
    mutable std::mutex mutex_;
    bool initialized_{false};
};

}  // namespace agent::mcp
