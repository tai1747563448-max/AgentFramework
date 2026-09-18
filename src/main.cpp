#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/process/reproc_jsonl_process.h"
#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "adapters/rag/native_rag_pack_verifier.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "adapters/tools/composite_tool_gateway.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "application/runtime_engine.h"
#include "adapters/permission/static_permission.h"
#include "ports/permission.h"
#include "application/memory_engine.h"
#include "application/memory_maintenance_scheduler.h"
#include "application/memory_policy.h"
#include "application/memory_retriever.h"
#include "application/model_context_compactor.h"
#include "application/model_memory_consolidator.h"
#include "application/session_engine.h"
#include "application/state_reducer.h"
#include "cli/cli_app.h"
#include "cli/interactive_cli.h"
#include "cli/terminal_text.h"
#include "cli/terminal_capabilities.h"
#include "config/runtime_config.h"
#include "config/setting_source.h"
#include "domain/latency_trace.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class DiagnosticRouter {
public:
    void observe(std::function<void(const std::string&)> observer) {
        std::lock_guard<std::mutex> lock(mutex_);
        observer_ = std::move(observer);
    }
    void emit(const std::string& message) {
        std::function<void(const std::string&)> observer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            observer = observer_;
        }
        if (observer) {
            observer(message);
        } else {
            std::cerr << agent::render_terminal_text(message) << '\n';
            std::cerr.flush();
        }
    }
private:
    std::mutex mutex_;
    std::function<void(const std::string&)> observer_;
};

// LatencyTraceSink writes LatencySample records to a JSONL file. It is
// installed only when AGENT_LATENCY_TRACE_FILE points to a writable path;
// otherwise the global observer stays null and emission becomes a no-op.
//
// T07: batching. Per-sample fflush() was the second fsync hotspot after
// JsonlEventStore. The sink now buffers up to 32 samples or 500ms, then
// writes the whole batch with a single flush. The destructor still drains
// any pending samples so the trace file is closed in a durable state.
class LatencyTraceSink {
public:
    explicit LatencyTraceSink(const std::filesystem::path& path)
        : stream_(path, std::ios::out | std::ios::trunc) {
        if (!stream_.is_open()) {
            throw std::runtime_error("latency trace file could not be opened");
        }
        last_flush_ = std::chrono::steady_clock::now();
    }
    ~LatencyTraceSink() {
        try { flush_locked(); } catch (...) {}
    }
    void operator()(const agent::LatencySample& sample) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_ += "{\"request_id\":\"" + escape(sample.request_id)
                + "\",\"stage\":\"" + escape(sample.stage)
                + "\",\"monotonic_us\":"
                + std::to_string(sample.monotonic_us) + "}\n";
        ++pending_;
        const auto now = std::chrono::steady_clock::now();
        if (pending_ >= kBatchThreshold ||
            now - last_flush_ >= kBatchInterval) {
            flush_locked();
        }
    }
private:
    // 32 samples / 500ms — same cadence as JsonlEventStore so the trace
    // file does not get ahead of the WAL during a bursty turn.
    static constexpr std::size_t kBatchThreshold = 32;
    static constexpr std::chrono::milliseconds kBatchInterval{500};

    void flush_locked() {
        if (pending_ == 0) return;
        stream_ << buffer_;
        stream_.flush();
        buffer_.clear();
        pending_ = 0;
        last_flush_ = std::chrono::steady_clock::now();
    }
    static std::string escape(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (const auto byte : raw) {
            switch (byte) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(byte) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned char>(byte));
                    out += buffer;
                } else {
                    out.push_back(byte);
                }
            }
        }
        return out;
    }
    std::mutex mutex_;
    std::ofstream stream_;
    std::string buffer_;
    std::size_t pending_{0};
    std::chrono::steady_clock::time_point last_flush_;
};

