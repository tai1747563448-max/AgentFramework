#include "application/memory_engine.h"
#include "application/memory_policy.h"
#include "application/memory_retriever.h"
#include "application/session_reducer.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/memory_consolidator.h"
#include "test_support.h"

#include <iomanip>
#include <sstream>

namespace memory_engine_test {

const std::string session_id = "session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const std::string workspace = "E:/workspace";

std::string identity(const char* prefix, std::uint64_t number) {
    std::ostringstream stream;
    stream << prefix << std::hex << std::setw(32) << std::setfill('0') << number;
    return stream.str();
}

class Clock final : public agent::Clock {
public:
    std::string now_utc() const override { return utc; }
    std::int64_t monotonic_ms() const override { return 1000; }
    std::string utc{"2026-09-07T12:00:00.000Z"};
};

class Ids final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return invalid_seed ? "bad" : identity("task-", next++);
    }
    std::string next_correlation_id() override {
        return "corr-memory-" + std::to_string(correlation++);
    }
    bool invalid_seed{false};
    std::uint64_t next{1}, correlation{1};
};

// Keep the actual JSONL/reducer behavior; inject only the failing I/O boundary.
class FaultStore final : public agent::MemoryStore {
public:
    explicit FaultStore(const std::filesystem::path& root) : durable(root) {}
    agent::Result<void> append(const agent::MemoryEvent& event) override {
        if (event.sequence == fail_sequence)
            return agent::Result<void>::failure(
                {agent::ErrorCode::PersistenceFailure, "private store body", true});
        return durable.append(event);
    }
    agent::Result<std::vector<agent::MemoryEvent>> read_all() const override {
        return durable.read_all();
    }
    agent::Result<agent::MemoryState> read_state() const override {
        ++reads;
        if (fail_read)
            return agent::Result<agent::MemoryState>::failure(
                {agent::ErrorCode::PersistenceFailure, "private store body", true});
        return durable.read_state();
    }
    agent::JsonlMemoryStore durable;
    std::uint64_t fail_sequence{0};
    bool fail_read{false};
    mutable std::size_t reads{0};
};

class Consolidator final : public agent::MemoryConsolidator {
public:
    agent::Result<std::vector<agent::MemoryCandidate>> consolidate(
        const agent::MemoryConsolidationInput& input) override {
        inputs.push_back(input);
        std::size_t text_bytes = 0;
        for (const auto& turn : input.turns) {
            for (const auto& message : turn.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<agent::TextBlock>(&block))
                        text_bytes += text->text.size();
                }
            }
        }
        if (max_input_text_bytes != 0 && text_bytes > max_input_text_bytes)
            return agent::Result<std::vector<agent::MemoryCandidate>>::failure(
                {agent::ErrorCode::BudgetExceeded, "provider context limit", false});
        if (throws) throw std::runtime_error("private model body");
        if (fails)
            return agent::Result<std::vector<agent::MemoryCandidate>>::failure(
                {agent::ErrorCode::ProtocolFailure, "private model body", true});
        return agent::Result<std::vector<agent::MemoryCandidate>>::success(candidates);
    }
    bool fails{false}, throws{false};
    std::size_t max_input_text_bytes{0};
    std::vector<agent::MemoryCandidate> candidates;
    std::vector<agent::MemoryConsolidationInput> inputs;
};

struct Fixture {
    test::ScopedTempDir root{"agent-memory-engine"};
    agent::JsonlSessionStore sessions{root.path()};
    FaultStore memories{root.path()};
    Consolidator consolidator;
    agent::MemoryRetriever retriever;
    agent::MemoryPolicy policy{{1024, {"protected-fixture-value"}}};
    Clock clock;
    Ids ids;
    agent::MemoryEngine engine{memories, sessions, consolidator, retriever,
                               policy, clock, ids};

    Fixture() {
        REQUIRE(sessions.append({1, 1, session_id, clock.utc, "corr-start",
                                 agent::SessionStartedPayload{workspace, "model"}})
                    .has_value());
    }

    void append(agent::SessionEventPayload payload) {
        const auto events = sessions.read_session(session_id);
        REQUIRE(events.has_value());
        REQUIRE(sessions.append({1, events.value().size() + 1, session_id,
                                 clock.utc, ids.next_correlation_id(),
                                 std::move(payload)}).has_value());
    }

