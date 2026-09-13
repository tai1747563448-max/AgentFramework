#include "cli/interactive_cli.h"
#include "config/runtime_config.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "application/memory_engine.h"
#include "application/memory_retriever.h"
#include "ports/clock.h"
#include "ports/context_compactor.h"
#include "ports/id_generator.h"
#include "test_support.h"

#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace fixtures {

constexpr const char* kFirstSession =
    "session-11111111111111111111111111111111";
constexpr const char* kSecondSession =
    "session-22222222222222222222222222222222";

agent::SessionState state(std::string session_id,
                          std::string workspace,
                          std::uint64_t turns = 0) {
    agent::SessionState value;
    value.session_id = std::move(session_id);
    value.workspace_utf8 = std::move(workspace);
    value.model = "MiniMax-M3";
    value.completed_turns = turns;
    return value;
}

agent::SessionTurnResult completed(agent::SessionState session,
                                   const std::string& question,
                                   const std::string& answer) {
    session.messages.push_back(
        {agent::Role::User, {agent::TextBlock{question}}});
    session.messages.push_back(
        {agent::Role::Assistant, {agent::TextBlock{answer}}});
    ++session.completed_turns;
    agent::TaskState task;
    task.task_id = "task-11111111111111111111111111111111";
    task.status = agent::TaskStatus::Completed;
    task.final_text = answer;
    return {std::move(session), std::move(task), std::nullopt};
}

}  // namespace fixtures

TEST_CASE(interactive_cli_streams_on_the_main_thread_and_survives_worker_failure) {
    const auto main_thread = std::this_thread::get_id();
    struct WriterGuard : std::stringbuf {
        std::thread::id owner;
        bool wrong_thread{false};
        explicit WriterGuard(std::thread::id id) : owner(id) {}
        std::streamsize xsputn(const char* text, std::streamsize size) override {
            wrong_thread = wrong_thread || owner != std::this_thread::get_id();
            return std::stringbuf::xsputn(text, size);
        }
        int overflow(int character) override {
            wrong_thread = wrong_thread || owner != std::this_thread::get_id();
            return std::stringbuf::overflow(character);
        }
    } buffer(main_thread);
    std::ostream output(&buffer);
    std::ostringstream error;
    std::istringstream input("first\nsecond\n/exit\n");
    auto session = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    int calls = 0;
    agent::InteractiveSessionCommands commands;
    commands.list = [session] {
        return agent::Result<std::vector<agent::SessionState>>::success({session});
    };
    commands.submit_presented = [&](const std::string&, const std::string& question,
        const agent::RuntimeProgressObserver& observer, bool,
        const agent::RuntimePresentationOptions& presentation) {
        REQUIRE(std::this_thread::get_id() != main_thread);
        REQUIRE(presentation.stream);
        observer({"task", 1, agent::EventKind::ModelCallStarted,
                  agent::TaskStatus::AwaitingModel});
        presentation.text_observer({"task", 1,
            {agent::ModelStreamEventKind::TextDelta, 0, calls++ == 0 ? "partial" : "answer"}});
        if (calls == 1) throw std::runtime_error("private failure payload");
        return fixtures::completed(session, question, "answer");
    };
    agent::InteractiveCli cli(std::move(commands), "model", "E:/workspace",
                              input, output, error);
    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(calls == 2);
    REQUIRE(!buffer.wrong_thread);
    REQUIRE(buffer.str().find("[Incomplete; turn was not committed]") != std::string::npos);
    REQUIRE(buffer.str().find("answer") == buffer.str().rfind("answer"));
    REQUIRE(error.str().find("private failure payload") == std::string::npos);
}

TEST_CASE(interactive_cli_stream_off_preserves_complete_answer_and_statuses) {
    auto session = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    agent::InteractiveSessionCommands commands;
    commands.list = [session] {
        return agent::Result<std::vector<agent::SessionState>>::success({session});
    };
    commands.submit_presented = [session](const std::string&, const std::string& text,
        const agent::RuntimeProgressObserver&, bool,
        const agent::RuntimePresentationOptions& presentation) {
        REQUIRE(!presentation.stream);
        return fixtures::completed(session, text, "buffered answer");
    };
    std::istringstream input("question\n/exit\n");
    std::ostringstream output, error;
    agent::InteractiveUiOptions ui;
    ui.stream = false;
    agent::InteractiveCli cli(std::move(commands), "model", "E:/workspace",
                              input, output, error, true, ui);
    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(output.str().find("buffered answer\n") != std::string::npos);
    REQUIRE(output.str().find("[Generating") == std::string::npos);
}

