#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/empty/empty_tool_gateway.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "cli/cli_app.h"
#include "config/runtime_config.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            args.emplace_back(argv[index]);
        }

        auto startup = agent::parse_startup_arguments(args);
        if (!startup.has_value()) {
            std::cerr << startup.error().message << '\n';
            return agent::ExitCode::InvalidInputOrConfig;
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
        agent::EmptyToolGateway tools;
        agent::EmptyKnowledgeProvider knowledge;
        agent::JsonlEventStore events(config.value().runtime_root);
        agent::SystemClock clock;
        agent::RandomIdGenerator ids;
        agent::SignalCancellation cancellation;
        agent::RuntimeEngine engine(model, tools, knowledge, events, clock, ids,
                                    cancellation);

        agent::RunCommand run = [&](const agent::RunRequest& cli_request) {
            auto request = cli_request;
            request.system_prompt = config.value().system_prompt;
            request.budgets = config.value().budgets;
            return engine.run(request);
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

        agent::CliApp app(std::move(run), std::move(verify), std::cout,
                          std::cerr);
        return app.execute(startup.value().command_args);
    } catch (const std::exception&) {
        std::cerr << "runtime initialization failed\n";
        return agent::ExitCode::InvalidInputOrConfig;
    } catch (...) {
        std::cerr << "runtime initialization failed\n";
        return agent::ExitCode::InvalidInputOrConfig;
    }
}
