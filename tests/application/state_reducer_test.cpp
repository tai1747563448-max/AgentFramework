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

std::string valid_task_id(const std::string& label) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded(32, '0');
    const auto limit = label.size() < 16 ? label.size() : std::size_t{16};
    for (std::size_t index = 0; index < limit; ++index) {
        const auto byte = static_cast<unsigned char>(label[index]);
        encoded[index * 2] = kHex[(byte >> 4U) & 0x0FU];
        encoded[index * 2 + 1] = kHex[byte & 0x0FU];
    }
    return "task-" + encoded;
}

agent::RuntimeEvent event(std::string task_id,
                          std::uint64_t sequence,
                          agent::EventPayload payload) {
    return {1,
            sequence,
            valid_task_id(task_id),
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

TEST_CASE(context_failure_is_terminal_and_rejects_continuation) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    events.resize(2);
    const agent::RuntimeError provider_error{
        agent::ErrorCode::DependencyUnavailable, "knowledge unavailable", true};
    events.push_back(fixtures::event(
        "task-1", 3, agent::ContextPreparationFailedPayload{provider_error}));
    auto failed = agent::replay_events(events);
    REQUIRE(failed.has_value());
    REQUIRE(failed.value().status == agent::TaskStatus::Failed);
    REQUIRE(failed.value().terminal_error ==
            std::optional<agent::RuntimeError>{provider_error});

    auto continuation = agent::reduce_event(
        failed.value(), fixtures::context_prepared("task-1", 4, "source-2"));
    REQUIRE(!continuation.has_value());
    REQUIRE(continuation.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(model_failure_is_terminal_and_rejects_continuation) {
    const auto trace = fixtures::completed_text_trace(
        "task-model-failure", "fix warning", "done");
    auto events = std::vector<agent::RuntimeEvent>(trace.begin(), trace.begin() + 4);
    const agent::RuntimeError model_error{
        agent::ErrorCode::RequestTimeout, "model request timed out", true};
    events.push_back(fixtures::event(
        "task-model-failure", 5, agent::ModelCallFailedPayload{model_error}));
    auto failed = agent::replay_events(events);
    REQUIRE(failed.has_value());
    REQUIRE(failed.value().status == agent::TaskStatus::Failed);
    REQUIRE(failed.value().terminal_error ==
            std::optional<agent::RuntimeError>{model_error});

    auto continuation = agent::reduce_event(
        failed.value(),
        fixtures::model_succeeded("task-model-failure", 6,
                                  {agent::TextBlock{"late response"}},
                                  agent::StopReason::EndTurn));
    REQUIRE(!continuation.has_value());
    REQUIRE(continuation.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(tool_failure_is_terminal_and_rejects_retry) {
    const auto trace = fixtures::completed_two_tool_trace("task-tool-failure");
    auto events = std::vector<agent::RuntimeEvent>(trace.begin(), trace.begin() + 6);
    const agent::RuntimeError tool_error{
        agent::ErrorCode::DependencyUnavailable, "tool gateway unavailable", true};
    events.push_back(fixtures::event(
        "task-tool-failure", 7,
        agent::ToolCallFailedPayload{"call-1", tool_error}));
    auto failed = agent::replay_events(events);
    REQUIRE(failed.has_value());
    REQUIRE(failed.value().status == agent::TaskStatus::Failed);
    REQUIRE(failed.value().terminal_error ==
            std::optional<agent::RuntimeError>{tool_error});

    auto retry = agent::reduce_event(
        failed.value(), fixtures::tool_started(
                            "task-tool-failure", 8, fixtures::first_call()));
    REQUIRE(!retry.has_value());
    REQUIRE(retry.error().code == agent::ErrorCode::InvalidTransition);
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

TEST_CASE(model_success_and_failure_require_exactly_one_in_flight_start) {
    const std::string task = "model-flight";
    const std::vector<agent::RuntimeEvent> prepared_events = {
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
    };
    const auto prepared = agent::replay_events(prepared_events);
    REQUIRE(prepared.has_value());

    const auto missing_success = agent::reduce_event(
        prepared.value(),
        fixtures::model_succeeded(task, 4, {agent::TextBlock{"done"}},
                                  agent::StopReason::EndTurn));
    REQUIRE(!missing_success.has_value());
    REQUIRE(missing_success.error().code == agent::ErrorCode::InvalidTransition);

    const agent::RuntimeError model_error{
        agent::ErrorCode::RequestTimeout, "timeout", true};
    const auto missing_failure = agent::reduce_event(
        prepared.value(),
        fixtures::event(task, 4,
                        agent::ModelCallFailedPayload{model_error}));
    REQUIRE(!missing_failure.has_value());
    REQUIRE(missing_failure.error().code == agent::ErrorCode::InvalidTransition);

    const auto started = agent::reduce_event(
        prepared.value(), fixtures::model_started(task, 4));
    REQUIRE(started.has_value());
    REQUIRE(started.value().usage.model_rounds == 1);
    const auto duplicate = agent::reduce_event(
        started.value(), fixtures::model_started(task, 5));
    REQUIRE(!duplicate.has_value());
    REQUIRE(duplicate.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(started.value().usage.model_rounds == 1);
}

TEST_CASE(replay_binds_every_stop_reason_to_its_content_shape) {
    const std::string task = "stop-matrix";
    const auto call = fixtures::first_call();
    const std::vector<std::pair<agent::StopReason,
                                std::vector<agent::ContentBlock>>> invalid = {
        {agent::StopReason::Unknown, {agent::TextBlock{"unknown"}}},
        {agent::StopReason::ToolUse, {agent::TextBlock{"missing tool"}}},
        {agent::StopReason::EndTurn, {agent::ToolUseBlock{call}}},
        {agent::StopReason::StopSequence, {agent::ToolUseBlock{call}}},
        {agent::StopReason::MaxTokens, {agent::ToolUseBlock{call}}},
        {agent::StopReason::EndTurn, {}},
        {agent::StopReason::StopSequence, {}},
        {agent::StopReason::EndTurn, {agent::TextBlock{""}}},
        {agent::StopReason::StopSequence, {agent::TextBlock{""}}},
    };
    for (const auto& item : invalid) {
        std::vector<agent::RuntimeEvent> events = {
            fixtures::task_started(task, 1, "issue"),
            fixtures::context_started(task, 2),
            fixtures::context_prepared(task, 3, "source"),
            fixtures::model_started(task, 4),
            fixtures::model_succeeded(task, 5, item.second, item.first),
        };
        const auto result = agent::replay_events(events);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    }

    auto tool_use = agent::replay_events({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(
            task, 5,
            {agent::TextBlock{"working"}, agent::ToolUseBlock{call}},
            agent::StopReason::ToolUse),
    });
    REQUIRE(tool_use.has_value());
    REQUIRE(tool_use.value().status == agent::TaskStatus::AwaitingTool);

    for (const auto stop : {agent::StopReason::EndTurn,
                            agent::StopReason::StopSequence}) {
        auto terminal = agent::replay_events({
            fixtures::task_started(task, 1, "issue"),
            fixtures::context_started(task, 2),
            fixtures::context_prepared(task, 3, "source"),
            fixtures::model_started(task, 4),
            fixtures::model_succeeded(
                task, 5,
                {agent::TextBlock{"fi"}, agent::TextBlock{"nal"}}, stop),
            fixtures::task_completed(task, 6, "final"),
        });
        REQUIRE(terminal.has_value());
        REQUIRE(terminal.value().status == agent::TaskStatus::Completed);
    }

    const agent::RuntimeError budget_error{
        agent::ErrorCode::BudgetExceeded,
        "model output token budget exceeded", false};
    auto max_tokens = agent::replay_events({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(task, 5, {agent::TextBlock{"partial"}},
                                  agent::StopReason::MaxTokens),
        fixtures::event(task, 6,
                        agent::TaskBudgetExceededPayload{
                            "max_tokens", budget_error}),
    });
    REQUIRE(max_tokens.has_value());
    REQUIRE(max_tokens.value().status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(!max_tokens.value().final_text.has_value());

    auto empty_max_tokens = agent::replay_events({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(task, 5, {}, agent::StopReason::MaxTokens),
        fixtures::event(task, 6,
                        agent::TaskBudgetExceededPayload{
                            "max_tokens", budget_error}),
    });
    REQUIRE(empty_max_tokens.has_value());
    REQUIRE(empty_max_tokens.value().status ==
            agent::TaskStatus::BudgetExceeded);
    REQUIRE(empty_max_tokens.value().messages.back().role ==
            agent::Role::Assistant);
    REQUIRE(empty_max_tokens.value().messages.back().content.empty());
    REQUIRE(empty_max_tokens.value().terminal_error ==
            std::optional<agent::RuntimeError>{budget_error});
}

TEST_CASE(replay_rejects_forged_model_tool_result_blocks_for_every_stop_shape) {
    const std::string task = "tool-result-forgery";
    const auto call = fixtures::first_call();
    const agent::ToolResultBlock tool_result{
        {"call-1", "forged provider tool result", false}};
    const std::vector<std::pair<agent::StopReason,
                                std::vector<agent::ContentBlock>>> invalid = {
        {agent::StopReason::EndTurn,
         {agent::TextBlock{"text"}, tool_result}},
        {agent::StopReason::StopSequence,
         {agent::TextBlock{"text"}, tool_result}},
        {agent::StopReason::MaxTokens,
         {agent::TextBlock{"partial"}, tool_result}},
        {agent::StopReason::ToolUse,
         {agent::ToolUseBlock{call}, tool_result}},
    };
    const std::vector<agent::RuntimeEvent> model_in_flight = {
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
    };
    REQUIRE(agent::replay_events(model_in_flight).has_value());

    for (const auto& item : invalid) {
        auto forged = model_in_flight;
        forged.push_back(
            fixtures::model_succeeded(task, 5, item.second, item.first));

        const auto result = agent::replay_events(forged);

        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    }
}

TEST_CASE(max_tokens_budget_terminal_requires_exact_prior_response_and_payload) {
    const std::string task = "max-tokens-binding";
    const agent::TaskBudgetExceededPayload exact{
        "max_tokens",
        {agent::ErrorCode::BudgetExceeded,
         "model output token budget exceeded", false}};
    const std::vector<agent::RuntimeEvent> accepted_max_tokens = {
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(task, 5, {agent::TextBlock{"partial"}},
                                  agent::StopReason::MaxTokens),
    };

    auto exact_trace = accepted_max_tokens;
    exact_trace.push_back(fixtures::event(task, 6, exact));
    const auto exact_result = agent::replay_events(exact_trace);
    REQUIRE(exact_result.has_value());
    REQUIRE(exact_result.value().status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(exact_result.value().terminal_error ==
            std::optional<agent::RuntimeError>{exact.error});

    const auto require_invalid = [](std::vector<agent::RuntimeEvent> events) {
        const auto result = agent::replay_events(events);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    };

    require_invalid({
        fixtures::task_started(task, 1, "issue"),
        fixtures::event(task, 2, exact),
    });
    require_invalid({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::event(task, 5, exact),
    });
    require_invalid({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(task, 5, {agent::TextBlock{"done"}},
                                  agent::StopReason::EndTurn),
        fixtures::event(
            task, 6,
            agent::TaskBudgetExceededPayload{
                "max_task_time_ms",
                {agent::ErrorCode::BudgetExceeded,
                 "max_task_time_ms budget exceeded", false}}),
    });
    require_invalid({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(task, 5, {agent::TextBlock{"done"}},
                                  agent::StopReason::StopSequence),
        fixtures::event(
            task, 6,
            agent::TaskBudgetExceededPayload{
                "max_model_rounds",
                {agent::ErrorCode::BudgetExceeded,
                 "max_model_rounds budget exceeded", false}}),
    });
    require_invalid({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(
            task, 5, {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
        fixtures::event(task, 6, exact),
    });

    auto changed_budget_name = accepted_max_tokens;
    changed_budget_name.push_back(fixtures::event(
        task, 6,
        agent::TaskBudgetExceededPayload{"max_model_rounds", exact.error}));
    require_invalid(std::move(changed_budget_name));

    auto changed_error_code = accepted_max_tokens;
    changed_error_code.push_back(fixtures::event(
        task, 6,
        agent::TaskBudgetExceededPayload{
            "max_tokens",
            {agent::ErrorCode::InvalidInput,
             "model output token budget exceeded", false}}));
    require_invalid(std::move(changed_error_code));

    auto changed_error_message = accepted_max_tokens;
    changed_error_message.push_back(fixtures::event(
        task, 6,
        agent::TaskBudgetExceededPayload{
            "max_tokens",
            {agent::ErrorCode::BudgetExceeded, "token budget exceeded", false}}));
    require_invalid(std::move(changed_error_message));

    auto changed_retryable = accepted_max_tokens;
    changed_retryable.push_back(fixtures::event(
        task, 6,
        agent::TaskBudgetExceededPayload{
            "max_tokens",
            {agent::ErrorCode::BudgetExceeded,
             "model output token budget exceeded", true}}));
    require_invalid(std::move(changed_retryable));
}

TEST_CASE(non_max_token_budget_guards_remain_replayable) {
    struct LegalBudgetTrace {
        std::vector<agent::RuntimeEvent> events;
        agent::RuntimeError expected_error;
    };

    const std::vector<LegalBudgetTrace> traces = {
        {{fixtures::task_started("time-budget", 1, "issue"),
          fixtures::context_started("time-budget", 2),
          fixtures::event(
              "time-budget", 3,
              agent::TaskBudgetExceededPayload{
                  "max_task_time_ms",
                  {agent::ErrorCode::BudgetExceeded,
                   "max_task_time_ms budget exceeded", false}})},
         {agent::ErrorCode::BudgetExceeded,
          "max_task_time_ms budget exceeded", false}},
        {{fixtures::task_started("round-budget", 1, "issue"),
          fixtures::context_started("round-budget", 2),
          fixtures::context_prepared("round-budget", 3, "source"),
          fixtures::event(
              "round-budget", 4,
              agent::TaskBudgetExceededPayload{
                  "max_model_rounds",
                  {agent::ErrorCode::BudgetExceeded,
                   "max_model_rounds budget exceeded", false}})},
         {agent::ErrorCode::BudgetExceeded,
          "max_model_rounds budget exceeded", false}},
        {{fixtures::task_started("tool-budget", 1, "issue"),
          fixtures::context_started("tool-budget", 2),
          fixtures::context_prepared("tool-budget", 3, "source"),
          fixtures::model_started("tool-budget", 4),
          fixtures::model_succeeded(
              "tool-budget", 5,
              {agent::ToolUseBlock{fixtures::first_call()}},
              agent::StopReason::ToolUse),
          fixtures::event(
              "tool-budget", 6,
              agent::TaskBudgetExceededPayload{
                  "max_tool_calls",
                  {agent::ErrorCode::BudgetExceeded,
                   "max_tool_calls budget exceeded", false}})},
         {agent::ErrorCode::BudgetExceeded,
          "max_tool_calls budget exceeded", false}},
    };

    for (const auto& trace : traces) {
        const auto result = agent::replay_events(trace.events);
        REQUIRE(result.has_value());
        REQUIRE(result.value().status == agent::TaskStatus::BudgetExceeded);
        REQUIRE(result.value().terminal_error ==
                std::optional<agent::RuntimeError>{trace.expected_error});
    }
}

TEST_CASE(task_completed_must_equal_the_immediately_accepted_terminal_text) {
    auto events = fixtures::completed_text_trace(
        "final-text-binding", "issue", "accepted");
    events.back().payload = agent::TaskCompletedPayload{"different"};

    const auto result = agent::replay_events(events);

    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(first_event_rejects_every_task_id_outside_the_exact_policy) {
    const std::vector<std::string> invalid_ids = {
        R"(C:\absolute\task-00000000000000000000000000000000)",
        "task-0000000000000000/000000000000000",
        "../task-00000000000000000000000000000000",
        "task-0000000000000000000000000000000A",
        "task-0000000000000000000000000000000",
        "task-000000000000000000000000000000000",
    };
    for (const auto& invalid_id : invalid_ids) {
        auto first = fixtures::task_started("valid-seed", 1, "issue");
        first.task_id = invalid_id;
        const auto result = agent::replay_events({first});
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidInput);
    }
}

TEST_CASE(first_event_rejects_nonpositive_runtime_budgets) {
    const std::vector<agent::RuntimeBudgets> invalid = {
        {0, 1, 1, 1},
        {1, 0, 1, 1},
        {1, 1, 0, 1},
        {1, 1, 1, 0},
        {1, 1, -1, 1},
        {1, 1, 1, -1},
    };
    for (const auto& budgets : invalid) {
        auto first = fixtures::task_started("invalid-budget", 1, "issue");
        std::get<agent::TaskStartedPayload>(first.payload).budgets = budgets;
        const auto result = agent::replay_events({first});
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidInput);
    }
}