TEST_CASE(interactive_cli_presents_first_bytes_before_worker_completes) {
    std::atomic<bool> displayed{false};
    struct ObservedBuffer : std::stringbuf {
        std::atomic<bool>& displayed;
        explicit ObservedBuffer(std::atomic<bool>& value) : displayed(value) {}
        std::streamsize xsputn(const char* data, std::streamsize size) override {
            const auto count = std::stringbuf::xsputn(data, size);
            if (str().find("first-byte") != std::string::npos) displayed.store(true);
            return count;
        }
    } buffer(displayed);
    std::ostream output(&buffer);
    std::ostringstream error;
    std::istringstream input("question\n/exit\n");
    const auto session = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    bool observed_before_completion = false;
    agent::InteractiveSessionCommands commands;
    commands.list = [session] {
        return agent::Result<std::vector<agent::SessionState>>::success({session});
    };
    commands.submit_presented = [&](const std::string&, const std::string& text,
        const agent::RuntimeProgressObserver&, bool,
        const agent::RuntimePresentationOptions& presentation) {
        presentation.text_observer({"task", 1,
            {agent::ModelStreamEventKind::TextDelta, 0, "first-byte"}});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!displayed.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        observed_before_completion = displayed.load();
        return fixtures::completed(session, text, "first-byte then done");
    };
    agent::InteractiveCli cli(std::move(commands), "model", "E:/workspace",
                              input, output, error);
    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(observed_before_completion);
    REQUIRE(buffer.str().find("first-byte then done") != std::string::npos);
    REQUIRE(buffer.str().find("first-byte") == buffer.str().rfind("first-byte"));
}

TEST_CASE(interactive_cli_closes_full_queue_on_output_failure_and_joins_worker) {
    struct FailedBuffer : std::stringbuf {
        std::streamsize xsputn(const char* data, std::streamsize size) override {
            if (std::string(data, static_cast<std::size_t>(size)).find("[Generating") != std::string::npos)
                return 0;
            return std::stringbuf::xsputn(data, size);
        }
    } buffer;
    std::ostream output(&buffer);
    std::ostringstream error;
    std::istringstream input("question\n/exit\n");
    const auto session = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    std::atomic<bool> cancelled{false}, exited{false};
    int lifecycle_ends = 0;
    agent::InteractiveSessionCommands commands;
    commands.list = [session] {
        return agent::Result<std::vector<agent::SessionState>>::success({session});
    };
    commands.cancel_turn = [&] { cancelled = true; };
    commands.end_turn = [&] { ++lifecycle_ends; };
    commands.submit_presented = [&](const std::string&, const std::string&,
        const agent::RuntimeProgressObserver&, bool,
        const agent::RuntimePresentationOptions& presentation) {
        for (int index = 0; index < 2048; ++index) {
            presentation.text_observer({"task", 1,
                {agent::ModelStreamEventKind::TextDelta, 0, std::string(4096, 'a')}});
        }
        exited = true;
        agent::SessionTurnResult result;
        result.error = agent::RuntimeError{agent::ErrorCode::PersistenceFailure,
            "private persistence failure", false};
        return result;
    };
    agent::InteractiveCli cli(std::move(commands), "model", "E:/workspace",
                              input, output, error);
    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(cancelled.load());
    REQUIRE(exited.load());
    REQUIRE(lifecycle_ends == 1);
    REQUIRE(error.str().find("session turn failed") != std::string::npos);
    REQUIRE(error.str().find("private persistence failure") == std::string::npos);
}

TEST_CASE(interactive_cli_creates_session_and_keeps_two_turns) {
    std::optional<agent::SessionState> current;
    std::vector<std::string> submitted;
    agent::InteractiveSessionCommands commands;
    commands.list = [] {
        return agent::Result<std::vector<agent::SessionState>>::success({});
    };
    commands.create = [&](const std::string& workspace) {
        current = fixtures::state(fixtures::kFirstSession, workspace);
        return agent::Result<agent::SessionState>::success(*current);
    };
    commands.load = [&](const std::string&) {
        return agent::Result<agent::SessionState>::success(*current);
    };
    commands.submit = [&](const std::string&,
                          const std::string& text,
                          const agent::RuntimeProgressObserver& observer, bool) {
        if (observer) {
            observer({"task-11111111111111111111111111111111", 5,
                      agent::EventKind::ModelCallStarted,
                      agent::TaskStatus::AwaitingModel});
        }
        submitted.push_back(text);
        auto result = fixtures::completed(
            *current, text,
            submitted.size() == 1 ? "answer one" : "answer two");
        current = result.session;
        return result;
    };
    commands.recover = [](const std::string&,
                          const agent::RuntimeProgressObserver&, bool) {
        return agent::SessionTurnResult{};
    };
    std::istringstream input(
        "\nfirst question\nsecond question\n/status\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    agent::InteractiveCli cli(
        std::move(commands), "MiniMax-M3", "E:/default",
        input, output, error);

    const int code = cli.run();

    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(submitted ==
            std::vector<std::string>({"first question", "second question"}));
    REQUIRE(current.has_value());
    REQUIRE(current->workspace_utf8 == "E:/default");
    REQUIRE(current->completed_turns == 2);
    REQUIRE(output.str().find("AgentFramework\n") != std::string::npos);
    REQUIRE(output.str().find("Model: MiniMax-M3\n") != std::string::npos);
    REQUIRE(output.str().find("answer one\n") != std::string::npos);
    REQUIRE(output.str().find("answer two\n") != std::string::npos);
    REQUIRE(output.str().find("Thinking...\n") != std::string::npos);
    REQUIRE(output.str().find("Turns: 2\n") != std::string::npos);
    REQUIRE(output.str().find("Messages: 4\n") != std::string::npos);
    REQUIRE(error.str().empty());
}

