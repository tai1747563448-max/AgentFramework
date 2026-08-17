#include "application/state_reducer.h"
#include "domain/runtime_event.h"
#include "test_support.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fixtures {

agent::RuntimeEvent event(std::string task_id,
                          std::uint64_t sequence,
                          agent::EventPayload payload) {
    return {1,
            sequence,
            std::move(task_id),
            "2026-08-17T12:00:00.000Z",
            "corr-" + std::to_string(sequence),
            std::move(payload)};
}

agent::RuntimeEvent task_started(const std::string& task_id,
                                 std::uint64_t sequence,
                                 const std::string& issue) {
    return event(task_id, sequence,
                 agent::TaskStartedPayload{issue, "E:/workspace", {8, 12, 90'000, 30'000}});
}

agent::RuntimeEvent context_started(const std::string& task_id,
                                    std::uint64_t sequence) {
    return event(task_id, sequence, agent::ContextPreparationStartedPayload{});
}

agent::EvidencePack evidence(const std::string& source_id) {
    return {{{source_id, "source contents",
              agent::Value::object({{"line", agent::Value(std::int64_t{7})}})}}};
}

agent::RuntimeEvent context_prepared(const std::string& task_id,
                                     std::uint64_t sequence,
                                     const std::string& source_id) {
    return event(task_id, sequence, agent::ContextPreparedPayload{evidence(source_id)});
}

agent::RuntimeEvent model_started(const std::string& task_id,
                                  std::uint64_t sequence) {
    return event(task_id, sequence,
                 agent::ModelCallStartedPayload{
                     {"runtime prompt", {}, {}, 30'000}});
}

agent::RuntimeEvent model_succeeded(const std::string& task_id,
                                    std::uint64_t sequence,
                                    std::vector<agent::ContentBlock> content,
                                    agent::StopReason stop_reason) {
    return event(task_id, sequence,
                 agent::ModelCallSucceededPayload{
                     {std::move(content), stop_reason, "stop", 11, 5, "request-1"}});
}

agent::ToolCall first_call() {
    return {"call-1", "read_file",
            agent::Value::object({{"path", "src/main.cpp"}})};
}

agent::ToolCall second_call() {
    return {"call-2", "compile",
            agent::Value::object({{"target", "agent"}})};
}

agent::ToolResult first_result() {
    return {"call-1", "file contents", false};
}

agent::ToolResult second_result() {
    return {"call-2", "build passed", false};
}

agent::RuntimeEvent tool_started(const std::string& task_id,
                                 std::uint64_t sequence,
                                 agent::ToolCall call) {
    return event(task_id, sequence, agent::ToolCallStartedPayload{std::move(call)});
}

agent::RuntimeEvent tool_succeeded(const std::string& task_id,
                                   std::uint64_t sequence,
                                   agent::ToolResult result) {
    return event(task_id, sequence, agent::ToolCallSucceededPayload{std::move(result)});
}

agent::RuntimeEvent task_completed(const std::string& task_id,
                                   std::uint64_t sequence,
                                   const std::string& final_text) {
    return event(task_id, sequence, agent::TaskCompletedPayload{final_text});
}

std::vector<agent::RuntimeEvent> completed_text_trace(const std::string& task_id,
                                                      const std::string& issue,
                                                      const std::string& final_text) {
    return {
        task_started(task_id, 1, issue),
        context_started(task_id, 2),
        context_prepared(task_id, 3, "source-1"),
        model_started(task_id, 4),
        model_succeeded(task_id, 5, {agent::TextBlock{final_text}},
                        agent::StopReason::EndTurn),
        task_completed(task_id, 6, final_text),
    };
}

std::vector<agent::RuntimeEvent> completed_two_tool_trace(const std::string& task_id) {
    const auto call_1 = first_call();
    const auto call_2 = second_call();
    return {
        task_started(task_id, 1, "inspect and build"),
        context_started(task_id, 2),
        context_prepared(task_id, 3, "source-1"),
        model_started(task_id, 4),
        model_succeeded(task_id, 5,
                        {agent::TextBlock{"working"}, agent::ToolUseBlock{call_1},
                         agent::ToolUseBlock{call_2}},
                        agent::StopReason::ToolUse),
        tool_started(task_id, 6, call_1),
        tool_succeeded(task_id, 7, first_result()),
        tool_started(task_id, 8, call_2),
        tool_succeeded(task_id, 9, second_result()),
        context_started(task_id, 10),
        context_prepared(task_id, 11, "source-2"),
        model_started(task_id, 12),
        model_succeeded(task_id, 13, {agent::TextBlock{"done"}},
                        agent::StopReason::EndTurn),
        task_completed(task_id, 14, "done"),
    };
}

agent::TaskState replay_prefix(const std::vector<agent::RuntimeEvent>& events,
                               std::size_t count) {
    const std::vector<agent::RuntimeEvent> prefix(events.begin(), events.begin() + count);
    return agent::replay_events(prefix).value();
}

}  // namespace fixtures

TEST_CASE(replay_text_completion_is_deterministic) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    auto first = agent::replay_events(events);
    auto second = agent::replay_events(events);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value() == second.value());
    REQUIRE(first.value().status == agent::TaskStatus::Completed);
    REQUIRE(first.value().final_text == std::optional<std::string>{"done"});
    REQUIRE(first.value().last_sequence == 6);
    REQUIRE(first.value().usage.model_rounds == 1);
    const agent::Message expected_initial_message{
        agent::Role::User, {agent::TextBlock{"fix warning"}}};
    REQUIRE(first.value().messages.front() == expected_initial_message);
}

