#include "application/session_engine.h"
#include "application/memory_engine.h"
#include "application/memory_policy.h"
#include "application/memory_retriever.h"
#include "application/session_reducer.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "ports/context_compactor.h"
#include "ports/memory_consolidator.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/session_store.h"
#include "test_support.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace test {

class MemorySessionStore final : public agent::SessionStore {
public:
    agent::Result<void> append(const agent::SessionEvent& event) override {
        if (fail_compaction &&
            std::holds_alternative<agent::SessionCompactedPayload>(event.payload)) {
            return agent::Result<void>::failure(
                {agent::ErrorCode::PersistenceFailure, "private persistence body", true});
        }
        std::optional<agent::SessionState> current;
        if (!events.empty()) {
            auto replayed = agent::replay_session_events(events);
            if (!replayed.has_value()) {
                return agent::Result<void>::failure(replayed.error());
            }
            current = std::move(replayed.value());
        }
        auto reduced = agent::reduce_session_event(current, event);
        if (!reduced.has_value()) {
            return agent::Result<void>::failure(reduced.error());
        }
        events.push_back(event);
        return agent::Result<void>::success();
    }

    agent::Result<std::vector<agent::SessionEvent>> read_session(
        const std::string& session_id) const override {
        if (events.empty() || events.front().session_id != session_id) {
            return agent::Result<std::vector<agent::SessionEvent>>::failure(
                {agent::ErrorCode::PersistenceFailure,
                 "missing session", false});
        }
        return agent::Result<std::vector<agent::SessionEvent>>::success(events);
    }

    agent::Result<std::vector<agent::SessionState>> list_sessions()
        const override {
        if (events.empty()) {
            return agent::Result<std::vector<agent::SessionState>>::success({});
        }
        auto replayed = agent::replay_session_events(events);
        if (!replayed.has_value()) {
            return agent::Result<std::vector<agent::SessionState>>::failure(
                replayed.error());
        }
        return agent::Result<std::vector<agent::SessionState>>::success(
            {replayed.value()});
    }

    std::vector<agent::SessionEvent> events;
    bool fail_compaction{false};
};

class SessionClock final : public agent::Clock {
public:
    std::string now_utc() const override {
        return "2026-09-06T12:00:00.000Z";
    }
    std::int64_t monotonic_ms() const override { return 1000; }
};

class SessionIds final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        const char digit = static_cast<char>('0' + next_task_++);
        return "task-" + std::string(32, digit);
    }
    std::string next_correlation_id() override {
        return "corr-session-" + std::to_string(next_correlation_++);
    }

private:
    int next_task_{1};
    int next_correlation_{1};
};

agent::RuntimeResult completed_task(const agent::RunRequest& request,
                                    const std::string& answer) {
    agent::TaskState state;
    state.task_id = *request.requested_task_id;
    state.status = agent::TaskStatus::Completed;
    state.workspace_utf8 = request.workspace_utf8;
    state.session_link = request.session_link;
    state.messages = request.initial_messages;
    state.messages.push_back(
        {agent::Role::User, {agent::TextBlock{request.issue}}});
    state.messages.push_back(
        {agent::Role::Assistant, {agent::TextBlock{answer}}});
    state.final_text = answer;
    return {state, std::nullopt};
}

agent::RuntimeEvent task_event(const std::string& task_id,
                               std::uint64_t sequence,
                               agent::EventPayload payload) {
    return {1, sequence, task_id, "2026-09-06T12:00:00.000Z",
            "corr-task-" + std::to_string(sequence), std::move(payload)};
}

std::vector<agent::RuntimeEvent> awaiting_model_task_events(
    const std::string& task_id,
    const std::string& workspace = "E:/workspace") {
    const agent::SessionTaskLink link{
        "session-11111111111111111111111111111111", 1};
    return {
        task_event(task_id, 1,
                   agent::TaskStartedPayload{
                       "question", workspace, {4, 8, 60'000, 30'000}, {},
                       link}),
        task_event(task_id, 2,
                   agent::ContextPreparationStartedPayload{}),
        task_event(task_id, 3,
                   agent::ContextPreparedPayload{agent::EvidencePack{}}),
    };
}