TEST_CASE(interactive_cli_supports_new_clear_resume_and_unknown_command) {
    auto current = fixtures::state(fixtures::kFirstSession, "E:/first", 3);
    std::vector<std::string> created_workspaces;
    agent::InteractiveSessionCommands commands;
    commands.list = [current] {
        return agent::Result<std::vector<agent::SessionState>>::success(
            {current});
    };
    commands.create = [&](const std::string& workspace) {
        created_workspaces.push_back(workspace);
        current = fixtures::state(
            created_workspaces.size() == 1
                ? fixtures::kSecondSession
                : fixtures::kFirstSession,
            workspace);
        return agent::Result<agent::SessionState>::success(current);
    };
    commands.load = [&](const std::string& session_id) {
        current = fixtures::state(session_id, "E:/resumed", 7);
        return agent::Result<agent::SessionState>::success(current);
    };
    commands.submit = [](const std::string&, const std::string&,
                         const agent::RuntimeProgressObserver&, bool) {
        return agent::SessionTurnResult{};
    };
    commands.recover = [](const std::string&,
                          const agent::RuntimeProgressObserver&, bool) {
        return agent::SessionTurnResult{};
    };
    std::istringstream input(
        "/new E:/second workspace\n/clear\n/resume " +
        std::string(fixtures::kFirstSession) +
        "\n/unknown\n/status\n/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    agent::InteractiveCli cli(
        std::move(commands), "MiniMax-M3", "E:/default",
        input, output, error);

    const int code = cli.run();

    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(created_workspaces ==
            std::vector<std::string>({"E:/second workspace",
                                      "E:/second workspace"}));
    REQUIRE(output.str().find("Workspace: E:/resumed\n") !=
            std::string::npos);
    REQUIRE(output.str().find("Turns: 7\n") != std::string::npos);
    REQUIRE(error.str().find("unknown command") != std::string::npos);
}

TEST_CASE(interactive_cli_recovers_latest_pending_turn_before_prompt) {
    auto pending = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    pending.pending_turn = agent::PendingSessionTurn{
        1, "task-11111111111111111111111111111111", "question"};
    int recover_calls = 0;
    agent::InteractiveSessionCommands commands;
    commands.list = [pending] {
        return agent::Result<std::vector<agent::SessionState>>::success(
            {pending});
    };
    commands.create = [](const std::string&) {
        return agent::Result<agent::SessionState>::failure(
            {agent::ErrorCode::InvalidInput, "unexpected create", false});
    };
    commands.load = [](const std::string&) {
        return agent::Result<agent::SessionState>::failure(
            {agent::ErrorCode::InvalidInput, "unexpected load", false});
    };
    commands.submit = [](const std::string&, const std::string&,
                         const agent::RuntimeProgressObserver&, bool) {
        return agent::SessionTurnResult{};
    };
    commands.recover = [&](const std::string&,
                           const agent::RuntimeProgressObserver&, bool) {
        ++recover_calls;
        pending.pending_turn.reset();
        return fixtures::completed(pending, "question", "recovered answer");
    };
    std::istringstream input("/exit\n");
    std::ostringstream output;
    std::ostringstream error;
    agent::InteractiveCli cli(
        std::move(commands), "MiniMax-M3", "E:/default",
        input, output, error);

    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(recover_calls == 1);
    REQUIRE(output.str().find("recovered answer\n") != std::string::npos);
    REQUIRE(error.str().empty());
}

