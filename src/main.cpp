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
#include "application/memory_engine.h"
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

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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

int run_agent(std::vector<std::string> args) {
    try {
        auto startup = agent::parse_startup_arguments(args);
        if (!startup.has_value()) {
            std::cerr << startup.error().message << '\n';
            return agent::ExitCode::InvalidInputOrConfig;
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
        agent::RuntimeEngine engine(model, tools, *knowledge, events, clock,
                                    ids, cancellation);

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
        agent::ModelMemoryConsolidator consolidator(model,
            {config.value().budgets.model_timeout_ms});
        agent::MemoryPolicy memory_policy(config.value().memory_policy);
        agent::MemoryRetriever memory_retriever;
        std::unique_ptr<agent::JsonlMemoryStore> memories;
        std::unique_ptr<agent::MemoryEngine> memory_engine;
        if (config.value().session_context.memory_enabled) {
            memories = std::make_unique<agent::JsonlMemoryStore>(config.value().runtime_root);
            memory_engine = std::make_unique<agent::MemoryEngine>(
                *memories, sessions, consolidator, memory_retriever,
                memory_policy, clock, ids);
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
            cancellation.end_turn();
            commands.begin_turn = [&] { cancellation.begin_turn(); };
            commands.end_turn = [&] { cancellation.end_turn(); };
            commands.cancel_turn = [&] { cancellation.request_cancel(); };
            commands.cancellation_requested = [&] { return cancellation.requested(); };
            commands.set_phase_observer = [&](std::function<void(const std::string&)> observer) {
                diagnostics.observe(std::move(observer));
            };
            commands.list = [&] { return session_engine.list_sessions(); };
            commands.create = [&](const std::string& workspace) {
                return session_engine.create_session(
                    workspace, config.value().anthropic.model);
            };
            commands.load = [&](const std::string& session_id) {
                return session_engine.load_session(session_id);
            };
            commands.submit = [&]
                (const std::string& session_id,
                 const std::string& text,
                 const agent::RuntimeProgressObserver& observer,
                 bool use_memory) {
                return session_engine.submit_turn(
                    session_id, text, observer, use_memory);
            };
            commands.recover = [&]
                (const std::string& session_id,
                 const agent::RuntimeProgressObserver& observer,
                 bool use_memory) {
                return session_engine.recover_pending_turn(
                    session_id, observer, use_memory);
            };
            commands.submit_presented = [&]
                (const std::string& session_id, const std::string& text,
                 const agent::RuntimeProgressObserver& observer, bool use_memory,
                 const agent::RuntimePresentationOptions& presentation) {
                return session_engine.submit_turn(
                    session_id, text, observer, use_memory, presentation);
            };
            commands.recover_presented = [&]
                (const std::string& session_id,
                 const agent::RuntimeProgressObserver& observer, bool use_memory,
                 const agent::RuntimePresentationOptions& presentation) {
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
                    return memory_engine->forget(memory_id);
                };
                commands.consolidate = [&](const std::string& session_id) {
                    const auto result = memory_engine->consolidate(session_id);
                    return result.has_value() ? agent::Result<void>::success() :
                        agent::Result<void>::failure(result.error());
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
