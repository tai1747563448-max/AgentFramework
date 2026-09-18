#include "composition/engine.h"

#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/process/reproc_jsonl_process.h"
#include "adapters/rag/native_rag_pack_verifier.h"
#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "adapters/anthropic/cpr_http_transport.h"
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
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/memory_store.h"
#include "ports/model_client.h"
#include "ports/permission.h"
#include "ports/session_store.h"

#include <memory>
#include <string>
#include <utility>

namespace agent {

Engine::Engine(EngineConfig config,
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
               std::unique_ptr<MemoryEngine> memory)
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
      memory_(std::move(memory)) {}

Engine::~Engine() = default;

SessionTurnResult Engine::ask(const std::string& session_id,
                              const std::string& prompt,
                              RuntimeProgressObserver observer,
                              bool use_memory) {
    (void)session_id;
    (void)prompt;
    (void)use_memory;
    // The Engine facade is the SDK entry point. Wiring it through
    // the full SessionEngine::submit_turn path is follow-up work;
    // for now the facade returns an empty SessionTurnResult so the
    // composition root has a stable surface. Tests can assert the
    // facade exists and refuses to silently fall back to inline
    // assembly.
    return {};
}

RuntimeResult Engine::resume(const std::string& session_id,
                             const std::vector<RuntimeEvent>& events,
                             const std::string& fallback_system_prompt,
                             RuntimeProgressObserver observer) {
    (void)session_id;
    (void)events;
    (void)fallback_system_prompt;
    (void)observer;
    return {};
}

void Engine::cancel() {
    // The base Cancellation interface only exposes requested(); the
    // concrete SignalCancellation has request_cancel(). Downcast
    // and forward when the runtime is the real SignalCancellation;
    // other implementations (e.g. test fakes) just observe the
    // existing flag.
    if (auto* signal =
            dynamic_cast<class SignalCancellation*>(cancellation_.get())) {
        signal->request_cancel();
    }
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
    auto tools = std::make_unique<CompositeToolGateway>(
        std::move(gateways));
    auto knowledge_owned = std::make_unique<EmptyKnowledgeProvider>();
    auto events_owned = std::make_unique<JsonlEventStore>(
        config.runtime_root);
    auto clock_owned = std::make_unique<SystemClock>();
    auto ids_owned = std::make_unique<RandomIdGenerator>();
    auto cancellation_owned = std::make_unique<SignalCancellation>();
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
        std::move(memory)));
}

}  // namespace agent