TEST_CASE(interactive_cli_ignores_blank_chat_and_exits_cleanly_on_eof) {
    auto current = fixtures::state(fixtures::kFirstSession, "E:/workspace");
    int submit_calls = 0;
    agent::InteractiveSessionCommands commands;
    commands.list = [current] {
        return agent::Result<std::vector<agent::SessionState>>::success(
            {current});
    };
    commands.create = [](const std::string&) {
        return agent::Result<agent::SessionState>::failure(
            {agent::ErrorCode::InvalidInput, "unused", false});
    };
    commands.load = [](const std::string&) {
        return agent::Result<agent::SessionState>::failure(
            {agent::ErrorCode::InvalidInput, "unused", false});
    };
    commands.submit = [&](const std::string&, const std::string&,
                          const agent::RuntimeProgressObserver&, bool) {
        ++submit_calls;
        return agent::SessionTurnResult{};
    };
    commands.recover = [](const std::string&,
                          const agent::RuntimeProgressObserver&, bool) {
        return agent::SessionTurnResult{};
    };
    std::istringstream input("   \n\t\n");
    std::ostringstream output;
    std::ostringstream error;
    agent::InteractiveCli cli(
        std::move(commands), "MiniMax-M3", "E:/default",
        input, output, error);

    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(submit_calls == 0);
    REQUIRE(error.str().empty());
}

TEST_CASE(interactive_env_discovery_prefers_executable_directory_then_cwd) {
    test::ScopedTempDir temp("interactive-env-discovery");
    const auto executable_directory = temp.path() / "bin";
    const auto cwd = temp.path() / "workspace";
    std::filesystem::create_directories(executable_directory);
    std::filesystem::create_directories(cwd);
    const auto executable = executable_directory / "AgentFramework.exe";
    const auto exe_env = temp.write_text("bin/.env", "AGENT_MODEL=exe\n");
    const auto cwd_env = temp.write_text(
        "workspace/.env", "AGENT_MODEL=cwd\n");

    auto discovered =
        agent::discover_interactive_env_file(executable, cwd);
    REQUIRE(discovered.has_value());
    REQUIRE(discovered.value() ==
            std::optional<std::filesystem::path>{exe_env});

    std::filesystem::remove(exe_env);
    discovered = agent::discover_interactive_env_file(executable, cwd);
    REQUIRE(discovered.has_value());
    REQUIRE(discovered.value() ==
            std::optional<std::filesystem::path>{cwd_env});
}

TEST_CASE(interactive_env_discovery_returns_empty_when_no_env_exists) {
    test::ScopedTempDir temp("interactive-env-missing");
    const auto result = agent::discover_interactive_env_file(
        temp.path() / "bin/AgentFramework.exe",
        temp.path() / "workspace");

    REQUIRE(result.has_value());
    REQUIRE(!result.value().has_value());
}

TEST_CASE(interactive_cli_status_reports_compacted_projection_and_memory_state) {
    auto current = fixtures::state(fixtures::kFirstSession, "E:/workspace", 8);
    current.compacted_through_turn = 6;
    current.summary = "retained summary";
    current.messages.resize(4);
    agent::InteractiveSessionCommands commands;
    commands.list = [current] {
        return agent::Result<std::vector<agent::SessionState>>::success({current});
    };
    std::istringstream input("/status\n/exit\n");
    std::ostringstream output, error;
    agent::InteractiveCli cli(std::move(commands), "model", "E:/default",
                              input, output, error);
    REQUIRE(cli.run() == agent::ExitCode::Success);
    REQUIRE(output.str().find("Compacted through turn: 6\n") != std::string::npos);
    REQUIRE(output.str().find("Summary: yes\n") != std::string::npos);
    REQUIRE(output.str().find("Retained messages: 4\n") != std::string::npos);
    REQUIRE(output.str().find("Memory: off\n") != std::string::npos);
    REQUIRE(output.str().find("Active memories: 0\n") != std::string::npos);
    REQUIRE(error.str().empty());
}

TEST_CASE(interactive_cli_renders_compaction_result_flags_without_provider_details) {
    for (const auto outcome : {0, 1, 2}) {
        auto current = fixtures::state(fixtures::kFirstSession, "E:/workspace");
        agent::InteractiveSessionCommands commands;
        commands.list = [current] {
            return agent::Result<std::vector<agent::SessionState>>::success({current});
        };
        commands.submit = [current, outcome](const std::string&, const std::string&,
                                            const agent::RuntimeProgressObserver&, bool) {
            auto result = fixtures::completed(current, "question", "answer");
            result.compacted = outcome == 0;
            if (outcome != 0) result.warning = agent::RuntimeError{
                agent::ErrorCode::ProtocolFailure, "PRIVATE_PROVIDER_BODY", true};
            if (outcome == 2) result.error = agent::RuntimeError{
                agent::ErrorCode::BudgetExceeded, "PRIVATE_PROVIDER_BODY", false};
            return result;
        };
        std::istringstream input("question\n/exit\n");
        std::ostringstream output, error;
        agent::InteractiveCli cli(std::move(commands), "model", "E:/default",
                                  input, output, error);
        REQUIRE(cli.run() == agent::ExitCode::Success);
        REQUIRE((output.str().find("Context compacted.\n") != std::string::npos) ==
                (outcome == 0));
        REQUIRE((error.str().find("using retained context") != std::string::npos) ==
                (outcome == 1));
        REQUIRE((error.str().find("turn was not started") != std::string::npos) ==
                (outcome == 2));
        REQUIRE((output.str() + error.str()).find("PRIVATE_PROVIDER_BODY") ==
                std::string::npos);
    }
}

