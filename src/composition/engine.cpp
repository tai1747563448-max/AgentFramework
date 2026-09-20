#include "composition/engine.h"

#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/mcp/mcp_tool_gateway.h"
#include "adapters/mcp/server_registry.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/process/reproc_jsonl_process.h"
#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "adapters/rag/native_rag_pack_verifier.h"
#include "adapters/sandbox/sandbox_factory.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "adapters/tools/composite_tool_gateway.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "adapters/permission/static_permission.h"
#include "application/memory_engine.h"
#include "application/memory_maintenance_scheduler.h"
#include "application/memory_policy.h"
#include "application/memory_retriever.h"
#include "application/model_context_compactor.h"
#include "application/model_memory_consolidator.h"
#include "application/runtime_engine.h"
#include "application/session_engine.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/context_compactor.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/memory_store.h"
#include "ports/model_client.h"
#include "ports/permission.h"
#include "ports/sandbox.h"
#include "ports/session_store.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace agent {
namespace {

// MCP server-list parser for EngineConfig::mcp_servers.
//   "name=path args;name2=path2 args2"
// Each entry splits on the first '=' for name/program, then splits
// the remainder on whitespace for argv tokens. Returns an empty
// vector on empty or malformed input; build_engine treats the empty
// result as "no MCP configured" and skips the bootstrap path.
std::vector<mcp::ServerConfig> parse_mcp_servers(const std::string& spec) {
    std::vector<mcp::ServerConfig> configs;
    if (spec.empty()) return configs;
    std::size_t cursor = 0;
    while (cursor < spec.size()) {
        const auto semi = spec.find(';', cursor);
        const std::string entry =
            spec.substr(cursor, semi == std::string::npos ? std::string::npos
                                                          : semi - cursor);
        cursor = (semi == std::string::npos) ? spec.size() : semi + 1;
        if (entry.empty()) continue;
        const auto eq = entry.find('=');
        if (eq == std::string::npos || eq == 0 || eq == entry.size() - 1) {
            std::cerr << "mcp: skipping malformed entry '" << entry << "'\n";
            continue;
        }
        mcp::ServerConfig cfg;
        cfg.name = entry.substr(0, eq);
        // Tokenise the remainder on whitespace; first token is the
        // program path, rest are arguments.
        std::istringstream stream(entry.substr(eq + 1));
        std::vector<std::string> tokens;
        std::string token;
        while (stream >> token) tokens.push_back(token);
        if (tokens.empty()) {
            std::cerr << "mcp: skipping empty program for '" << cfg.name
                      << "'\n";
            continue;
        }
        cfg.program = tokens.front();
        cfg.arguments.assign(tokens.begin() + 1, tokens.end());
        configs.push_back(std::move(cfg));
    }
    return configs;
}

}  // namespace

Engine::Engine(EngineConfig config,
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
               std::unique_ptr<Sandbox> sandbox)
    : config_(std::move(config)),
      runtime_(std::move(runtime)),
      hook_chain_(std::move(hook_chain)),
      permission_(std::move(permission)),
      model_(std::move(model)),
      tools_(std::move(tools)),
      knowledge_(std::move(knowledge)),
      events_(std::move(events)),
      clock_(std::move(clock)),
      ids_(std::move(ids)),
      cancellation_(std::move(cancellation)),
      memory_(std::move(memory)),
      sessions_(std::move(sessions)),
      compactor_(std::move(compactor)),
      sandbox_(std::move(sandbox)) {}

Engine::~Engine() = default;