int run_agent(std::vector<std::string> args) {
    try {
        // T0 latency trace wiring. The trace file is opt-in via env so the
        // production Ready EXE is unaffected unless the benchmark harness
        // explicitly requests samples.
        std::optional<LatencyTraceSink> trace_sink;
        {
            agent::ProcessEnvironment probe;
            const auto trace_path = probe.get("AGENT_LATENCY_TRACE_FILE");
            if (trace_path.has_value() && !trace_path->empty()) {
                try {
                    trace_sink.emplace(std::filesystem::u8path(*trace_path));
                    agent::set_global_latency_observer(
                        [&trace_sink](const agent::LatencySample& sample) {
                            if (trace_sink.has_value()) (*trace_sink)(sample);
                        });
                } catch (const std::exception&) {
                    std::cerr << "latency trace file could not be opened; "
                                 "disabling trace\n";
                    agent::set_global_latency_observer(nullptr);
                }
            }
        }
        agent::emit_latency_sample("process", agent::kStageProcessStart);

        auto startup = agent::parse_startup_arguments(args);
        if (!startup.has_value()) {
            std::cerr << startup.error().message << '\n';
            return agent::ExitCode::InvalidInputOrConfig;
        }
        if (startup.value().show_effective_config) {
            // T05: print the merged layered view without booting
            // the runtime. Process env is read via ProcessEnvironment
            // (the same source load_runtime_config consumes).
            const auto layered = agent::build_default_layered_settings();
            if (!layered.has_value()) {
                std::cerr << layered.error().message << '\n';
                return agent::ExitCode::InvalidInputOrConfig;
            }
            std::cout << agent::render_layered_settings_json(
                              layered.value())
                      << '\n';
            return agent::ExitCode::Success;
        }
        if (startup.value().command_args.size() > 1 &&
            (startup.value().command_args[1] == "verify-log" ||
             startup.value().command_args[1] == "evaluate-log")) {
            agent::JsonlEventStore local_events(std::filesystem::path{});
            agent::RunCommand unavailable_run = [](
                const agent::RunRequest&,
                const agent::RuntimeProgressObserver&) {
                return agent::RuntimeResult{
                    std::nullopt,
                    agent::RuntimeError{agent::ErrorCode::InvalidInput,
                                        "run command is unavailable", false}};
            };
            agent::VerifyCommand verify =
                [&](const std::filesystem::path& path) {
                    auto loaded = local_events.read_file(path);
                    if (!loaded.has_value()) {
                        return agent::Result<agent::TaskState>::failure(
                            {agent::ErrorCode::PersistenceFailure,
                             "event log validation failed", false});
                    }
                    return agent::replay_events(loaded.value());
                };
            agent::ResumeCommand unavailable_resume = [](
                const std::string&,
                const agent::RuntimeProgressObserver&) {
                return agent::RuntimeResult{
                    std::nullopt,
                    agent::RuntimeError{agent::ErrorCode::InvalidInput,
                                        "resume command is unavailable",
                                        false}};
            };
            agent::EvaluateCommand evaluate =
                [&](const std::filesystem::path& path) {
                    auto loaded = local_events.read_file(path);
                    if (!loaded.has_value()) {
                        return agent::Result<agent::TaskEvaluation>::failure(
                            {agent::ErrorCode::PersistenceFailure,
                             "event log evaluation failed", false});
                    }
                    return agent::evaluate_task_events(loaded.value());
                };
            agent::CliApp app(
                std::move(unavailable_run), std::move(unavailable_resume),
                std::move(verify), std::move(evaluate), std::cout, std::cerr);
            return app.execute(startup.value().command_args);
        }
        if (startup.value().env_file.has_value()) {
            const auto loaded =
                agent::load_explicit_env_file(*startup.value().env_file);
            if (!loaded.has_value()) {
                std::cerr << loaded.error().message << '\n';
                return agent::ExitCode::InvalidInputOrConfig;
            }
        } else if (startup.value().command_args.size() == 1) {
            std::error_code error;
            const auto cwd = std::filesystem::current_path(error);
            if (error) {
                std::cerr << "interactive working directory is unavailable\n";
                return agent::ExitCode::InvalidInputOrConfig;
            }
            const auto discovered = agent::discover_interactive_env_file(
                std::filesystem::u8path(
                    startup.value().command_args.front()),
                cwd);
            if (!discovered.has_value()) {
                std::cerr << discovered.error().message << '\n';
                return agent::ExitCode::InvalidInputOrConfig;
            }
            if (discovered.value().has_value()) {
                const auto loaded = agent::load_explicit_env_file(
                    *discovered.value());
                if (!loaded.has_value()) {
                    std::cerr << loaded.error().message << '\n';
                    return agent::ExitCode::InvalidInputOrConfig;
                }
            }
        }

        agent::ProcessEnvironment environment;
        auto config = agent::load_runtime_config(
            environment,
            std::filesystem::u8path(startup.value().command_args.front()));
        if (!config.has_value()) {
            std::cerr << config.error().message << '\n';
            return agent::ExitCode::InvalidInputOrConfig;
        }

        agent::CprHttpTransport transport;
        agent::AnthropicMessagesClient model(config.value().anthropic, transport);
        agent::DirectProcessRunner process;
        agent::WorkspaceToolGateway file_tools(config.value().runtime_root);
        agent::CMakeToolGateway build_tools(
            process, config.value().build_timeout_ms);
        std::vector<std::reference_wrapper<agent::ToolGateway>> gateways{
            std::ref(file_tools)};
        if (config.value().build_tools_enabled) {
            gateways.push_back(std::ref(build_tools));
        }
        agent::CompositeToolGateway tools(std::move(gateways));
        DiagnosticRouter diagnostics;
        std::unique_ptr<agent::JsonlProcess> rag_process;
        std::unique_ptr<agent::RagPackVerifier> rag_pack_verifier;
        // Destroy the provider before the process and verifier it references.
        std::unique_ptr<agent::KnowledgeProvider> knowledge;
        if (config.value().rag_enabled) {
            const auto rag_progress = [&](const std::string& message) {
                diagnostics.emit(message);
            };
            rag_process =
                std::make_unique<agent::ReprocJsonlProcess>(rag_progress);
            rag_pack_verifier =
                std::make_unique<agent::NativeRagPackVerifier>(rag_progress);
            knowledge =
                std::make_unique<agent::PersistentRagKnowledgeProvider>(
                    *rag_process, *rag_pack_verifier, config.value().rag);
        } else {
            knowledge = std::make_unique<agent::EmptyKnowledgeProvider>();
        }
        agent::JsonlEventStore events(config.value().runtime_root);
        agent::SystemClock clock;
        agent::RandomIdGenerator ids;
        agent::SignalCancellation cancellation;
        // T11: assemble a StaticPermission from the layered settings.
        // Default posture is "default" (read tools allowed, write tools
        // prompt). Operators can switch to bypassPermissions via
        // .agentrc.json or AGENT_PERMISSION_MODE. The shared_ptr is
        // moved into the engine so the REPL's /permissions command can
        // print the same state the runtime is enforcing.
        std::shared_ptr<agent::StaticPermission> permission;
        {
            const auto mode_setting =
                std::getenv("AGENT_PERMISSION_MODE");
            const auto parsed = mode_setting == nullptr
                                   ? agent::PermissionMode::Default
                                   : agent::parse_permission_mode(mode_setting)
                                         .value_or(
                                             agent::PermissionMode::Default);
            permission = std::make_shared<agent::StaticPermission>(parsed);
        }
        agent::RuntimeEngine engine(model, tools, *knowledge, events, clock,
                                    ids, cancellation,
                                    config.value().anthropic.model,
                                    {},
                                    nullptr,
                                    permission);

        agent::RunCommand run = [&]
            (const agent::RunRequest& cli_request,
             const agent::RuntimeProgressObserver& observer) {
            auto request = cli_request;
            request.system_prompt = config.value().system_prompt;
            request.budgets = config.value().budgets;
            return engine.run(request, observer);
        };
        agent::ResumeCommand resume = [&]
            (const std::string& task_id,
             const agent::RuntimeProgressObserver& observer) {
            auto loaded = events.read_task(task_id);
            if (!loaded.has_value() || loaded.value().empty() ||
                loaded.value().front().task_id != task_id) {
                return agent::RuntimeResult{
                    std::nullopt,
                    agent::RuntimeError{
                        agent::ErrorCode::PersistenceFailure,
                        "task event log could not be loaded", false}};
            }
            return engine.resume(
                {std::move(loaded.value()), config.value().system_prompt},
                observer);
        };
        agent::VerifyCommand verify = [&](const std::filesystem::path& path) {
            auto loaded = events.read_file(path);
            if (!loaded.has_value()) {
                return agent::Result<agent::TaskState>::failure(
                    {agent::ErrorCode::PersistenceFailure,
                     "event log validation failed", false});
            }
            return agent::replay_events(loaded.value());
        };

        agent::JsonlSessionStore sessions(config.value().runtime_root);
        agent::ModelContextCompactor compactor(model,
            {config.value().session_context.max_summary_bytes,
             config.value().budgets.model_timeout_ms});
        // Two independent ModelClient instances are required: the foreground
        // chat client and the extraction client used by the maintenance
        // scheduler. Sharing one would couple cancellable maintenance work to
        // the foreground request path, which is exactly what T8 forbids.
        agent::CprHttpTransport maintenance_transport;
        agent::AnthropicMessagesClient maintenance_model(config.value().anthropic,
                                                         maintenance_transport);
        agent::ModelMemoryConsolidator consolidator(model,
            {config.value().budgets.model_timeout_ms});
        agent::ModelMemoryConsolidator maintenance_consolidator(maintenance_model,
            {config.value().budgets.model_timeout_ms});
        agent::MemoryPolicy memory_policy(config.value().memory_policy);
        agent::MemoryRetriever memory_retriever;
        std::unique_ptr<agent::JsonlMemoryStore> memories;
        std::unique_ptr<agent::MemoryEngine> memory_engine;
        std::unique_ptr<agent::MemoryMaintenanceScheduler> maintenance_scheduler;
        if (config.value().session_context.memory_enabled) {
            memories = std::make_unique<agent::JsonlMemoryStore>(config.value().runtime_root);
            memory_engine = std::make_unique<agent::MemoryEngine>(
                *memories, sessions, consolidator, memory_retriever,
                memory_policy, clock, ids);
            // The scheduler owns its own consolidator with its own ModelClient.
            // The foreground request path never touches this consolidator.
            auto consolidator_for_scheduler =
                std::make_unique<agent::ModelMemoryConsolidator>(
                    maintenance_model,
                    agent::ModelMemoryConsolidatorConfig{
                        config.value().budgets.model_timeout_ms});
            maintenance_scheduler =
                std::make_unique<agent::MemoryMaintenanceScheduler>(
                    *memory_engine, std::move(consolidator_for_scheduler),
                    agent::MemoryMaintenanceScheduler::Config{2,
                        std::chrono::milliseconds(
                            config.value().budgets.model_timeout_ms)});
        }
        agent::SessionLoadTask load_task = [&](const std::string& task_id) {
            const auto path = events.event_path(task_id);
            if (!path.has_value()) {
                return agent::Result<std::optional<
                    std::vector<agent::RuntimeEvent>>>::failure(path.error());
            }
            std::error_code error;
            const bool exists = std::filesystem::exists(path.value(), error);
            if (error) {
                return agent::Result<std::optional<
                    std::vector<agent::RuntimeEvent>>>::failure(
                        {agent::ErrorCode::PersistenceFailure,
                         "task event log could not be inspected", false});
            }
            if (!exists) {
                return agent::Result<std::optional<
                    std::vector<agent::RuntimeEvent>>>::success(std::nullopt);
            }
            auto loaded = events.read_task(task_id);
            if (!loaded.has_value()) {
                return agent::Result<std::optional<
                    std::vector<agent::RuntimeEvent>>>::failure(
                        loaded.error());
            }
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(
                    std::move(loaded.value()));
        };
        agent::SessionResumeTask resume_session = [&]
            (const agent::ResumeRequest& request,
             const agent::RuntimeProgressObserver& observer) {
            return engine.resume(request, observer);
        };
        agent::SessionRunTask run_session = [&]
            (const agent::RunRequest& request,
             const agent::RuntimeProgressObserver& observer) {
            // Summary and memory are already assembled.
            return engine.run(request, observer);
        };
        agent::SessionEngine session_engine(
            sessions, clock, ids, std::move(run_session), std::move(resume_session),
            std::move(load_task),
            {config.value().system_prompt, config.value().budgets},
            &compactor, memory_engine.get(), config.value().session_context);

        if (startup.value().command_args.size() == 1) {
            agent::TerminalCapabilities terminal(
                std::cin, std::cout, startup.value().plain_ui);
            if (!terminal.utf8_ready()) {
                std::cerr << "interactive UTF-8 console setup failed\n";
                return agent::ExitCode::InvalidInputOrConfig;
            }
            agent::InteractiveSessionCommands commands;
            // Track the currently active session id so the foreground forget
            // command can invalidate any in-flight scheduler work that was
            // derived from the same session. The pointer is captured by the
            // forget closure below.
            std::string current_session_holder;
            std::string* current_session_id = &current_session_holder;
            cancellation.end_turn();
            commands.begin_turn = [&] { cancellation.begin_turn(); };
            commands.end_turn = [&] { cancellation.end_turn(); };
            commands.cancel_turn = [&] { cancellation.request_cancel(); };
            commands.cancellation_requested = [&] { return cancellation.requested(); };
            commands.set_phase_observer = [&](std::function<void(const std::string&)> observer) {
                diagnostics.observe(std::move(observer));
            };
            // T11: surface the runtime's permission state to /permissions.
            // Capturing the shared_ptr by value keeps the callback alive
            // even after the engine goes out of scope.
            commands.permission_state_text = [permission]() {
                return agent::serialise_permission_state(*permission);
            };
            // T17 (v2 §3): /tasks snapshots the active (non-terminal)
            // task list by walking every session and reading its durable
            // last_task_status. Background tasks that the model spawned
            // in earlier turns still surface here even when the
            // foreground session has moved on to a new issue.
            commands.active_tasks_text = [&session_engine]() {
                auto listed = session_engine.list_sessions();
                if (!listed.has_value()) {
                    return std::string{"[]"};
                }
                std::ostringstream stream;
                stream << "[";
                bool first = true;
                for (const auto& session : listed.value()) {
                    if (!session.last_task_id.has_value() ||
                        !session.last_task_status.has_value()) {
                        continue;
                    }
                    const auto status = session.last_task_status.value();
                    const bool active =
                        status == agent::TaskStatus::Created ||
                        status == agent::TaskStatus::PreparingContext ||
                        status == agent::TaskStatus::AwaitingModel ||
                        status == agent::TaskStatus::AwaitingTool;
                    if (!active) continue;
                    if (!first) stream << ",";
                    first = false;
                    stream << "{\"session_id\":\""
                           << session.session_id
                           << "\",\"task_id\":\"" << session.last_task_id.value()
                           << "\",\"status\":\""
                           << agent::task_status_name(status) << "\"}";
                }
                stream << "]";
                return stream.str();
            };
            commands.list = [&] { return session_engine.list_sessions(); };
            commands.create = [&](const std::string& workspace) {
                auto created = session_engine.create_session(
                    workspace, config.value().anthropic.model);
                if (created.has_value()) {
                    current_session_holder = created.value().session_id;
                }
                return created;
            };
            commands.load = [&](const std::string& session_id) {
                auto loaded = session_engine.load_session(session_id);
                if (loaded.has_value()) {
                    current_session_holder = loaded.value().session_id;
                }
                return loaded;
            };
            commands.submit = [&, current_session_id]
                (const std::string& session_id,
                 const std::string& text,
                 const agent::RuntimeProgressObserver& observer,
                 bool use_memory) {
                *current_session_id = session_id;
                return session_engine.submit_turn(
                    session_id, text, observer, use_memory);
            };
            commands.recover = [&, current_session_id]
                (const std::string& session_id,
                 const agent::RuntimeProgressObserver& observer,
                 bool use_memory) {
                *current_session_id = session_id;
                return session_engine.recover_pending_turn(
                    session_id, observer, use_memory);
            };
            commands.submit_presented = [&, current_session_id]
                (const std::string& session_id, const std::string& text,
                 const agent::RuntimeProgressObserver& observer, bool use_memory,
                 const agent::RuntimePresentationOptions& presentation) {
                *current_session_id = session_id;
                return session_engine.submit_turn(
                    session_id, text, observer, use_memory, presentation);
            };
            // T10: dry-run variant. Forwards to session_engine with
            // dry_run=true so the runtime strips tool definitions and the
            // model produces a plan-only text response.
            commands.submit_presented_dry = [&, current_session_id]
                (const std::string& session_id, const std::string& text,
                 const agent::RuntimeProgressObserver& observer, bool use_memory,
                 const agent::RuntimePresentationOptions& presentation,
                 bool dry_run) {
                *current_session_id = session_id;
                return session_engine.submit_turn(
                    session_id, text, observer, use_memory, presentation,
                    dry_run);
            };
            commands.recover_presented = [&, current_session_id]
                (const std::string& session_id,
                 const agent::RuntimeProgressObserver& observer, bool use_memory,
                 const agent::RuntimePresentationOptions& presentation) {
                *current_session_id = session_id;
                return session_engine.recover_pending_turn(
                    session_id, observer, use_memory, presentation);
            };
            if (memory_engine) {
                commands.memories = [&](const std::string& workspace) {
                    return memory_engine->list(workspace);
                };
                commands.remember = [&](const std::string& session_id, const std::string& text) {
                    return memory_engine->remember(session_id, text);
                };
                commands.forget = [&](const std::string& memory_id) {
                    const auto result = memory_engine->forget(memory_id);
                    // The forget call already mutated the persisted log; tell
                    // the scheduler that any pending maintenance for the
                    // current session must be invalidated before commit.
                    if (result.has_value() && maintenance_scheduler &&
                        current_session_id != nullptr) {
                        maintenance_scheduler->forget(*current_session_id,
                                                      memory_id);
                    }
                    return result;
                };
                commands.request_maintenance = [&](const std::string& session_id,
                                                  std::uint64_t through) {
                    if (maintenance_scheduler)
                        maintenance_scheduler->request_maintenance(session_id, through);
                };
                commands.drain_for_exit = [&](std::chrono::milliseconds timeout) {
                    if (maintenance_scheduler)
                        maintenance_scheduler->drain_for_exit(timeout);
                };
                commands.pending_through = [&](const std::string& session_id)
                    -> std::optional<std::uint64_t> {
                    if (!maintenance_scheduler) return std::nullopt;
                    return maintenance_scheduler->pending_through(session_id);
                };
            }
            std::error_code error;
            const auto cwd = std::filesystem::current_path(error);
            if (error) {
                std::cerr << "interactive working directory is unavailable\n";
                return agent::ExitCode::InvalidInputOrConfig;
            }
            agent::InteractiveCli interactive(
                std::move(commands), config.value().anthropic.model,
                cwd.generic_u8string(), std::cin, std::cout, std::cerr,
                config.value().session_context.memory_enabled,
                {terminal.dynamic(), startup.value().stream_enabled,
                 [&terminal] { return terminal.columns(); }});
            agent::emit_latency_sample("process", agent::kStageMenuReady);
            return interactive.run();
        }

        agent::CliApp app(std::move(run), std::move(resume),
                          std::move(verify), std::cout, std::cerr);
        return app.execute(startup.value().command_args);
    } catch (const std::exception&) {
        std::cerr << "runtime initialization failed\n";
        return agent::ExitCode::InvalidInputOrConfig;
    } catch (...) {
        std::cerr << "runtime initialization failed\n";
        return agent::ExitCode::InvalidInputOrConfig;
    }
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    const auto converted =
        agent::utf8_arguments_from_windows(argc, argv);
    if (!converted.has_value()) {
        std::cerr << converted.error().message << '\n';
        return agent::ExitCode::InvalidInputOrConfig;
    }
    return run_agent(std::move(converted.value()));
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return run_agent(std::move(args));
}
#endif