TEST_CASE(replay_preserves_two_tool_call_and_result_order) {
    auto state = agent::replay_events(fixtures::completed_two_tool_trace("task-tools"));
    REQUIRE(state.has_value());
    REQUIRE(state.value().status == agent::TaskStatus::Completed);
    REQUIRE(state.value().usage.model_rounds == 2);
    REQUIRE(state.value().usage.tool_calls == 2);
    REQUIRE(state.value().messages.size() == 4);
    const auto& assistant = state.value().messages.at(1);
    REQUIRE(assistant.role == agent::Role::Assistant);
    REQUIRE(std::get<agent::ToolUseBlock>(assistant.content.at(1)).call ==
            fixtures::first_call());
    REQUIRE(std::get<agent::ToolUseBlock>(assistant.content.at(2)).call ==
            fixtures::second_call());
    const auto& results = state.value().messages.at(2);
    REQUIRE(results.role == agent::Role::User);
    REQUIRE(results.content.size() == 2);
    REQUIRE(std::get<agent::ToolResultBlock>(results.content.at(0)).result ==
            fixtures::first_result());
    REQUIRE(std::get<agent::ToolResultBlock>(results.content.at(1)).result ==
            fixtures::second_result());
    REQUIRE(state.value().pending_tool_calls.empty());
    REQUIRE(state.value().pending_tool_results.empty());
    REQUIRE(state.value().next_tool_index == 0);
    REQUIRE(!state.value().active_tool_call_id.has_value());
    REQUIRE(state.value().evidence == fixtures::evidence("source-2"));
}

TEST_CASE(replay_rejects_noncontiguous_sequence_without_repair) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    events.at(3).sequence = 9;
    auto result = agent::replay_events(events);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(replay_rejects_task_id_change) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    events.at(3).task_id = "task-2";
    auto result = agent::replay_events(events);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(replay_stops_at_first_schema_error) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    events.at(2).schema_version = 2;
    events.at(3).sequence = 99;
    auto result = agent::replay_events(events);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(reducer_rejects_context_start_until_all_tools_are_processed) {
    const auto events = fixtures::completed_two_tool_trace("task-tools");
    const auto state = fixtures::replay_prefix(events, 7);
    auto result = agent::reduce_event(
        state, fixtures::context_started("task-tools", state.last_sequence + 1));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.last_sequence == 7);
    REQUIRE(state.next_tool_index == 1);
    REQUIRE(state.pending_tool_results.size() == 1);
}

TEST_CASE(reducer_preserves_active_tool_identity_on_mismatched_result) {
    const auto events = fixtures::completed_two_tool_trace("task-tools");
    const auto state = fixtures::replay_prefix(events, 6);
    auto result = agent::reduce_event(
        state, fixtures::tool_succeeded("task-tools", 7, fixtures::second_result()));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.last_sequence == 6);
    REQUIRE(state.active_tool_call_id == std::optional<std::string>{"call-1"});
    REQUIRE(state.pending_tool_results.empty());
}

TEST_CASE(reducer_rejects_event_after_terminal_state) {
    auto state = agent::replay_events(
        fixtures::completed_text_trace("task-1", "fix warning", "done"));
    auto event = fixtures::context_started("task-1", state.value().last_sequence + 1);
    auto result = agent::reduce_event(state.value(), event);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(failure_boundary_retains_state_until_task_failed) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    events.resize(2);
    const agent::RuntimeError provider_error{
        agent::ErrorCode::DependencyUnavailable, "knowledge unavailable", true};
    events.push_back(fixtures::event(
        "task-1", 3, agent::ContextPreparationFailedPayload{provider_error}));
    auto retained = agent::replay_events(events);
    REQUIRE(retained.has_value());
    REQUIRE(retained.value().status == agent::TaskStatus::PreparingContext);
    REQUIRE(!retained.value().terminal_error.has_value());
    events.push_back(fixtures::event(
        "task-1", 4, agent::TaskFailedPayload{provider_error}));
    auto failed = agent::replay_events(events);
    REQUIRE(failed.has_value());
    REQUIRE(failed.value().status == agent::TaskStatus::Failed);
    REQUIRE(failed.value().terminal_error ==
            std::optional<agent::RuntimeError>{provider_error});
}

TEST_CASE(first_event_must_be_version_one_task_started_at_sequence_one) {
    auto empty = agent::replay_events({});
    REQUIRE(!empty.has_value());
    REQUIRE(empty.error().code == agent::ErrorCode::InvalidInput);

    auto wrong_kind = agent::replay_events({fixtures::context_started("task-1", 1)});
    REQUIRE(!wrong_kind.has_value());
    REQUIRE(wrong_kind.error().code == agent::ErrorCode::InvalidTransition);

    auto wrong_sequence = fixtures::task_started("task-1", 2, "issue");
    auto sequence_result = agent::replay_events({wrong_sequence});
    REQUIRE(!sequence_result.has_value());
    REQUIRE(sequence_result.error().code == agent::ErrorCode::InvalidTransition);

    auto wrong_schema = fixtures::task_started("task-1", 1, "issue");
    wrong_schema.schema_version = 2;
    auto schema_result = agent::replay_events({wrong_schema});
    REQUIRE(!schema_result.has_value());
    REQUIRE(schema_result.error().code == agent::ErrorCode::InvalidInput);
}
