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
           std::unique_ptr<EventStore> events,
           std::unique_ptr<Clock> clock,
           std::unique_ptr<IdGenerator> ids,
           std::unique_ptr<Cancellation> cancellation,
           std::unique_ptr<MemoryEngine> memory);

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
    // events argument carries the persisted task history.
    RuntimeResult resume(const std::string& session_id,
                         const std::vector<RuntimeEvent>& events,
                         const std::string& fallback_system_prompt,
                         RuntimeProgressObserver observer = {});

    // cancel triggers the shared Cancellation token. Subsequent
    // ask / resume calls observe cancellation between external calls.
    void cancel();

    const EngineConfig& config() const noexcept { return config_; }

private:
    EngineConfig config_;
    std::unique_ptr<RuntimeEngine> runtime_;
    std::shared_ptr<HookChain> hook_chain_;
    std::shared_ptr<Permission> permission_;
    std::unique_ptr<ModelClient> model_;
    std::unique_ptr<ToolGateway> tools_;
    std::unique_ptr<KnowledgeProvider> knowledge_;
    std::unique_ptr<EventStore> events_;
    std::unique_ptr<Clock> clock_;
    std::unique_ptr<IdGenerator> ids_;
    std::unique_ptr<Cancellation> cancellation_;
    std::unique_ptr<MemoryEngine> memory_;
};

// build_engine assembles the engine from the supplied config. The
// caller supplies the file-system / model / network adapters; this
// composition function wires them up with the right defaults so the
// Engine facade has the same observable behaviour as the legacy
// run_agent() path.
std::unique_ptr<Engine> build_engine(const EngineConfig& config);

}  // namespace agent