SessionEngine& Engine::ensure_session_engine() {
    if (session_engine_) return *session_engine_;
    // Build the three closures that SessionEngine needs:
    //   load_task   - read a task event log by id
    //   run_session - delegate to RuntimeEngine::run with budgets/system
    //                 prompt injected from the EngineConfig
    //   resume_session - delegate to RuntimeEngine::resume using the
    //                    supplied fallback system prompt.
    //
    // The closures borrow from Engine members by reference; the
    // SessionEngine only lives as long as the Engine that owns it
    // (session_engine_ is the last member declared), so the borrowed
    // references are guaranteed valid for its lifetime.
    auto events_ptr = events_.get();
    auto clock_ptr = clock_.get();
    auto ids_ptr = ids_.get();
    auto memory_ptr = memory_.get();
    auto sessions_ptr = sessions_.get();
    auto compactor_ptr = compactor_.get();
    auto runtime_ptr = runtime_.get();
    const auto default_system_prompt = config_.system_prompt;
    const auto default_budgets = config_.budgets;
    SessionLoadTask load_task =
        [events_ptr](const std::string& task_id)
        -> Result<std::optional<std::vector<RuntimeEvent>>> {
        const auto path = events_ptr->event_path(task_id);
        if (!path.has_value()) {
            return Result<std::optional<std::vector<RuntimeEvent>>>::failure(
                path.error());
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(path.value(), error);
        if (error) {
            return Result<std::optional<std::vector<RuntimeEvent>>>::failure(
                {ErrorCode::PersistenceFailure,
                 "task event log could not be inspected", false});
        }
        if (!exists) {
            return Result<std::optional<std::vector<RuntimeEvent>>>::success(
                std::nullopt);
        }
        auto loaded = events_ptr->read_task(task_id);
        if (!loaded.has_value()) {
            return Result<std::optional<std::vector<RuntimeEvent>>>::failure(
                loaded.error());
        }
        return Result<std::optional<std::vector<RuntimeEvent>>>::success(
            std::move(loaded.value()));
    };
    SessionRunTask run_session =
        [runtime_ptr, default_system_prompt, default_budgets](
            const RunRequest& request,
            const RuntimeProgressObserver& observer) {
            RunRequest stamped = request;
            if (stamped.system_prompt.empty()) {
                stamped.system_prompt = default_system_prompt;
            }
            stamped.budgets = default_budgets;
            return runtime_ptr->run(stamped, observer);
        };
    SessionResumeTask resume_session =
        [runtime_ptr](const ResumeRequest& request,
                      const RuntimeProgressObserver& observer) {
            return runtime_ptr->resume(request, observer);
        };
    SessionRuntimeDefaults defaults{config_.system_prompt, config_.budgets};
    session_engine_ = std::make_unique<SessionEngine>(
        *sessions_ptr, *clock_ptr, *ids_ptr, std::move(run_session),
        std::move(resume_session), std::move(load_task), defaults,
        compactor_ptr, memory_ptr, config_.session_context);
    return *session_engine_;
}

SessionTurnResult Engine::ask(const std::string& session_id,
                              const std::string& prompt,
                              RuntimeProgressObserver observer,
                              bool use_memory) {
    auto& session_engine = ensure_session_engine();
    std::string resolved_session_id = session_id;
    if (resolved_session_id.empty()) {
        const auto workspace_utf8 =
            config_.runtime_root.empty()
                ? std::string{}
                : config_.runtime_root.lexically_normal().string();
        auto created = session_engine.create_session(workspace_utf8,
                                                    config_.model_name);
        if (!created.has_value()) {
            return {std::nullopt, std::nullopt, created.error()};
        }
        resolved_session_id = created.value().session_id;
    }
    return session_engine.submit_turn(resolved_session_id, prompt,
                                      std::move(observer), use_memory);
}

RuntimeResult Engine::resume(const std::string& session_id,
                             const std::vector<RuntimeEvent>& events,
                             const std::string& fallback_system_prompt,
                             RuntimeProgressObserver observer) {
    auto& session_engine = ensure_session_engine();
    if (session_id.empty()) {
        return {std::nullopt,
                RuntimeError{ErrorCode::InvalidInput,
                             "engine::resume requires a non-empty session_id",
                             false}};
    }
    // resume replays the supplied event log; the SessionEngine doesn't
    // surface a direct resume path, so we route through the
    // ResumeRequest that the runtime closure already understands.
    (void)session_engine;
    ResumeRequest request;
    request.durable_events = events;
    request.fallback_system_prompt = fallback_system_prompt;
    request.presentation = {};
    return runtime_->resume(request, std::move(observer));
}

void Engine::cancel() {
    // Pure port call: no downcast, no knowledge of the concrete
    // adapter. The Cancellation port guarantees cancel() is noexcept
    // and observable to is_cancelled() on every implementation.
    cancellation_->cancel();
}

std::unique_ptr<Engine> build_engine(const EngineConfig& config) {
    // T20: the composition root that replaces main.cpp's run_agent().
    // All ports are wired here so the Engine facade is the only thing
    // the SDK consumer touches. Subordinate layers (T11 Permission,
    // T13 Hooks, T19 Cost-tracker) attach to the resulting Engine
    // through their public constructors.
    auto transport_owned = std::make_unique<CprHttpTransport>();
    AnthropicConfig anthropic_config;
    anthropic_config.model = config.model_name;
    anthropic_config.api_version = "2023-06-01";
    auto model_owned = std::make_unique<AnthropicMessagesClient>(
        anthropic_config, *transport_owned);
    DirectProcessRunner process;
    auto tools_owned = std::make_unique<WorkspaceToolGateway>(
        config.runtime_root);
    std::vector<std::reference_wrapper<ToolGateway>> gateways{
        std::ref(*tools_owned)};

    // MCP bootstrap. Fail-open: when mcp_servers is empty we skip the
    // path entirely; when bootstrap itself fails we log and continue
    // with the workspace-only gateway so the CLI keeps starting. This
    // mirrors Claude Code's behaviour where a misconfigured MCP server
    // is reported but does not abort the host process.
    auto mcp_configs = parse_mcp_servers(config.mcp_servers);
    auto registry_owned = std::make_unique<mcp::ServerRegistry>();
    if (!mcp_configs.empty()) {
        registry_owned->set_runner(&process);
        for (const auto& cfg : mcp_configs) {
            registry_owned->add_server(cfg);
        }
        const auto bootstrap = registry_owned->bootstrap_all();
        if (!bootstrap.has_value()) {
            std::cerr << "mcp: bootstrap failed (" << bootstrap.error().message
                      << "); continuing without MCP tools\n";
        } else {
            gateways.push_back(
                std::ref(static_cast<ToolGateway&>(
                    *std::make_unique<mcp::McpToolGateway>(*registry_owned))));
        }
    }
    auto tools = std::make_unique<CompositeToolGateway>(
        std::move(gateways));
    auto knowledge_owned = std::make_unique<EmptyKnowledgeProvider>();
    auto events_owned = std::make_unique<JsonlEventStore>(
        config.runtime_root);
    auto clock_owned = std::make_unique<SystemClock>();
    auto ids_owned = std::make_unique<RandomIdGenerator>();
    auto cancellation_owned = std::make_unique<SignalCancellation>();
    auto sessions_owned = std::make_unique<JsonlSessionStore>(
        config.runtime_root);
    auto compactor_owned = std::make_unique<ModelContextCompactor>(
        *model_owned,
        ModelContextCompactorConfig{config.session_context.max_summary_bytes,
                                    config.budgets.model_timeout_ms});
    KnowledgeProvider* knowledge_ptr = knowledge_owned.get();
    EventStore* events_ptr = events_owned.get();
    Clock* clock_ptr = clock_owned.get();
    IdGenerator* ids_ptr = ids_owned.get();
    Cancellation* cancellation_ptr = cancellation_owned.get();
    auto hook_chain = std::make_shared<HookChain>();
    auto permission = std::make_shared<StaticPermission>(
        PermissionMode::Default);
    std::unique_ptr<MemoryEngine> memory;
    std::shared_ptr<MemoryStore> memory_store;
    std::shared_ptr<SessionStore> session_store;
    if (config.memory_enabled) {
        memory_store = std::make_shared<JsonlMemoryStore>(
            config.runtime_root);
        session_store = std::make_shared<JsonlSessionStore>(
            config.runtime_root);
        MemoryPolicy policy(config.memory_policy);
        MemoryRetriever retriever;
        // The consolidator needs its own ModelClient; share the
        // pointer since the SDK facade treats the model as opaque.
        auto consolidator = std::make_unique<ModelMemoryConsolidator>(
            *model_owned,
            ModelMemoryConsolidatorConfig{config.budgets.model_timeout_ms});
        memory = std::make_unique<MemoryEngine>(
            *memory_store, *session_store, *consolidator, retriever,
            policy, *clock_ptr, *ids_ptr);
    }
    auto runtime = std::make_unique<RuntimeEngine>(
        *model_owned, *tools, *knowledge_ptr, *events_ptr, *clock_ptr,
        *ids_ptr, *cancellation_ptr, config.model_name, nullptr,
        hook_chain, permission);
    // Sandbox is wired best-effort: make_default_sandbox() picks the
    // platform-appropriate implementation and falls back to NoOp when
    // the host (e.g. Linux without /usr/bin/bwrap) cannot enforce a
    // containment policy. The facade advertises that a sandbox is
    // available; whether tool execution actually uses it is a T22
    // follow-up once WorkspaceToolGateway consumes the Sandbox port.
    auto sandbox_owned = make_default_sandbox();
    return std::unique_ptr<Engine>(new Engine(
        config,
        std::move(runtime),
        std::move(hook_chain),
        std::move(permission),
        std::move(model_owned),
        std::move(tools),
        std::move(knowledge_owned),
        std::move(events_owned),
        std::move(clock_owned),
        std::move(ids_owned),
        std::move(cancellation_owned),
        std::move(memory),
        std::move(sessions_owned),
        std::move(compactor_owned),
        std::move(sandbox_owned)));
}

}  // namespace agent