    void turn(std::uint64_t index, bool failed = false, bool tools = false) {
        const auto task_id = identity("task-", 100 + index);
        const auto question = "question " + std::to_string(index);
        append(agent::SessionTurnStartedPayload{index, task_id, question});
        if (failed) {
            append(agent::SessionTurnFailedPayload{
                index, task_id, agent::TaskStatus::Failed, "task failed"});
            return;
        }
        std::vector<agent::Message> messages{
            {agent::Role::User, {agent::TextBlock{question}}}};
        if (tools) {
            messages.push_back({agent::Role::Assistant,
                {agent::ToolUseBlock{{"call-1", "read_file",
                    agent::Value::object({{"path", "src/main.cpp"}})}}}});
            messages.push_back({agent::Role::User,
                {agent::ToolResultBlock{{"call-1", "exact tool evidence", false}}}});
        }
        messages.push_back({agent::Role::Assistant, {agent::TextBlock{"answer"}}});
        append(agent::SessionTurnCommittedPayload{index, task_id, messages});
    }

    void large_turn(std::uint64_t index, std::size_t user_bytes) {
        const auto task_id = identity("task-", 100 + index);
        std::string question(user_bytes, static_cast<char>('a' + index));
        append(agent::SessionTurnStartedPayload{index, task_id, question});
        append(agent::SessionTurnCommittedPayload{
            index, task_id,
            {{agent::Role::User, {agent::TextBlock{std::move(question)}}},
             {agent::Role::Assistant, {agent::TextBlock{"answer"}}}}});
    }
};

} // namespace memory_engine_test

TEST_CASE(memory_engine_remember_uses_persisted_session_scope_provenance_and_event_time) {
    using namespace memory_engine_test;
    Fixture fixture;
    const auto empty = fixture.engine.remember(session_id, "C++ project uses CMake");
    REQUIRE(empty.has_value());
    REQUIRE(empty.value().memory_id == "memory-00000000000000000000000000000001");
    REQUIRE(empty.value().category == agent::MemoryCategory::Fact);
    REQUIRE(empty.value().origin == agent::MemoryOrigin::ExplicitUser);
    REQUIRE(empty.value().scope_utf8 == workspace);
    REQUIRE(empty.value().source_session_id == session_id);
    REQUIRE(empty.value().source_turn_start == 1);
    REQUIRE(empty.value().source_turn_end == 1);
    fixture.turn(1);
    fixture.turn(2, true);
    fixture.clock.utc = "2026-09-07T12:00:01.000Z";
    const auto latest = fixture.engine.remember(session_id, "C++ tests run offline");
    REQUIRE(latest.has_value());
    REQUIRE(latest.value().source_turn_start == 2);
    REQUIRE(latest.value().source_turn_end == 2);
    const auto events = fixture.memories.read_all();
    REQUIRE(events.has_value());
    REQUIRE(events.value().size() == 2);
    REQUIRE(events.value()[1].sequence == 2);
    REQUIRE(!events.value()[1].correlation_id.empty());
    REQUIRE(latest.value().created_at_utc == "2026-09-07T12:00:01.000Z");
    REQUIRE(latest.value().updated_at_utc == events.value()[1].timestamp_utc);
    REQUIRE(fixture.memories.reads >= 2);
}

TEST_CASE(memory_engine_lists_exact_workspace_plus_global_and_forget_is_durable) {
    using namespace memory_engine_test;
    Fixture fixture;
    const auto local = fixture.engine.remember(session_id, "C++ local memory");
    REQUIRE(local.has_value());
    auto global = local.value();
    global.memory_id = identity("memory-", 91);
    global.scope_utf8.clear();
    REQUIRE(fixture.memories.append({1, 2, fixture.clock.utc, "corr-global",
                                     agent::MemoryUpsertedPayload{global}}).has_value());
    auto other = global;
    other.memory_id = identity("memory-", 92);
    other.scope_utf8 = "E:/workspace/subdirectory";
    REQUIRE(fixture.memories.append({1, 3, fixture.clock.utc, "corr-other",
                                     agent::MemoryUpsertedPayload{other}}).has_value());
    const auto listed = fixture.engine.list(workspace);
    REQUIRE(listed.has_value());
    REQUIRE(listed.value().size() == 2);
    const auto different_case = fixture.engine.list("E:/Workspace");
    REQUIRE(different_case.has_value());
    REQUIRE(different_case.value().size() == 1);
    REQUIRE(different_case.value()[0].memory_id == global.memory_id);
    const auto rendered = fixture.engine.retrieve(workspace, "C++", 1, 4096);
    REQUIRE(rendered.has_value());
    REQUIRE(rendered.value().size() == 1);
    REQUIRE(rendered.value()[0].find("(fact) C++ local memory") != std::string::npos);
    REQUIRE(fixture.engine.forget(local.value().memory_id).has_value());
    const auto state = fixture.memories.read_state();
    REQUIRE(state.has_value());
    REQUIRE(state.value().active_entries.count(local.value().memory_id) == 0);
    REQUIRE(state.value().used_memory_ids.count(local.value().memory_id) == 1);
    REQUIRE(fixture.engine.list(workspace).value().size() == 1);
    REQUIRE(!fixture.engine.forget(local.value().memory_id).has_value());
}