namespace memory_cli_fixture {

class Clock final : public agent::Clock {
public:
    std::string now_utc() const override { return "2026-09-07T12:00:00.000Z"; }
    std::int64_t monotonic_ms() const override { return 1000; }
};

class Ids final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        std::ostringstream stream;
        stream << "task-" << std::hex << std::setw(32) << std::setfill('0') << next++;
        return stream.str();
    }
    std::string next_correlation_id() override { return "corr-" + std::to_string(next++); }
    std::uint64_t next{1};
};

class Consolidator final : public agent::MemoryConsolidator {
public:
    agent::Result<std::vector<agent::MemoryCandidate>> consolidate(
        const agent::MemoryConsolidationInput& input) override {
        inputs.push_back(input);
        if (throws) throw std::runtime_error("PRIVATE_PROVIDER_BODY");
        if (fails) return agent::Result<std::vector<agent::MemoryCandidate>>::failure(
            {agent::ErrorCode::ProtocolFailure, "PRIVATE_PROVIDER_BODY", true});
        return agent::Result<std::vector<agent::MemoryCandidate>>::success({});
    }
    bool fails{false}, throws{false};
    std::vector<agent::MemoryConsolidationInput> inputs;
};

class Compactor final : public agent::ContextCompactor {
public:
    agent::Result<std::string> compact(const agent::ContextCompactionInput&) override {
        return agent::Result<std::string>::success("C++ retained summary");
    }
};

struct Fixture {
    test::ScopedTempDir root{"task-six-cli"};
    agent::JsonlSessionStore sessions{root.path()};
    agent::JsonlMemoryStore memories{root.path()};
    Clock clock;
    Ids ids;
    Consolidator consolidator;
    Compactor compactor;
    agent::MemoryRetriever retriever;
    agent::MemoryPolicy policy{{1024, {"task-six-protected-fixture"}}};
    agent::MemoryEngine memory{memories, sessions, consolidator, retriever, policy, clock, ids};
    std::vector<agent::RunRequest> requests;
    agent::SessionEngine engine;
    agent::SessionState initial;
    std::string output, error;

    explicit Fixture(agent::SessionContextSettings context = {})
        : engine(sessions, clock, ids,
            [this](const agent::RunRequest& request, const agent::RuntimeProgressObserver&) {
                requests.push_back(request);
                agent::TaskState task;
                task.task_id = *request.requested_task_id;
                task.status = agent::TaskStatus::Completed;
                task.workspace_utf8 = request.workspace_utf8;
                task.session_link = request.session_link;
                task.messages = request.initial_messages;
                task.messages.push_back({agent::Role::User, {agent::TextBlock{request.issue}}});
                task.messages.push_back({agent::Role::Assistant, {agent::TextBlock{"answer"}}});
                task.final_text = "answer";
                return agent::RuntimeResult{std::move(task), std::nullopt};
            },
            [](const agent::ResumeRequest&, const agent::RuntimeProgressObserver&) {
                return agent::RuntimeResult{};
            },
            [](const std::string&) {
                return agent::Result<std::optional<std::vector<agent::RuntimeEvent>>>::success(std::nullopt);
            }, {"fixture base prompt", {}}, &compactor, &memory, context) {
        initial = create("E:/workspace");
    }

    agent::SessionState create(const std::string& workspace) {
        const auto created = engine.create_session(workspace, "fixture-model");
        REQUIRE(created.has_value());
        return created.value();
    }

    void turn(const agent::SessionState& session, const std::string& text = "C++ question") {
        const auto result = engine.submit_turn(session.session_id, text, {}, false);
        REQUIRE(!result.error.has_value());
    }

