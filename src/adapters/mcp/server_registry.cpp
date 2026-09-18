#include "adapters/mcp/server_registry.h"

#include "domain/latency_trace.h"
#include "ports/process_runner.h"

#include <utility>

namespace agent::mcp {

ServerRegistry::ServerRegistry() = default;

void ServerRegistry::set_runner(ProcessRunner* runner) {
    runner_ = runner;
}

void ServerRegistry::add_server(ServerConfig config) {
    auto name = config.name;
    auto client = std::make_unique<StdioClient>(
        name, std::move(config.program), std::move(config.arguments),
        std::move(config.working_directory), runner_);
    clients_.emplace(std::move(name), std::move(client));
}

std::vector<ServerConfig> ServerRegistry::servers() const {
    std::vector<ServerConfig> out;
    out.reserve(clients_.size());
    for (const auto& [name, client] : clients_) {
        ServerConfig config;
        config.name = name;
        out.push_back(std::move(config));
    }
    return out;
}

StdioClient* ServerRegistry::get_client(const std::string& server_name) {
    const auto found = clients_.find(server_name);
    if (found == clients_.end()) {
        return nullptr;
    }
    return found->second.get();
}

Result<void> ServerRegistry::bootstrap_one(const std::string& server_name,
                                            StdioClient& client) {
    const auto init = client.initialize();
    if (!init.has_value()) {
        return Result<void>::failure(init.error());
    }
    const auto listed = client.list_tools();
    if (!listed.has_value()) {
        return Result<void>::failure(listed.error());
    }
    for (auto& tool : listed.value()) {
        // The gateway dispatches by "server_name__tool_name" so
        // two servers can expose tools of the same local name
        // without colliding on the ToolDefinition map.
        std::string qualified = server_name + "__" + tool.name;
        McpTool renamed;
        renamed.name = std::move(qualified);
        renamed.description = std::move(tool.description);
        renamed.input_schema = std::move(tool.input_schema);
        inventory_.push_back(std::move(renamed));
    }
    return Result<void>::success();
}

Result<std::vector<McpTool>> ServerRegistry::bootstrap_all() {
    inventory_.clear();
    for (auto& [name, client] : clients_) {
        const auto step = bootstrap_one(name, *client);
        if (!step.has_value()) {
            return Result<std::vector<McpTool>>::failure(step.error());
        }
    }
    return Result<std::vector<McpTool>>::success(inventory_);
}

}  // namespace agent::mcp