TEST_CASE(memory_engine_rejects_unsafe_remember_and_invalid_id_without_append) {
    using namespace memory_engine_test;
    Fixture fixture;
    for (const auto& text : {std::string{"password=private-fixture"},
                             std::string{"protected-fixture-value"}, std::string(1025, 'x')}) {
        const auto result = fixture.engine.remember(session_id, text);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().message.find(text) == std::string::npos);
    }
    fixture.ids.invalid_seed = true;
    REQUIRE(!fixture.engine.remember(session_id, "safe memory").has_value());
    REQUIRE(fixture.memories.read_all().value().empty());
    REQUIRE(!fixture.engine.remember("session-bad", "safe memory").has_value());
}

TEST_CASE(memory_engine_consolidates_original_events_after_compaction_and_covers_failed_turns) {
    using namespace memory_engine_test;
    Fixture fixture;
    fixture.turn(1, false, true);
    fixture.turn(2, true);
    fixture.turn(3);
    fixture.turn(4, true);
    fixture.turn(5);
    fixture.append(agent::SessionCompactedPayload{3, "short cumulative summary"});
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Workflow, workspace, "Build using CMake"},
        {agent::MemoryCategory::Workflow, workspace, "Build using CMake"},
        {agent::MemoryCategory::Fact, workspace, "password=private-fixture"},
        {agent::MemoryCategory::Fact, "E:/other", "unrelated scope"},
        {agent::MemoryCategory::Decision, workspace, "Use C++17"}};
    const auto result = fixture.engine.consolidate(session_id);
    REQUIRE(result.has_value());
    REQUIRE(result.value().appended_entries == 2);
    REQUIRE(result.value().through_turn == 5);
    REQUIRE(result.value().model_called);
    REQUIRE(fixture.consolidator.inputs.size() == 1);
    const auto& input = fixture.consolidator.inputs[0];
    REQUIRE(input.source_turn_start == 1);
    REQUIRE(input.source_turn_end == 5);
    REQUIRE(input.turns.size() == 3);
    REQUIRE(input.turns[0].turn_index == 1);
    REQUIRE(input.turns[1].turn_index == 3);
    REQUIRE(input.turns[2].turn_index == 5);
    REQUIRE(input.turns[0].messages.size() == 4);
    REQUIRE(std::get<agent::ToolResultBlock>(input.turns[0].messages[2].content[0])
                .result.content == "exact tool evidence");
    const auto events = fixture.memories.read_all();
    REQUIRE(events.value().size() == 3);
    REQUIRE(std::holds_alternative<agent::SessionMemoryConsolidatedPayload>(
        events.value().back().payload));
    const auto& entry = std::get<agent::MemoryUpsertedPayload>(events.value()[0].payload).entry;
    REQUIRE(entry.origin == agent::MemoryOrigin::ModelConsolidation);
    REQUIRE(entry.source_turn_start == 1);
    REQUIRE(entry.source_turn_end == 5);
    REQUIRE(entry.created_at_utc == events.value()[0].timestamp_utc);
    REQUIRE(entry.updated_at_utc == events.value()[0].timestamp_utc);
    REQUIRE(fixture.engine.consolidate(session_id).has_value());
    REQUIRE(fixture.consolidator.inputs.size() == 1);
    REQUIRE(fixture.memories.read_all().value().size() == 3);
    fixture.turn(6, true);
    fixture.turn(7);
    REQUIRE(fixture.engine.consolidate(session_id).has_value());
    REQUIRE(fixture.consolidator.inputs[1].source_turn_start == 6);
    REQUIRE(fixture.consolidator.inputs[1].source_turn_end == 7);
    REQUIRE(fixture.consolidator.inputs[1].turns.size() == 1);
    REQUIRE(fixture.consolidator.inputs[1].turns[0].turn_index == 7);
    REQUIRE(fixture.memories.read_state().value().session_checkpoints.at(session_id) == 7);
    REQUIRE(fixture.memories.read_state().value().active_entries.size() == 2);
}