    agent::InteractiveSessionCommands commands() {
        agent::InteractiveSessionCommands result;
        result.list = [this] { return engine.list_sessions(); };
        result.create = [this](const std::string& workspace) {
            return engine.create_session(workspace, "fixture-model");
        };
        result.load = [this](const std::string& id) { return engine.load_session(id); };
        result.submit = [this](const std::string& id, const std::string& text,
                               const agent::RuntimeProgressObserver& observer, bool use_memory) {
            return engine.submit_turn(id, text, observer, use_memory);
        };
        result.recover = [this](const std::string& id,
                                const agent::RuntimeProgressObserver& observer, bool use_memory) {
            return engine.recover_pending_turn(id, observer, use_memory);
        };
        result.memories = [this](const std::string& workspace) { return memory.list(workspace); };
        result.remember = [this](const std::string& id, const std::string& text) {
            return memory.remember(id, text);
        };
        result.forget = [this](const std::string& id) { return memory.forget(id); };
        result.consolidate = [this](const std::string& id) {
            const auto result = memory.consolidate(id);
            return result.has_value() ? agent::Result<void>::success() :
                agent::Result<void>::failure(result.error());
        };
        return result;
    }

    void run(const std::string& text, bool enabled = true,
             std::optional<agent::InteractiveSessionCommands> supplied = std::nullopt) {
        std::istringstream input(text);
        std::ostringstream out, err;
        agent::InteractiveCli cli(supplied.has_value() ? std::move(*supplied) : commands(),
                                  "fixture-model", "E:/workspace", input, out, err, enabled);
        REQUIRE(cli.run() == agent::ExitCode::Success);
        output = out.str();
        error = err.str();
    }

    agent::MemoryState state() {
        const auto loaded = memories.read_state();
        REQUIRE(loaded.has_value());
        return loaded.value();
    }
};

} // namespace memory_cli_fixture

TEST_CASE(interactive_memory_off_keeps_explicit_commands_and_disables_automatic_work) {
    memory_cli_fixture::Fixture fixture;
    const auto old = fixture.memory.remember(fixture.initial.session_id, "C++ old memory");
    REQUIRE(old.has_value());
    fixture.run("/memory off\n/remember C++ new memory\n/memories\n/forget " +
                old.value().memory_id + "\nC++ question\n/status\n/memory status\n/exit\n");
    const auto stored = fixture.state();
    REQUIRE(stored.active_entries.size() == 1);
    REQUIRE(stored.active_entries.begin()->second.content == "C++ new memory");
    REQUIRE(stored.session_checkpoints.empty());
    REQUIRE(fixture.requests.size() == 1);
    REQUIRE(fixture.requests.front().system_prompt.find("Long-term memories") == std::string::npos);
    REQUIRE(fixture.output.find("Forgotten: " + old.value().memory_id + "\n") != std::string::npos);
    REQUIRE(fixture.output.find("Memory: off\n") != std::string::npos);
    REQUIRE(fixture.output.find("Active memories: 1\n") != std::string::npos);
    REQUIRE(fixture.error.empty());
    const auto restarted = fixture.commands();
    fixture.run("/memory status\nC++ question\n/exit\n", true, restarted);
    REQUIRE(fixture.output.find("Memory: on\n") != std::string::npos);
    REQUIRE(fixture.requests.back().system_prompt.find("C++ new memory") != std::string::npos);
}

TEST_CASE(interactive_memory_toggle_controls_subsequent_turns_and_exit_checkpoint) {
    memory_cli_fixture::Fixture fixture;
    const auto remembered = fixture.memory.remember(fixture.initial.session_id, "C++ tests are offline");
    REQUIRE(remembered.has_value());
    fixture.run("/memory off\nC++ one\n/memory on\nC++ two\n/memory status\n/exit\n");
    REQUIRE(fixture.requests.size() == 2);
    REQUIRE(fixture.requests[0].system_prompt.find("Long-term memories") == std::string::npos);
    REQUIRE(fixture.requests[1].system_prompt.find("C++ tests are offline") != std::string::npos);
    REQUIRE(fixture.state().session_checkpoints.at(fixture.initial.session_id) == 2);
    REQUIRE(fixture.error.empty());
}

TEST_CASE(interactive_config_disabled_memory_commands_cannot_enable_or_mutate_store) {
    memory_cli_fixture::Fixture fixture;
    fixture.run("/memories\n/remember C++ note\n/forget memory-11111111111111111111111111111111\n"
                "/memory on\n/memory off\n/memory status\nC++ question\n/status\n/exit\n", false);
    REQUIRE(fixture.state().last_sequence == 0);
    REQUIRE(fixture.requests.size() == 1);
    REQUIRE(fixture.requests.front().system_prompt.find("Long-term memories") == std::string::npos);
    REQUIRE(fixture.output.find("Memory: off\n") != std::string::npos);
    REQUIRE(fixture.error.find("memory is unavailable; enable it in configuration") != std::string::npos);
    REQUIRE(fixture.error.find("unknown command") == std::string::npos);
}

