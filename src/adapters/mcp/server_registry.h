#pragma once

#include "adapters/mcp/stdio_client.h"
#include "domain/result.h"
#include "ports/process_runner.h"
#include "ports/tool_gateway.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent::mcp {

// T14 (v2 §3): ServerConfig captures the spawn shape for a named
// MCP server. The registry keeps one logical MCP server entry per
// name; runtime tool dispatch is handled by the composite gateway
// (see register_runtime) rather than a ToolGateway subclass so the
// existing permission / hook chains continue to apply unchanged.
struct ServerConfig {
    std::string name;
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
};

// ServerRegistry owns one StdioClient per registered server. It
// holds a non-owning pointer to the ProcessRunner that every
// StdioClient uses; the runner outlives the registry because the
// composition root owns it.
class ServerRegistry final {
public:
    ServerRegistry();

    // Inject the ProcessRunner shared by every client. Must be
    // called before add_server; the registry never takes ownership.
    void set_runner(ProcessRunner* runner);

    void add_server(ServerConfig config);
    std::vector<ServerConfig> servers() const;

    // Resolve the client for a given server name. Returns nullptr
    // when the server has not been registered; callers should
    // surface "unknown mcp server" through the ToolResult envelope.
    StdioClient* get_client(const std::string& server_name);

    // Eagerly initialise every registered server and pull its
    // tool inventory. The composite gateway calls this once at
    // composition time so model-facing ToolDefinitions are stable
    // for the lifetime of the process.
    Result<std::vector<McpTool>> bootstrap_all();

    // Tool inventory produced by the most recent bootstrap_all.
    // The map is keyed by qualified tool name ("server__tool").
    // Returns an empty vector when bootstrap_all has not run.
    const std::vector<McpTool>& inventory() const noexcept {
        return inventory_;
    }

private:
    Result<void> bootstrap_one(const std::string& server_name,
                                StdioClient& client);

    ProcessRunner* runner_{nullptr};
    std::map<std::string, std::unique_ptr<StdioClient>> clients_;
    std::vector<McpTool> inventory_;
};

}  // namespace agent::mcp
