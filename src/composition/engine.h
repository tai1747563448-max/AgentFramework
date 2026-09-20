#pragma once

// T20 (v2 §3): SDK / composition root. Encapsulates the runtime
// assembly that used to live inline in main.cpp behind a 400-line
// run_agent() function. The composition layer exposes a thin Engine
// facade with ask / resume / cancel — those are the three verbs
// every consumer (CLI, batch runner, Python SDK) actually needs.
//
// The composition layer keeps the legacy behaviour: the same model
// client, tool gateway, knowledge provider, hook chain, permission,
// and reactive-compact trigger that main.cpp previously assembled by
// hand. Future work can replace individual layers (T12 LLM provider
// abstraction, T14 MCP adapter) by passing alternative factories to
// build_engine().
#include "application/memory_policy.h"
#include "application/session_engine.h"
#include "config/runtime_config.h"
#include "domain/task_state.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class RuntimeEngine;
class EventStore;
class Clock;
class IdGenerator;
class Cancellation;
class ToolGateway;
class KnowledgeProvider;
class MemoryEngine;
class HookChain;
class Permission;
class ModelClient;
class SessionEngine;
class SessionStore;
class ContextCompactor;
class Sandbox;
class JsonlEventStore;

struct EngineConfig {
    // Workspace the engine will operate in. Empty means the caller
    // wants an ad-hoc workspace resolved later via create_session().
    std::filesystem::path runtime_root;
    std::string system_prompt;
    std::string model_name;
    bool memory_enabled{true};
    bool rag_enabled{false};
    RagConfig rag;
    SessionContextSettings session_context;
    MemoryPolicyConfig memory_policy{1024, {}};
    RuntimeBudgets budgets;
    // Fail-open MCP server list parsed by build_engine. Format:
    //   "name1=/path/to/server1 args;name2=/path/to/server2"
    // Empty disables MCP. Bootstrap failures are logged but never
    // fail build_engine; the CLI must keep starting when an MCP
    // server is misconfigured.
    std::string mcp_servers;
};

class Engine {
public:
    Engine(EngineConfig config,
           std::unique_ptr<RuntimeEngine> runtime,
           std::shared_ptr<HookChain> hook_chain,
           std::shared_ptr<Permission> permission,
           std::unique_ptr<ModelClient> model,
           std::unique_ptr<ToolGateway> tools,
           std::unique_ptr<KnowledgeProvider> knowledge,
           std::unique_ptr<JsonlEventStore> events,
           std::unique_ptr<Clock> clock,
           std::unique_ptr<IdGenerator> ids,
           std::unique_ptr<Cancellation> cancellation,
           std::unique_ptr<MemoryEngine> memory,
           std::unique_ptr<SessionStore> sessions,
           std::unique_ptr<ContextCompactor> compactor,
           std::unique_ptr<Sandbox> sandbox);

    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // ask submits a turn against the supplied session. If session_id
    // is empty, a new session is created for the engine's runtime
    // root and the returned result references it.
    SessionTurnResult ask(const std::string& session_id,
                          const std::string& prompt,
                          RuntimeProgressObserver observer = {},
                          bool use_memory = true);

    // resume replays a durable event log into the runtime. The
    // events argument carries the persisted task history; the
    // session_id parameter selects the workspace owner.
    RuntimeResult resume(const std::string& session_id,
                         const std::vector<RuntimeEvent>& events,
                         const std::string& fallback_system_prompt,
                         RuntimeProgressObserver observer = {});

    // cancel flips the shared Cancellation token through the port.
    // Subsequent ask / resume calls observe cancellation between
    // external calls without any downcast.
    void cancel();

    const EngineConfig& config() const noexcept { return config_; }

private:
    // Lazily constructs session_engine_ on first ask/resume. The
    // dependency chain (sessions_, compactor_, memory_) is owned by
    // the Engine so the SessionEngine can borrow raw references
    // without lifetime concerns; the Engine owns session_engine_ to
    // keep the reference graph rooted here.
    SessionEngine& ensure_session_engine();

    EngineConfig config_;
    std::unique_ptr<RuntimeEngine> runtime_;
    std::shared_ptr<HookChain> hook_chain_;
    std::shared_ptr<Permission> permission_;
    std::unique_ptr<ModelClient> model_;
    std::unique_ptr<ToolGateway> tools_;
    std::unique_ptr<KnowledgeProvider> knowledge_;
    // Held as the concrete JsonlEventStore (not the EventStore port)
    // because the SessionEngine load-task closure needs event_path()
    // and read_task(), which are adapter-level methods. The runtime
    // still consumes only the EventStore surface.
    std::unique_ptr<JsonlEventStore> events_;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<IdGenerator> ids_;
    std::unique_ptr<Cancellation> cancellation_;
    std::unique_ptr<MemoryEngine> memory_;
    // Sessions and compactor are owned so the SessionEngine has
    // stable references for the Engine's lifetime; constructed in
    // build_engine before the SessionEngine helper runs.
    std::unique_ptr<SessionStore> sessions_;
    std::unique_ptr<ContextCompactor> compactor_;
    // Sandbox is held best-effort: the Engine facade advertises that
    // a sandbox is available without yet forcing its use on every
    // tool call (the tool gateway does not consume the Sandbox port
    // today). Future T22 work will thread this through WorkspaceToolGateway.
    std::unique_ptr<Sandbox> sandbox_;
    // Lazily-constructed SessionEngine. Declared after the ports it
    // borrows so destruction order keeps the references valid.
    std::unique_ptr<SessionEngine> session_engine_;
};

// build_engine assembles the engine from the supplied config. The
// caller supplies the file-system / model / network adapters; this
// composition function wires them up with the right defaults so the
// Engine facade has the same observable behaviour as the legacy
// run_agent() path.
std::unique_ptr<Engine> build_engine(const EngineConfig& config);

}  // namespace agent