TEST_CASE(interactive_memory_commands_reject_invalid_arguments_without_echo_or_side_effects) {
    memory_cli_fixture::Fixture fixture;
    fixture.run("/remember\n/remember   \n/forget\n/forget invalid-private-input\n"
                "/forget memory-11111111111111111111111111111111 extra\n"
                "/memories extra\n/memory\n/memory invalid-private-input\n"
                "/memory on extra\n/memorying\n/exit\n");
    REQUIRE(fixture.state().last_sequence == 0);
    REQUIRE(fixture.error.find("remember requires nonempty text\n") != std::string::npos);
    REQUIRE(fixture.error.find("forget requires a valid memory ID\n") != std::string::npos);
    REQUIRE(fixture.error.find("memories takes no arguments\n") != std::string::npos);
    REQUIRE(fixture.error.find("memory requires on, off, or status\n") != std::string::npos);
    REQUIRE(fixture.error.find("unknown command\n") != std::string::npos);
    REQUIRE((fixture.output + fixture.error).find("invalid-private-input") == std::string::npos);
}

TEST_CASE(interactive_memories_empty_then_remember_prints_id_without_content) {
    memory_cli_fixture::Fixture fixture;
    fixture.run("/memories\n/remember C++ unique private note\n/exit\n");
    const auto state = fixture.state();
    REQUIRE(state.active_entries.size() == 1);
    REQUIRE(fixture.output.find("No active memories.\n") != std::string::npos);
    REQUIRE(fixture.output.find(state.active_entries.begin()->first + "\n") != std::string::npos);
    REQUIRE(fixture.output.find("C++ unique private note") == std::string::npos);
    REQUIRE(fixture.error.empty());
}

TEST_CASE(interactive_memories_filters_workspace_sorts_ids_and_sanitizes_terminal_data) {
    memory_cli_fixture::Fixture fixture;
    const auto saved = fixture.memory.remember(fixture.initial.session_id, "C++ local memory");
    REQUIRE(saved.has_value());
    auto local = saved.value();
    local.memory_id = "memory-22222222222222222222222222222222";
    local.content = "C++ local\x1b[31m";
    auto global = local;
    global.memory_id = "memory-11111111111111111111111111111111";
    global.scope_utf8.clear();
    global.category = agent::MemoryCategory::Preference;
    global.content = "C++ global";
    auto foreign = local;
    foreign.scope_utf8 += "/child";
    foreign.content = "foreign-private-note";
    auto commands = fixture.commands();
    commands.memories = [local, global, foreign](const std::string&) {
        return agent::Result<std::vector<agent::MemoryEntry>>::success({local, foreign, global});
    };
    fixture.run("/memories\n/status\n/exit\n", true, std::move(commands));
    REQUIRE(fixture.output.find(global.memory_id) < fixture.output.find(local.memory_id));
    REQUIRE(fixture.output.find("preference | global | C++ global") != std::string::npos);
    REQUIRE(fixture.output.find("fact | " + local.scope_utf8 +
                                " | C++ local\\u001B[31m") !=
            std::string::npos);
    REQUIRE(fixture.output.find('\x1b') == std::string::npos);
    REQUIRE(fixture.output.find("foreign-private-note") == std::string::npos);
    REQUIRE(fixture.output.find("Active memories: 2\n") != std::string::npos);
}

TEST_CASE(interactive_memory_consolidates_before_new_clear_exit_and_eof) {
    for (const auto& boundary : {std::string("/new\n"), std::string("/clear\n"),
                                 std::string("/exit\n"), std::string{}}) {
        memory_cli_fixture::Fixture fixture;
        fixture.run("C++ question\n" + boundary);
        REQUIRE(fixture.state().session_checkpoints.at(fixture.initial.session_id) == 1);
        REQUIRE(fixture.consolidator.inputs.size() == 1);
        REQUIRE(fixture.error.empty());
        const auto listed = fixture.engine.list_sessions();
        REQUIRE(listed.has_value());
        REQUIRE(listed.value().size() == ((boundary == "/new\n" || boundary == "/clear\n") ? 2U : 1U));
    }
}