std::vector<agent::RuntimeEvent> awaiting_tool_task_events(
    const std::string& task_id,
    const std::string& workspace = "E:/workspace") {
    auto events = awaiting_model_task_events(task_id, workspace);
    const std::vector<agent::Message> messages{
        {agent::Role::User, {agent::TextBlock{"question"}}}};
    events.push_back(task_event(
        task_id, 4,
        agent::ModelCallStartedPayload{
            {"system", messages, {}, 30'000, agent::EvidencePack{}}}));
    events.push_back(task_event(
        task_id, 5,
        agent::ModelCallSucceededPayload{
            {{agent::TextBlock{"working"},
              agent::ToolUseBlock{{"call-1", "read_file",
                                    agent::Value::object(
                                        {{"path", "src/main.cpp"}})}}},
             agent::StopReason::ToolUse, "tool_use", 10, 5,
             "request-1"}}));
    return events;
}

std::vector<agent::RuntimeEvent> completed_task_events(
    const std::string& task_id,
    const std::string& workspace = "E:/workspace") {
    auto events = awaiting_model_task_events(task_id, workspace);
    const std::vector<agent::Message> messages{
        {agent::Role::User, {agent::TextBlock{"question"}}}};
    events.push_back(task_event(
        task_id, 4,
        agent::ModelCallStartedPayload{
            {"system", messages, {}, 30'000, agent::EvidencePack{}}}));
    events.push_back(task_event(
        task_id, 5,
        agent::ModelCallSucceededPayload{
            {{agent::TextBlock{"answer"}}, agent::StopReason::EndTurn,
             "end_turn", 10, 5, "request-1"}}));
    events.push_back(task_event(
        task_id, 6, agent::TaskCompletedPayload{"answer"}));
    return events;
}

}  // namespace test

TEST_CASE(session_engine_persists_a_stable_absolute_workspace_identity) {
    test::ScopedTempDir temp{"session-workspace-identity"};
    const auto workspace = temp.path() / "workspace";
    std::filesystem::create_directories(workspace);
    const auto aliased = workspace / "child" / "..";

    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    agent::SessionEngine engine(
        store, clock, ids,
        [](const agent::RunRequest&, const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const agent::ResumeRequest&, const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const std::string&) {
            return agent::Result<std::optional<std::vector<agent::RuntimeEvent>>>::success(
                std::nullopt);
        }, {"system", {}});

    const auto created = engine.create_session(aliased.u8string(), "model");

    REQUIRE(created.has_value());
    auto expected = workspace.lexically_normal().generic_u8string();
#if defined(_WIN32)
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
#endif
    REQUIRE(created.value().workspace_utf8 == expected);
    const auto persisted = std::filesystem::u8path(created.value().workspace_utf8);
    REQUIRE(persisted.is_absolute());
    REQUIRE(created.value().workspace_utf8.find("/../") == std::string::npos);
    REQUIRE(created.value().workspace_utf8.find("\\..\\") == std::string::npos);
    REQUIRE(std::get<agent::SessionStartedPayload>(store.events.front().payload)
                .workspace_utf8 == created.value().workspace_utf8);
}

TEST_CASE(session_engine_rejects_legacy_relative_workspace_sessions) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    const std::string session_id{"session-11111111111111111111111111111111"};
    REQUIRE(store.append({1, 1, session_id, clock.now_utc(),
                          ids.next_correlation_id(),
                          agent::SessionStartedPayload{"relative/workspace", "model"}})
                .has_value());
    agent::SessionEngine engine(
        store, clock, ids,
        [](const agent::RunRequest&, const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const agent::ResumeRequest&, const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const std::string&) {
            return agent::Result<std::optional<std::vector<agent::RuntimeEvent>>>::success(
                std::nullopt);
        }, {"system", {}});

    const auto loaded = engine.load_session(session_id);

    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(session_engine_second_turn_receives_exact_first_turn_transcript) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    std::vector<agent::RunRequest> requests;
    agent::SessionRunTask run = [&requests](
        const agent::RunRequest& request,
        const agent::RuntimeProgressObserver&) {
        requests.push_back(request);
        return test::completed_task(
            request, requests.size() == 1 ? "answer one" : "answer two");
    };
    agent::SessionEngine engine(
        store, clock, ids, std::move(run),
        [](const agent::ResumeRequest&,
           const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{
                std::nullopt,
                agent::RuntimeError{agent::ErrorCode::InvalidInput,
                                    "unexpected resume", false}};
        },
        [](const std::string&) {
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(std::nullopt);
        },
        {"system", {4, 8, 60'000, 30'000}});
    const auto created = engine.create_session("E:/workspace", "MiniMax-M3");
    REQUIRE(created.has_value());

    const auto first = engine.submit_turn(
        created.value().session_id, "first question", {});
    REQUIRE(first.session.has_value());
    REQUIRE(!first.error.has_value());
    const auto second = engine.submit_turn(
        created.value().session_id, "second question", {});

    REQUIRE(second.session.has_value());
    REQUIRE(!second.error.has_value());
    REQUIRE(requests.size() == 2);
    const std::vector<agent::Message> expected_history{
        {agent::Role::User, {agent::TextBlock{"first question"}}},
        {agent::Role::Assistant, {agent::TextBlock{"answer one"}}},
    };
    REQUIRE(requests.at(1).initial_messages == expected_history);
    REQUIRE(requests.at(1).issue == "second question");
    const std::optional<agent::SessionTaskLink> expected_link{
        {created.value().session_id, 2}};
    REQUIRE(requests.at(1).session_link == expected_link);
    REQUIRE(second.session->completed_turns == 2);
    REQUIRE(second.session->messages.size() == 4);
}

TEST_CASE(session_engine_persists_turn_start_before_runtime_invocation) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    bool observed_pending = false;
    agent::SessionEngine* engine_pointer = nullptr;
    agent::SessionRunTask run = [&](const agent::RunRequest& request,
                                    const agent::RuntimeProgressObserver&) {
        const auto loaded = engine_pointer->load_session(
            request.session_link->session_id);
        observed_pending = loaded.has_value() &&
                           loaded.value().pending_turn.has_value() &&
                           loaded.value().pending_turn->task_id ==
                               request.requested_task_id;
        return test::completed_task(request, "answer");
    };
    agent::SessionEngine engine(
        store, clock, ids, std::move(run),
        [](const agent::ResumeRequest&,
           const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const std::string&) {
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(std::nullopt);
        },
        {"system", {4, 8, 60'000, 30'000}});
    engine_pointer = &engine;
    const auto created = engine.create_session("E:/workspace", "model");

    const auto result = engine.submit_turn(
        created.value().session_id, "question", {});

    REQUIRE(result.session.has_value());
    REQUIRE(observed_pending);
}

TEST_CASE(session_engine_recovers_existing_pending_task_via_resume) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    int run_calls = 0;
    int resume_calls = 0;
    std::vector<agent::RuntimeEvent> resumed_events;
    agent::RunRequest recovered_request;
    std::string normalized_workspace;
    agent::SessionEngine engine(
        store, clock, ids,
        [&run_calls](const agent::RunRequest&,
                     const agent::RuntimeProgressObserver&) {
            ++run_calls;
            return agent::RuntimeResult{};
        },
        [&resume_calls, &resumed_events, &recovered_request,
         &normalized_workspace](
            const agent::ResumeRequest& request,
            const agent::RuntimeProgressObserver&) {
            ++resume_calls;
            resumed_events = request.durable_events;
            const auto& task_id = request.durable_events.front().task_id;
            recovered_request = {"question", normalized_workspace, "system",
                                 {4, 8, 60'000, 30'000}, {}, task_id,
                                 agent::SessionTaskLink{
                                     "session-11111111111111111111111111111111",
                                     1}};
            return test::completed_task(recovered_request, "answer");
        },
        [&normalized_workspace](const std::string& task_id) {
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(
                    test::awaiting_model_task_events(task_id,
                                                     normalized_workspace));
        },
        {"system", {4, 8, 60'000, 30'000}});
    const auto created = engine.create_session("E:/workspace", "model");
    REQUIRE(created.has_value());
    normalized_workspace = created.value().workspace_utf8;
    const std::string task_id =
        "task-22222222222222222222222222222222";
    REQUIRE(store.append({1, 2, created.value().session_id,
                          clock.now_utc(), ids.next_correlation_id(),
                          agent::SessionTurnStartedPayload{
                              1, task_id, "question"}})
                .has_value());

    const auto recovered = engine.recover_pending_turn(
        created.value().session_id, {});

    REQUIRE(recovered.session.has_value());
    REQUIRE(!recovered.error.has_value());
    REQUIRE(run_calls == 0);
    REQUIRE(resume_calls == 1);
    REQUIRE(resumed_events ==
            test::awaiting_model_task_events(task_id, normalized_workspace));
    REQUIRE(recovered.session->messages.size() == 2);
    REQUIRE(recovered.session->last_task_id ==
            std::optional<std::string>{task_id});
}

TEST_CASE(session_engine_starts_missing_pending_task_with_same_id) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    std::optional<agent::RunRequest> observed;
    agent::SessionEngine engine(
        store, clock, ids,
        [&observed](const agent::RunRequest& request,
                    const agent::RuntimeProgressObserver&) {
            observed = request;
            return test::completed_task(request, "answer");
        },
        [](const agent::ResumeRequest&,
           const agent::RuntimeProgressObserver&) {
            return agent::RuntimeResult{};
        },
        [](const std::string&) {
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(std::nullopt);
        },
        {"system", {4, 8, 60'000, 30'000}});
    const auto created = engine.create_session("E:/workspace", "model");
    const std::string task_id =
        "task-22222222222222222222222222222222";
    REQUIRE(store.append({1, 2, created.value().session_id,
                          clock.now_utc(), ids.next_correlation_id(),
                          agent::SessionTurnStartedPayload{
                              1, task_id, "question"}})
                .has_value());

    const auto recovered = engine.recover_pending_turn(
        created.value().session_id, {});

    REQUIRE(recovered.session.has_value());
    REQUIRE(observed.has_value());
    REQUIRE(observed->requested_task_id ==
            std::optional<std::string>{task_id});
    REQUIRE(observed->issue == "question");
}

TEST_CASE(session_engine_rejects_mismatched_recovery_before_resume) {
    for (const auto status : {agent::TaskStatus::AwaitingModel,
                              agent::TaskStatus::AwaitingTool}) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    int run_calls = 0;
    int resume_calls = 0;
    agent::SessionEngine engine(
        store, clock, ids,
        [&run_calls](const agent::RunRequest&,
                     const agent::RuntimeProgressObserver&) {
            ++run_calls;
            return agent::RuntimeResult{};
        },
        [&resume_calls](const agent::ResumeRequest& request,
                        const agent::RuntimeProgressObserver&) {
            ++resume_calls;
            static_cast<void>(request);
            return agent::RuntimeResult{};
        },
        [status](const std::string& task_id) {
            auto events = status == agent::TaskStatus::AwaitingModel
                              ? test::awaiting_model_task_events(
                                    task_id, "E:/other-workspace")
                              : test::awaiting_tool_task_events(
                                    task_id, "E:/other-workspace");
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(
                    std::move(events));
        },
        {"system", {4, 8, 60'000, 30'000}});
    const auto created = engine.create_session("E:/workspace", "model");
    REQUIRE(created.has_value());
    const std::string task_id =
        "task-22222222222222222222222222222222";
    REQUIRE(store.append({1, 2, created.value().session_id,
                          clock.now_utc(), ids.next_correlation_id(),
                          agent::SessionTurnStartedPayload{
                              1, task_id, "question"}})
                .has_value());

    const auto recovered = engine.recover_pending_turn(
        created.value().session_id, {});

    REQUIRE(recovered.error.has_value());
    REQUIRE(run_calls == 0);
    REQUIRE(resume_calls == 0);
    }
}

TEST_CASE(session_engine_commits_loaded_terminal_task_without_resume) {
    test::MemorySessionStore store;
    test::SessionClock clock;
    test::SessionIds ids;
    int run_calls = 0;
    int resume_calls = 0;
    std::string normalized_workspace;
    agent::SessionEngine engine(
        store, clock, ids,
        [&run_calls](const agent::RunRequest&,
                     const agent::RuntimeProgressObserver&) {
            ++run_calls;
            return agent::RuntimeResult{};
        },
        [&resume_calls](const agent::ResumeRequest&,
                        const agent::RuntimeProgressObserver&) {
            ++resume_calls;
            return agent::RuntimeResult{};
        },
        [&normalized_workspace](const std::string& task_id) {
            return agent::Result<std::optional<
                std::vector<agent::RuntimeEvent>>>::success(
                    test::completed_task_events(task_id,
                                                normalized_workspace));
        },
        {"system", {4, 8, 60'000, 30'000}});
    const auto created = engine.create_session("E:/workspace", "model");
    REQUIRE(created.has_value());
    normalized_workspace = created.value().workspace_utf8;
    const std::string task_id =
        "task-22222222222222222222222222222222";
    REQUIRE(store.append({1, 2, created.value().session_id,
                          clock.now_utc(), ids.next_correlation_id(),
                          agent::SessionTurnStartedPayload{
                              1, task_id, "question"}})
                .has_value());

    const auto recovered = engine.recover_pending_turn(
        created.value().session_id, {});

    REQUIRE(!recovered.error.has_value());
    REQUIRE(recovered.session.has_value());
    REQUIRE(recovered.session->completed_turns == 1);
    REQUIRE(run_calls == 0);
    REQUIRE(resume_calls == 0);
}

namespace context_engine_test {

class Compactor final : public agent::ContextCompactor {
public:
    agent::Result<std::string> compact(const agent::ContextCompactionInput& input) override {
        if (on_compact) on_compact();
        inputs.push_back(input);
        if (throws) throw std::runtime_error("private compactor body");
        if (fails) return agent::Result<std::string>::failure(
            {agent::ErrorCode::ProtocolFailure, "private compactor body", true});
        return agent::Result<std::string>::success(summary);
    }
    bool fails{false}, throws{false};
    std::function<void()> on_compact;
    std::string summary{"old facts"};
    std::vector<agent::ContextCompactionInput> inputs;
};

class Consolidator final : public agent::MemoryConsolidator {
public:
    agent::Result<std::vector<agent::MemoryCandidate>> consolidate(
        const agent::MemoryConsolidationInput&) override {
        return agent::Result<std::vector<agent::MemoryCandidate>>::success({});
    }
};

struct Fixture {
    test::ScopedTempDir root{"agent-session-context"};
    test::MemorySessionStore sessions;
    test::SessionClock clock;
    test::SessionIds ids;
    Compactor compactor;
    agent::JsonlMemoryStore memories{root.path()};
    Consolidator consolidator;
    agent::MemoryRetriever retriever;
    agent::MemoryPolicy policy{{1024, {}}};
    agent::MemoryEngine memory{memories, sessions, consolidator, retriever, policy, clock, ids};
    std::vector<agent::RunRequest> requests;
    std::vector<agent::ResumeRequest> resumes;
    std::optional<std::vector<agent::RuntimeEvent>> task_events;
    const std::string session_id{"session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};

    Fixture() {
        REQUIRE(sessions.append({1, 1, session_id, clock.now_utc(), "corr-start",
            agent::SessionStartedPayload{"E:/workspace", "model"}}).has_value());
    }

    void append(agent::SessionEventPayload payload) {
        REQUIRE(sessions.append({1, sessions.events.size() + 1, session_id,
            clock.now_utc(), ids.next_correlation_id(), std::move(payload)}).has_value());
    }

    void seed_turns(int count, std::size_t user_bytes = 200) {
        for (int index = 1; index <= count; ++index) {
            const auto task_id = "task-" + std::string(32, static_cast<char>('5' + index));
            const auto text = std::string(user_bytes, static_cast<char>('a' + index));
            append(agent::SessionTurnStartedPayload{
                static_cast<std::uint64_t>(index), task_id, text});
            append(agent::SessionTurnCommittedPayload{
                static_cast<std::uint64_t>(index), task_id,
                {{agent::Role::User, {agent::TextBlock{text}}},
                 {agent::Role::Assistant, {agent::TextBlock{"answer"}}}}});
        }
    }

    agent::SessionEngine engine(agent::SessionContextSettings settings = {},
                                 bool with_memory = false,
                                 std::function<void()> compaction_started = {}) {
        return agent::SessionEngine(sessions, clock, ids,
            [this](const agent::RunRequest& request, const agent::RuntimeProgressObserver&) {
                const auto state = agent::replay_session_events(sessions.events);
                REQUIRE(state.has_value());
                REQUIRE(state.value().pending_turn.has_value());
                requests.push_back(request);
                return test::completed_task(request, "answer");
            },
            [this](const agent::ResumeRequest& request, const agent::RuntimeProgressObserver&) {
                resumes.push_back(request);
                const auto& started = std::get<agent::TaskStartedPayload>(
                    request.durable_events.front().payload);
                return test::completed_task(
                    {started.issue, started.workspace_utf8, request.fallback_system_prompt,
                     started.budgets, started.initial_messages,
                     request.durable_events.front().task_id, started.session_link}, "answer");
            },
            [this](const std::string&) {
                return agent::Result<std::optional<std::vector<agent::RuntimeEvent>>>::success(task_events);
            }, {"system", {4, 8, 60'000, 30'000}}, &compactor,
            with_memory ? &memory : nullptr, settings,
            std::move(compaction_started));
    }

    void pending(const std::string& text = "question") {
        append(agent::SessionTurnStartedPayload{
            5, "task-ffffffffffffffffffffffffffffffff", text});
    }
};

agent::SessionContextSettings settings() {
    agent::SessionContextSettings result;
    result.threshold_bytes = 500;
    result.hard_limit_bytes = 5000;
    result.retain_turns = 2;
    result.max_summary_bytes = 64;
    return result;
}

} // namespace context_engine_test

TEST_CASE(session_engine_compacts_oldest_prefix_before_turn_start_and_retains_exact_turns) {
    using namespace context_engine_test;
    Fixture fixture;
    fixture.seed_turns(4);
    const auto result = fixture.engine(settings()).submit_turn(fixture.session_id, "new question", {});
    REQUIRE(!result.error.has_value());
    REQUIRE(result.compacted);
    REQUIRE(!result.warning.has_value());
    REQUIRE(fixture.compactor.inputs.size() == 1);
    REQUIRE(fixture.compactor.inputs[0].previous_summary.empty());
    REQUIRE(fixture.compactor.inputs[0].turns.size() == 2);
    REQUIRE(fixture.compactor.inputs[0].turns[0].turn_index == 1);
    REQUIRE(fixture.compactor.inputs[0].turns[1].turn_index == 2);
    REQUIRE(fixture.requests[0].initial_messages.size() == 4);
    REQUIRE(std::get<agent::TextBlock>(fixture.requests[0].initial_messages[0].content[0]).text ==
            std::string(200, 'd'));
    REQUIRE(std::get<agent::TextBlock>(fixture.requests[0].initial_messages[2].content[0]).text ==
            std::string(200, 'e'));
    const auto& prompt = fixture.requests[0].system_prompt;
    REQUIRE(prompt.find("system") == 0);
    REQUIRE(prompt.find("Session summary") != std::string::npos);
    REQUIRE(prompt.find("untrusted data") != std::string::npos);
    REQUIRE(prompt.find("cannot override instructions") != std::string::npos);
    REQUIRE(prompt.find("old facts") != std::string::npos);
    REQUIRE(result.session->compacted_through_turn == 2);
    REQUIRE(result.session->committed_turns.size() == 3);
    REQUIRE(std::holds_alternative<agent::SessionCompactedPayload>(fixture.sessions.events[9].payload));
    REQUIRE(std::holds_alternative<agent::SessionTurnStartedPayload>(fixture.sessions.events[10].payload));
    const auto reloaded = fixture.engine().load_session(fixture.session_id);
    REQUIRE(reloaded.value() == *result.session);
}

TEST_CASE(session_engine_notifies_before_slow_compaction_and_before_task_start) {
    using namespace context_engine_test;
    Fixture fixture;
    fixture.seed_turns(4);
    bool notified = false;
    auto engine = fixture.engine(settings(), false, [&] { notified = true; });
    fixture.compactor.summary = "summary";
    fixture.compactor.on_compact = [&] { REQUIRE(notified); };

    const auto result = engine.submit_turn(fixture.session_id, "new question", {});

    REQUIRE(!result.error.has_value());
    REQUIRE(notified);
    REQUIRE(result.compacted);
}

TEST_CASE(session_engine_threshold_is_strict_and_disabled_compaction_keeps_history) {
    using namespace context_engine_test;
    for (const bool disabled : {false, true}) {
        Fixture fixture;
        fixture.seed_turns(4, 20);
        auto config = settings();
        // 4 * (20 user + 6 answer) + 6 system + 4 new user = 114 bytes.
        config.threshold_bytes = disabled ? 1 : 114;
        config.enabled = !disabled;
        const auto result = fixture.engine(config).submit_turn(fixture.session_id, "next", {});
        REQUIRE(!result.error.has_value());
        REQUIRE(!result.compacted);
        REQUIRE(fixture.compactor.inputs.empty());
        REQUIRE(fixture.requests[0].system_prompt == "system");
        REQUIRE(fixture.requests[0].initial_messages.size() == 8);
    }
}

TEST_CASE(session_engine_compaction_includes_previous_summary_and_exact_remaining_prefix) {
    using namespace context_engine_test;
    Fixture fixture;
    fixture.seed_turns(4);
    fixture.append(agent::SessionCompactedPayload{1, "earlier cumulative facts"});
    auto config = settings();
    config.retain_turns = 1;
    const auto result = fixture.engine(config).submit_turn(fixture.session_id, "next", {});
    REQUIRE(!result.error.has_value());
    REQUIRE(fixture.compactor.inputs.size() == 1);
    REQUIRE(fixture.compactor.inputs[0].previous_summary == "earlier cumulative facts");
    REQUIRE(fixture.compactor.inputs[0].turns.size() == 2);
    REQUIRE(fixture.compactor.inputs[0].turns[0].turn_index == 2);
    REQUIRE(fixture.compactor.inputs[0].turns[1].turn_index == 3);
    REQUIRE(fixture.requests[0].initial_messages.size() == 2);
    REQUIRE(result.session->compacted_through_turn == 3);
}

TEST_CASE(session_engine_soft_compaction_failures_preserve_exact_history_with_sanitized_warning) {
    using namespace context_engine_test;
    for (int failure = 0; failure < 6; ++failure) {
        Fixture fixture;
        fixture.seed_turns(4);
        if (failure == 0) fixture.compactor.fails = true;
        if (failure == 1) fixture.compactor.throws = true;
        if (failure == 2) fixture.compactor.summary.clear();
        if (failure == 3) fixture.compactor.summary = std::string(65, 'x');
        if (failure == 4) fixture.compactor.summary = std::string("bad\xc0\xaf");
        if (failure == 5) fixture.sessions.fail_compaction = true;
        const auto result = fixture.engine(settings()).submit_turn(fixture.session_id, "next", {});
        REQUIRE(!result.error.has_value());
        REQUIRE(!result.compacted);
        REQUIRE(result.warning.has_value());
        REQUIRE(result.warning->message.find("private") == std::string::npos);
        REQUIRE(fixture.requests[0].initial_messages.size() == 8);
        REQUIRE(fixture.requests[0].system_prompt == "system");
        REQUIRE(result.session->summary.empty());
        REQUIRE(result.session->compacted_through_turn == 0);
        REQUIRE(fixture.sessions.events.size() == 11);
    }
}

TEST_CASE(session_engine_hard_limit_rejects_before_pending_when_compaction_cannot_reduce_context) {
    using namespace context_engine_test;
    for (int failure = 0; failure < 4; ++failure) {
        Fixture fixture;
        fixture.seed_turns(4);
        auto config = settings();
        config.hard_limit_bytes = 700;
        if (failure == 0) fixture.compactor.fails = true;
        if (failure == 1) fixture.sessions.fail_compaction = true;
        if (failure == 2) config.retain_turns = 4;
        if (failure == 3) {
            config.max_summary_bytes = 512;
            fixture.compactor.summary = std::string(500, 's');
        }
        const auto result = fixture.engine(config).submit_turn(fixture.session_id, "next", {});
        REQUIRE(result.error.has_value());
        REQUIRE(result.error->code == agent::ErrorCode::BudgetExceeded);
        REQUIRE(result.error->message.find("private") == std::string::npos);
        REQUIRE(fixture.requests.empty());
        REQUIRE(!fixture.engine().load_session(fixture.session_id).value().pending_turn.has_value());
        REQUIRE(fixture.sessions.events.size() == (failure == 3 ? 10 : 9));
    }
}

TEST_CASE(session_engine_hard_limit_includes_new_user_text_and_equality_is_rejected) {
    using namespace context_engine_test;
    Fixture fixture;
    auto config = settings();
    config.threshold_bytes = 5;
    config.hard_limit_bytes = 10;
    const auto result = fixture.engine(config).submit_turn(fixture.session_id, "next", {});
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->code == agent::ErrorCode::BudgetExceeded);
    REQUIRE(fixture.sessions.events.size() == 1);
    REQUIRE(fixture.requests.empty());
}

TEST_CASE(session_engine_memory_opt_out_and_disabled_use_skip_only_long_term_section) {
    using namespace context_engine_test;
    for (int mode = 0; mode < 4; ++mode) {
        Fixture fixture;
        fixture.seed_turns(4, 20);
        fixture.append(agent::SessionCompactedPayload{2, "retained session facts"});
        REQUIRE(fixture.memory.remember(fixture.session_id, "C++ project uses CMake").has_value());
        auto config = settings();
        config.threshold_bytes = 2000;
        config.memory_enabled = mode != 3;
        const auto text = mode == 1 ? "C++ do not use memory" : "C++";
        const auto result = fixture.engine(config, true).submit_turn(fixture.session_id, text, {}, mode != 2);
        REQUIRE(!result.error.has_value());
        const auto& prompt = fixture.requests[0].system_prompt;
        REQUIRE(prompt.find("Session summary") != std::string::npos);
        REQUIRE(prompt.find("retained session facts") != std::string::npos);
        REQUIRE((prompt.find("Long-term memories") != std::string::npos) == (mode == 0));
        REQUIRE((prompt.find("C++ project uses CMake") != std::string::npos) == (mode == 0));
        REQUIRE(fixture.requests[0].initial_messages.size() == 4);
    }
}

TEST_CASE(session_engine_corrupt_memory_retrieval_fails_before_turn_started_but_opt_out_still_works) {
    using namespace context_engine_test;
    Fixture fixture;
    const auto path = fixture.memories.event_path();
    REQUIRE(path.has_value());
    fixture.root.write_text(path.value().lexically_relative(fixture.root.path()), "private corrupt body\n");
    const auto result = fixture.engine(settings(), true).submit_turn(fixture.session_id, "C++", {});
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(result.error->message.find("private corrupt body") == std::string::npos);
    REQUIRE(fixture.sessions.events.size() == 1);
    REQUIRE(fixture.requests.empty());
    const auto opted_out = fixture.engine(settings(), true).submit_turn(fixture.session_id, "C++", {}, false);
    REQUIRE(!opted_out.error.has_value());
}

TEST_CASE(session_engine_recovery_without_task_log_uses_summary_retained_history_and_pending_opt_out) {
    using namespace context_engine_test;
    Fixture fixture;
    fixture.seed_turns(4, 20);
    fixture.append(agent::SessionCompactedPayload{2, "recovery summary"});
    REQUIRE(fixture.memory.remember(fixture.session_id, "C++ project uses CMake").has_value());
    fixture.pending("C++ 不要使用记忆");
    const auto result = fixture.engine(settings(), true).recover_pending_turn(fixture.session_id, {});
    REQUIRE(!result.error.has_value());
    REQUIRE(fixture.requests.size() == 1);
    REQUIRE(fixture.requests[0].requested_task_id == "task-ffffffffffffffffffffffffffffffff");
    REQUIRE(fixture.requests[0].initial_messages.size() == 4);
    REQUIRE(fixture.requests[0].system_prompt.find("recovery summary") != std::string::npos);
    REQUIRE(fixture.requests[0].system_prompt.find("Long-term memories") == std::string::npos);
    REQUIRE(fixture.compactor.inputs.empty());
}

TEST_CASE(session_engine_recovery_validates_task_log_against_retained_projection_after_compaction) {
    using namespace context_engine_test;
    for (const bool wrong_history : {false, true}) {
        Fixture fixture;
        fixture.seed_turns(4, 20);
        const auto old_messages = agent::replay_session_events(fixture.sessions.events).value().messages;
        fixture.append(agent::SessionCompactedPayload{2, "recovery summary"});
        const auto retained = agent::replay_session_events(fixture.sessions.events).value().messages;
        fixture.pending();
        const std::string task_id = "task-ffffffffffffffffffffffffffffffff";
        fixture.task_events = std::vector<agent::RuntimeEvent>{
            test::task_event(task_id, 1, agent::TaskStartedPayload{
                "question", "E:/workspace", {4, 8, 60'000, 30'000},
                wrong_history ? old_messages : retained, agent::SessionTaskLink{fixture.session_id, 5}}),
            test::task_event(task_id, 2, agent::ContextPreparationStartedPayload{}),
            test::task_event(task_id, 3, agent::ContextPreparedPayload{agent::EvidencePack{}})};
        const auto result = fixture.engine(settings()).recover_pending_turn(fixture.session_id, {});
        REQUIRE(result.error.has_value() == wrong_history);
        REQUIRE(fixture.resumes.size() == (wrong_history ? 0 : 1));
        REQUIRE(fixture.requests.empty());
        if (!wrong_history) {
            REQUIRE(fixture.resumes[0].fallback_system_prompt.find("recovery summary") != std::string::npos);
            REQUIRE(result.session->completed_turns == 5);
        }
    }
}

TEST_CASE(session_engine_invalid_context_settings_fail_before_turn_started) {
    using namespace context_engine_test;
    for (int invalid = 0; invalid < 6; ++invalid) {
        Fixture fixture;
        auto config = settings();
        if (invalid == 0) config.threshold_bytes = 0;
        if (invalid == 1) config.hard_limit_bytes = config.threshold_bytes;
        if (invalid == 2) config.retain_turns = 0;
        if (invalid == 3) config.max_summary_bytes = 8193;
        if (invalid == 4) config.memory_top_k = 21;
        if (invalid == 5) config.memory_max_injected_bytes = 0;
        const auto result = fixture.engine(config).submit_turn(fixture.session_id, "next", {});
        REQUIRE(result.error.has_value());
        REQUIRE(result.error->code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(fixture.sessions.events.size() == 1);
        REQUIRE(fixture.requests.empty());
    }
}

TEST_CASE(session_engine_reconstructed_recovery_prompt_checks_hard_limit_equality_and_overflow) {
    using namespace context_engine_test;
    for (const std::size_t hard_limit : {117U, 118U, 119U}) {
        Fixture fixture;
        fixture.seed_turns(4, 20);
        const auto retained = agent::replay_session_events(fixture.sessions.events).value().messages;
        fixture.pending();
        const auto before = fixture.sessions.events;
        const std::string task_id = "task-ffffffffffffffffffffffffffffffff";
        fixture.task_events = std::vector<agent::RuntimeEvent>{
            test::task_event(task_id, 1, agent::TaskStartedPayload{
                "question", "E:/workspace", {4, 8, 60'000, 30'000}, retained,
                agent::SessionTaskLink{fixture.session_id, 5}}),
            test::task_event(task_id, 2, agent::ContextPreparationStartedPayload{}),
            test::task_event(task_id, 3, agent::ContextPreparedPayload{agent::EvidencePack{}})};
        auto config = settings();
        config.threshold_bytes = 1;
        config.hard_limit_bytes = hard_limit;
        // 4 * (20 user + 6 answer) + 6 system + 8 pending user = 118 bytes.
        const auto result = fixture.engine(config).recover_pending_turn(fixture.session_id, {});
        const bool rejected = hard_limit <= 118;
        REQUIRE(result.error.has_value() == rejected);
        REQUIRE(fixture.resumes.size() == (rejected ? 0 : 1));
        REQUIRE(fixture.requests.empty());
        REQUIRE(fixture.compactor.inputs.empty());
        if (rejected) {
            REQUIRE(result.error->code == agent::ErrorCode::BudgetExceeded);
            REQUIRE(result.session->pending_turn.has_value());
            REQUIRE(fixture.sessions.events == before);
        }
    }
}

TEST_CASE(session_engine_persisted_model_request_recovery_ignores_new_context_limit) {
    using namespace context_engine_test;
    Fixture fixture;
    fixture.seed_turns(4, 20);
    const auto retained = agent::replay_session_events(fixture.sessions.events).value().messages;
    fixture.pending();
    const std::string task_id = "task-ffffffffffffffffffffffffffffffff";
    auto model_messages = retained;
    model_messages.push_back({agent::Role::User, {agent::TextBlock{"question"}}});
    fixture.task_events = std::vector<agent::RuntimeEvent>{
        test::task_event(task_id, 1, agent::TaskStartedPayload{
            "question", "E:/workspace", {4, 8, 60'000, 30'000}, retained,
            agent::SessionTaskLink{fixture.session_id, 5}}),
        test::task_event(task_id, 2, agent::ContextPreparationStartedPayload{}),
        test::task_event(task_id, 3, agent::ContextPreparedPayload{agent::EvidencePack{}}),
        test::task_event(task_id, 4, agent::ModelCallStartedPayload{
            {"persisted prompt", model_messages, {}, 30'000, agent::EvidencePack{}}})};
    auto config = settings();
    config.threshold_bytes = 1;
    config.hard_limit_bytes = 2;
    const auto result = fixture.engine(config).recover_pending_turn(fixture.session_id, {});
    REQUIRE(!result.error.has_value());
    REQUIRE(fixture.resumes.size() == 1);
    REQUIRE(fixture.resumes[0].fallback_system_prompt == "persisted prompt");
    REQUIRE(fixture.requests.empty());
}

TEST_CASE(session_engine_injected_memory_lines_fit_budget_including_separator) {
    using namespace context_engine_test;
    Fixture fixture;
    const auto first = fixture.memory.remember(fixture.session_id, "alpha");
    const auto second = fixture.memory.remember(fixture.session_id, "alpha");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto config = settings();
    // Two 56-byte memory lines would need a 113-byte data section with '\n'.
    config.memory_max_injected_bytes = 112;
    const auto result = fixture.engine(config, true).submit_turn(fixture.session_id, "alpha", {});
    REQUIRE(!result.error.has_value());
    REQUIRE(fixture.requests.size() == 1);
    const auto& prompt = fixture.requests[0].system_prompt;
    REQUIRE(prompt.find(first.value().memory_id) != std::string::npos);
    REQUIRE(prompt.find(second.value().memory_id) == std::string::npos);
}
