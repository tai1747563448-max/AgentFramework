#include "application/task_evaluator.h"
#include "domain/runtime_event.h"
#include "test_support.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

constexpr const char* kTaskId =
    "task-0000000000000000000000000000000d";

agent::RuntimeEvent event(std::uint64_t sequence,
                          agent::EventPayload payload) {
    return {1, sequence, kTaskId, "2026-08-21T00:00:00.000Z",
            "corr-" + std::to_string(sequence), std::move(payload)};
}

agent::EvidencePack evidence(std::string prefix, std::size_t count) {
    agent::EvidencePack pack;
    for (std::size_t index = 0; index < count; ++index) {
        pack.items.push_back(
            {prefix + "-" + std::to_string(index), "reference",
             agent::Value::object(
                 {{"rank", agent::Value(
                               static_cast<std::int64_t>(index + 1))}})});
    }
    return pack;
}

agent::ModelRequest request(std::vector<agent::Message> messages,
                            agent::EvidencePack evidence_pack) {
    return {"coding prompt", std::move(messages), {}, 5'000,
            std::move(evidence_pack)};
}

std::vector<agent::RuntimeEvent> completed_tool_trace() {
    const auto first_evidence = evidence("first", 2);
    const auto second_evidence = evidence("second", 1);
    const agent::ToolCall call{
        "call-1", "run_tests",
        agent::Value::object({{"configuration", "Debug"}})};
    const agent::ToolResult tool_result{
        "call-1", "tests failed", true};
    const std::vector<agent::Message> first_messages{
        {agent::Role::User, {agent::TextBlock{"fix the failing test"}}}};
    const std::vector<agent::Message> second_messages{
        first_messages.front(),
        {agent::Role::Assistant, {agent::ToolUseBlock{call}}},
        {agent::Role::User, {agent::ToolResultBlock{tool_result}}}};
    return {
        event(1, agent::TaskStartedPayload{
                     "fix the failing test", "E:/workspace",
                     {4, 4, 30'000, 5'000}}),
        event(2, agent::ContextPreparationStartedPayload{}),
        event(3, agent::ContextPreparedPayload{first_evidence}),
        event(4, agent::ModelCallStartedPayload{
                     request(first_messages, first_evidence)}),
        event(5, agent::ModelCallSucceededPayload{
                     {{agent::ToolUseBlock{call}}, agent::StopReason::ToolUse,
                      10, 4, "provider-1"}}),
        event(6, agent::ToolCallStartedPayload{call}),
        event(7, agent::ToolCallSucceededPayload{tool_result}),
        event(8, agent::ContextPreparationStartedPayload{}),
        event(9, agent::ContextPreparedPayload{second_evidence}),
        event(10, agent::ModelCallStartedPayload{
                      request(second_messages, second_evidence)}),
        event(11, agent::ModelCallSucceededPayload{
                      {{agent::TextBlock{"fixed and verified"}},
                       agent::StopReason::EndTurn, 15, 5,
                       "provider-2"}}),
        event(12, agent::TaskCompletedPayload{"fixed and verified"}),
    };
}

}  // namespace fixtures

TEST_CASE(evaluator_reports_deterministic_runtime_metrics_for_completion) {
    const auto evaluated =
        agent::evaluate_task_events(fixtures::completed_tool_trace());

    REQUIRE(evaluated.has_value());
    REQUIRE(evaluated.value().task_id == fixtures::kTaskId);
    REQUIRE(evaluated.value().passed);
    REQUIRE(evaluated.value().status == agent::TaskStatus::Completed);
    REQUIRE(evaluated.value().model_rounds == 2);
    REQUIRE(evaluated.value().tool_calls == 1);
    REQUIRE(evaluated.value().evidence_rounds == 2);
    REQUIRE(evaluated.value().evidence_items == 3);
    REQUIRE(evaluated.value().model_requests_with_evidence == 2);
    REQUIRE(evaluated.value().tool_error_results == 1);
    REQUIRE(evaluated.value().last_sequence == 12);
}

TEST_CASE(evaluator_marks_every_valid_noncompleted_prefix_as_nonpassing) {
    const auto complete = fixtures::completed_tool_trace();
    const std::vector<std::size_t> prefix_lengths{1, 2, 3, 4, 5, 6, 7, 8,
                                                   9, 10, 11};
    for (const auto length : prefix_lengths) {
        const std::vector<agent::RuntimeEvent> prefix(
            complete.begin(), complete.begin() + length);
        const auto evaluated = agent::evaluate_task_events(prefix);
        REQUIRE(evaluated.has_value());
        REQUIRE(!evaluated.value().passed);
        REQUIRE(evaluated.value().last_sequence == length);
    }
}

TEST_CASE(evaluator_marks_failed_budget_and_cancelled_tasks_as_nonpassing) {
    const agent::RuntimeError failed_error{
        agent::ErrorCode::ProtocolFailure, "failed", false};
    const agent::RuntimeError budget_error{
        agent::ErrorCode::BudgetExceeded,
        "max_task_time_ms budget exceeded", false};
    const agent::RuntimeError cancelled_error{
        agent::ErrorCode::Cancelled, "cancelled", false};
    const std::vector<std::vector<agent::RuntimeEvent>> traces{
        {fixtures::event(1, agent::TaskStartedPayload{
                                "issue", "E:/workspace",
                                {2, 2, 30'000, 5'000}}),
         fixtures::event(2, agent::TaskFailedPayload{failed_error})},
        {fixtures::event(1, agent::TaskStartedPayload{
                                "issue", "E:/workspace",
                                {2, 2, 30'000, 5'000}}),
         fixtures::event(2, agent::ContextPreparationStartedPayload{}),
         fixtures::event(
             3, agent::TaskBudgetExceededPayload{
                    "max_task_time_ms", budget_error})},
        {fixtures::event(1, agent::TaskStartedPayload{
                                "issue", "E:/workspace",
                                {2, 2, 30'000, 5'000}}),
         fixtures::event(
             2, agent::TaskCancelledPayload{"operator", cancelled_error})},
    };

    for (const auto& trace : traces) {
        const auto evaluated = agent::evaluate_task_events(trace);
        REQUIRE(evaluated.has_value());
        REQUIRE(!evaluated.value().passed);
        REQUIRE(agent::is_terminal(evaluated.value().status));
    }
}

TEST_CASE(evaluator_rejects_an_invalid_or_empty_event_log) {
    const auto empty = agent::evaluate_task_events({});
    REQUIRE(!empty.has_value());
    REQUIRE(empty.error().code == agent::ErrorCode::InvalidInput);

    auto forged = fixtures::completed_tool_trace();
    forged.at(1).sequence = 99;
    const auto invalid = agent::evaluate_task_events(forged);
    REQUIRE(!invalid.has_value());
    REQUIRE(invalid.error().code == agent::ErrorCode::InvalidTransition);
}