TEST_CASE(interactive_startup_catches_up_every_session_before_recovering_latest) {
    memory_cli_fixture::Fixture fixture;
    fixture.turn(fixture.initial);
    const auto latest = fixture.create("E:/latest");
    fixture.turn(latest);
    auto latest_state = fixture.engine.load_session(latest.session_id).value();
    const auto pending_id = fixture.ids.next_task_id();
    REQUIRE(fixture.sessions.append({1, latest_state.last_sequence + 1, latest.session_id,
        fixture.clock.now_utc(), fixture.ids.next_correlation_id(),
        agent::SessionTurnStartedPayload{2, pending_id, "C++ recovered"}}).has_value());
    latest_state = fixture.engine.load_session(latest.session_id).value();
    auto commands = fixture.commands();
    commands.list = [&] {
        return agent::Result<std::vector<agent::SessionState>>::success(
            {latest_state, fixture.engine.load_session(fixture.initial.session_id).value()});
    };
    commands.recover = [&](const std::string& id, const agent::RuntimeProgressObserver& observer,
                            bool use_memory) {
        const auto memory = fixture.state();
        REQUIRE(memory.session_checkpoints.at(latest.session_id) == 1);
        REQUIRE(memory.session_checkpoints.at(fixture.initial.session_id) == 1);
        REQUIRE(use_memory);
        return fixture.engine.recover_pending_turn(id, observer, use_memory);
    };
    fixture.run("/memory off\n/exit\n", true, std::move(commands));
    REQUIRE(fixture.output.find("Workspace: " + latest.workspace_utf8 + "\n") !=
            std::string::npos);
    REQUIRE(fixture.requests.back().issue == "C++ recovered");
    REQUIRE(fixture.consolidator.inputs.size() == 2);
    REQUIRE(fixture.error.empty());
}

TEST_CASE(interactive_consolidation_failure_is_safe_nonblocking_and_retryable_at_every_boundary) {
    for (const auto& boundary : {std::string("/new\n"), std::string("/clear\n"),
                                 std::string("/exit\n"), std::string{}}) {
        for (const bool throws : {false, true}) {
            memory_cli_fixture::Fixture fixture;
            fixture.turn(fixture.initial);
            fixture.consolidator.fails = !throws;
            fixture.consolidator.throws = throws;
            fixture.run("C++ another question\n" + boundary);
            REQUIRE(fixture.state().session_checkpoints.empty());
            REQUIRE(fixture.output.find("answer\n") != std::string::npos);
            REQUIRE(fixture.error.find("memory consolidation failed; it will be retried\n") != std::string::npos);
            REQUIRE((fixture.output + fixture.error).find("PRIVATE_PROVIDER_BODY") == std::string::npos);
            fixture.consolidator.fails = false;
            fixture.consolidator.throws = false;
            fixture.run("/exit\n");
            REQUIRE(fixture.state().session_checkpoints.at(fixture.initial.session_id) == 2);
            REQUIRE(fixture.error.empty());
        }
    }
}

TEST_CASE(interactive_memory_failures_hide_provider_messages_and_allow_later_commands) {
    memory_cli_fixture::Fixture fixture;
    auto commands = fixture.commands();
    commands.memories = [](const std::string&) {
        return agent::Result<std::vector<agent::MemoryEntry>>::failure(
            {agent::ErrorCode::PersistenceFailure, "PRIVATE_PROVIDER_BODY", true});
    };
    commands.remember = [](const std::string&, const std::string&) -> agent::Result<agent::MemoryEntry> {
        throw std::runtime_error("PRIVATE_PROVIDER_BODY");
    };
    commands.forget = [](const std::string&) {
        return agent::Result<void>::failure(
            {agent::ErrorCode::PersistenceFailure, "PRIVATE_PROVIDER_BODY", true});
    };
    fixture.run("/memories\n/remember C++ note\n/forget memory-11111111111111111111111111111111\n"
                "/status\n/exit\n", true, std::move(commands));
    REQUIRE(fixture.error.find("memories could not be loaded\n") != std::string::npos);
    REQUIRE(fixture.error.find("memory could not be saved\n") != std::string::npos);
    REQUIRE(fixture.error.find("memory could not be forgotten\n") != std::string::npos);
    REQUIRE(fixture.output.find("Active memories: unavailable\n") != std::string::npos);
    REQUIRE((fixture.output + fixture.error).find("PRIVATE_PROVIDER_BODY") == std::string::npos);
}

TEST_CASE(interactive_memory_off_preserves_summary_compaction) {
    agent::SessionContextSettings context;
    context.threshold_bytes = 64;
    context.retain_turns = 1;
    memory_cli_fixture::Fixture fixture(context);
    fixture.turn(fixture.initial, std::string(40, 'x'));
    fixture.turn(fixture.initial, std::string(40, 'y'));
    fixture.run("/memory off\nC++ question\n/status\n/exit\n");
    REQUIRE(fixture.requests.back().system_prompt.find("Session summary") != std::string::npos);
    REQUIRE(fixture.requests.back().system_prompt.find("Long-term memories") == std::string::npos);
    REQUIRE(fixture.output.find("Context compacted.\n") != std::string::npos);
    REQUIRE(fixture.output.find("Summary: yes\n") != std::string::npos);
    REQUIRE(fixture.state().session_checkpoints.at(fixture.initial.session_id) == 2);
    REQUIRE(fixture.error.empty());
}