TEST_CASE(memory_engine_retries_entry_or_checkpoint_failure_without_exact_duplicates) {
    using namespace memory_engine_test;
    for (const auto failed_sequence : {2U, 3U}) {
        Fixture fixture;
        fixture.turn(1);
        fixture.consolidator.candidates = {
            {agent::MemoryCategory::Fact, workspace, "First fact"},
            {agent::MemoryCategory::Fact, workspace, "Second fact"}};
        fixture.memories.fail_sequence = failed_sequence;
        const auto failed = fixture.engine.consolidate(session_id);
        REQUIRE(!failed.has_value());
        REQUIRE(failed.error().message.find("private store body") == std::string::npos);
        REQUIRE(fixture.memories.read_state().value().session_checkpoints.empty());
        REQUIRE(fixture.memories.read_state().value().active_entries.size() == failed_sequence - 1);
        fixture.memories.fail_sequence = 0;
        const auto retried = fixture.engine.consolidate(session_id);
        REQUIRE(retried.has_value());
        const auto state = fixture.memories.read_state();
        REQUIRE(state.value().active_entries.size() == 2);
        REQUIRE(state.value().last_sequence == 3);
        REQUIRE(state.value().session_checkpoints.at(session_id) == 1);
        REQUIRE(fixture.consolidator.inputs.size() == 2);
    }
}

TEST_CASE(memory_engine_checkpoints_empty_candidates_and_failed_only_turns_without_repeated_calls) {
    using namespace memory_engine_test;
    Fixture fixture;
    REQUIRE(fixture.engine.consolidate(session_id).has_value());
    REQUIRE(fixture.memories.read_all().value().empty());
    fixture.turn(1, true);
    fixture.turn(2, true);
    const auto failed_only = fixture.engine.consolidate(session_id);
    REQUIRE(failed_only.has_value());
    REQUIRE(!failed_only.value().model_called);
    REQUIRE(fixture.consolidator.inputs.empty());
    REQUIRE(fixture.memories.read_state().value().session_checkpoints.at(session_id) == 2);
    fixture.turn(3);
    const auto empty_candidates = fixture.engine.consolidate(session_id);
    REQUIRE(empty_candidates.has_value());
    REQUIRE(empty_candidates.value().appended_entries == 0);
    REQUIRE(fixture.consolidator.inputs.size() == 1);
    REQUIRE(fixture.memories.read_state().value().session_checkpoints.at(session_id) == 3);
}

TEST_CASE(memory_engine_batches_long_unconsolidated_history_under_a_fixed_source_budget) {
    using namespace memory_engine_test;
    Fixture fixture;
    fixture.consolidator.max_input_text_bytes = 32 * 1024;
    fixture.large_turn(1, 20 * 1024);
    fixture.large_turn(2, 20 * 1024);
    fixture.large_turn(3, 20 * 1024);

    const auto result = fixture.engine.consolidate(session_id);

    REQUIRE(result.has_value());
    REQUIRE(fixture.consolidator.inputs.size() == 3);
    REQUIRE(fixture.memories.read_state().value().session_checkpoints.at(session_id) == 3);
    for (const auto& input : fixture.consolidator.inputs) {
        std::size_t text_bytes = 0;
        for (const auto& turn : input.turns) {
            for (const auto& message : turn.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<agent::TextBlock>(&block))
                        text_bytes += text->text.size();
                }
            }
        }
        REQUIRE(text_bytes <= 32 * 1024);
    }
}

TEST_CASE(memory_engine_checkpoints_an_oversized_single_turn_without_retrying_the_model) {
    using namespace memory_engine_test;
    Fixture fixture;
    fixture.consolidator.max_input_text_bytes = 32 * 1024;
    fixture.large_turn(1, 40 * 1024);

    const auto first = fixture.engine.consolidate(session_id);
    const auto second = fixture.engine.consolidate(session_id);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(fixture.consolidator.inputs.empty());
    REQUIRE(fixture.memories.read_state().value().session_checkpoints.at(session_id) == 1);
}

TEST_CASE(memory_engine_sanitizes_model_and_store_failure_and_keeps_checkpoint_unchanged) {
    using namespace memory_engine_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.consolidator.fails = true;
    const auto failed = fixture.engine.consolidate(session_id);
    REQUIRE(!failed.has_value());
    REQUIRE(failed.error().message.find("private model body") == std::string::npos);
    REQUIRE(fixture.memories.read_all().value().empty());
    fixture.consolidator.throws = true;
    const auto thrown = fixture.engine.consolidate(session_id);
    REQUIRE(!thrown.has_value());
    REQUIRE(thrown.error().message.find("private model body") == std::string::npos);
    REQUIRE(fixture.memories.read_all().value().empty());
    fixture.memories.fail_read = true;
    const auto retrieval = fixture.engine.retrieve(workspace, "query", 5, 4096);
    REQUIRE(!retrieval.has_value());
    REQUIRE(retrieval.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(retrieval.error().message.find("private store body") == std::string::npos);
    REQUIRE(!fixture.engine.list(workspace).has_value());
    REQUIRE(!fixture.engine.remember(session_id, "safe memory").has_value());
    REQUIRE(!fixture.engine.forget(identity("memory-", 1)).has_value());
    REQUIRE(!fixture.engine.consolidate(session_id).has_value());
}
