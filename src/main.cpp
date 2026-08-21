#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/rag/python_rag_knowledge_provider.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "adapters/tools/composite_tool_gateway.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "cli/cli_app.h"
#include "config/runtime_config.h"

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

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
        }

        agent::ProcessEnvironment environment;
        auto config = agent::load_runtime_config(environment);
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
        std::unique_ptr<agent::KnowledgeProvider> knowledge;
        if (config.value().rag_enabled) {
            knowledge = std::make_unique<agent::PythonRagKnowledgeProvider>(
                process, config.value().rag);
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
            const auto path = events.event_path(task_id);
            if (!path.has_value()) {
                return agent::RuntimeResult{
                    std::nullopt,
                    agent::RuntimeError{
                        agent::ErrorCode::PersistenceFailure,
                        "task event log could not be loaded", false}};
            }
            auto loaded = events.read_file(path.value());
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
