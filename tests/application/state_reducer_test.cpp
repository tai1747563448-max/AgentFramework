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
                                 const std::string& issue,
                                 agent::RuntimeBudgets budgets =
                                     {8, 12, 90'000, 30'000}) {
    return event(task_id, sequence,
                 agent::TaskStartedPayload{issue, "E:/workspace", budgets});
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
                                  std::uint64_t sequence,
                                  const std::string& source_id = "source",
                                  std::vector<agent::Message> messages = {
                                      {agent::Role::User,
                                       {agent::TextBlock{"issue"}}}},
                                  std::string system_prompt =
                                      "runtime prompt") {
    agent::ModelRequest request{std::move(system_prompt),
                                std::move(messages), {}, 30'000,
                                evidence(source_id)};
    return event(task_id, sequence,
                 agent::ModelCallStartedPayload{std::move(request)});
}

agent::RuntimeEvent model_succeeded(const std::string& task_id,
                                    std::uint64_t sequence,
                                    std::vector<agent::ContentBlock> content,
                                    agent::StopReason stop_reason) {
    // T12 (v2 §3): ModelResponse no longer carries raw_stop_reason;
    // the canonical StopReason is the only stop signal the reducer
    // inspects. Adapter-specific string mapping lives in
    // ports/stop_reason_codec.h.
    return event(task_id, sequence,
                 agent::ModelCallSucceededPayload{
                     {std::move(content), stop_reason, 11, 5, "request-1"}});
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
        model_started(task_id, 4, "source-1",
                      {{agent::Role::User,
                        {agent::TextBlock{issue}}}}),
        model_succeeded(task_id, 5, {agent::TextBlock{final_text}},
                        agent::StopReason::EndTurn),
        task_completed(task_id, 6, final_text),
    };
}

std::vector<agent::RuntimeEvent> completed_two_tool_trace(const std::string& task_id) {
    const auto call_1 = first_call();
    const auto call_2 = second_call();
    // T01 batched dispatch: the runtime emits both Started events
    // before any Succeeded arrives. Match that shape so the replay
    // reducer accepts the trace.
    return {
        task_started(task_id, 1, "inspect and build"),
        context_started(task_id, 2),
        context_prepared(task_id, 3, "source-1"),
        model_started(task_id, 4, "source-1",
                      {{agent::Role::User,
                        {agent::TextBlock{"inspect and build"}}}}),
        model_succeeded(task_id, 5,
                        {agent::TextBlock{"working"}, agent::ToolUseBlock{call_1},
                         agent::ToolUseBlock{call_2}},
                        agent::StopReason::ToolUse),
        tool_started(task_id, 6, call_1),
        tool_started(task_id, 7, call_2),
        tool_succeeded(task_id, 8, first_result()),
        tool_succeeded(task_id, 9, second_result()),
        context_started(task_id, 10),
        context_prepared(task_id, 11, "source-2"),
        model_started(
            task_id, 12, "source-2",
            {{agent::Role::User,
              {agent::TextBlock{"inspect and build"}}},
             {agent::Role::Assistant,
              {agent::TextBlock{"working"}, agent::ToolUseBlock{call_1},
               agent::ToolUseBlock{call_2}}},
             {agent::Role::User,
              {agent::ToolResultBlock{first_result()},
               agent::ToolResultBlock{second_result()}}}}),
        model_succeeded(task_id, 13, {agent::TextBlock{"done"}},
                        agent::StopReason::EndTurn),
        task_completed(task_id, 14, "done"),
    };
}

TEST_CASE(task_start_loads_valid_session_history_before_current_user_message) {
    const auto call = fixtures::first_call();
    const auto result = fixtures::first_result();
    const std::vector<agent::Message> history{
        {agent::Role::User, {agent::TextBlock{"first"}}},
        {agent::Role::Assistant, {agent::ToolUseBlock{call}}},
        {agent::Role::User, {agent::ToolResultBlock{result}}},
        {agent::Role::Assistant, {agent::TextBlock{"answer"}}},
    };
    auto started = fixtures::task_started("session-history", 1, "second");
    auto* payload = std::get_if<agent::TaskStartedPayload>(&started.payload);
    REQUIRE(payload != nullptr);
    payload->initial_messages = history;
    payload->session_link = agent::SessionTaskLink{
        "session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 2};

    const auto reduced = agent::reduce_event(std::nullopt, started);

    REQUIRE(reduced.has_value());
    auto expected = history;
    expected.push_back(
        {agent::Role::User, {agent::TextBlock{"second"}}});
    REQUIRE(reduced.value().messages == expected);
    REQUIRE(reduced.value().session_link == payload->session_link);
}

TEST_CASE(task_start_rejects_unbalanced_session_history) {
    auto started = fixtures::task_started("bad-session-history", 1, "next");
    auto* payload = std::get_if<agent::TaskStartedPayload>(&started.payload);
    REQUIRE(payload != nullptr);
    payload->initial_messages = {
        {agent::Role::Assistant,
         {agent::ToolUseBlock{fixtures::first_call()}}},
    };

    const auto reduced = agent::reduce_event(std::nullopt, started);

    REQUIRE(!reduced.has_value());
    REQUIRE(reduced.error().code == agent::ErrorCode::InvalidInput);
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
    // Replay up to and including the first Succeeded (sequence 8): at
    // this point one tool result is recorded and one is still in the
    // dispatch window, so the reducer must refuse any context-start
    // event that would skip the remaining work.
    const auto state = fixtures::replay_prefix(events, 8);
    auto result = agent::reduce_event(
        state, fixtures::context_started("task-tools", state.last_sequence + 1));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.last_sequence == 8);
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

TEST_CASE(reducer_rejects_invalid_context_evidence_before_state_mutation) {
    auto events = fixtures::completed_text_trace(
        "invalid-evidence", "fix warning", "done");
    events.resize(2);
    auto state = agent::replay_events(events);
    REQUIRE(state.has_value());
    auto invalid = fixtures::evidence("duplicate-source");
    invalid.items.push_back(invalid.items.front());

    const auto reduced = agent::reduce_event(
        state.value(),
        fixtures::event(
            "invalid-evidence", 3,
            agent::ContextPreparedPayload{std::move(invalid)}));

    REQUIRE(!reduced.has_value());
    REQUIRE(reduced.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.value().status == agent::TaskStatus::PreparingContext);
    REQUIRE(state.value().evidence.items.empty());
    REQUIRE(state.value().last_sequence == 2);
}

TEST_CASE(reducer_binds_model_request_evidence_to_prepared_context) {
    const auto task = std::string("evidence-binding");
    const std::vector<agent::RuntimeEvent> prefix{
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "prepared-source")};
    auto state = agent::replay_events(prefix);
    REQUIRE(state.has_value());

    const auto mismatch = agent::reduce_event(
        state.value(), fixtures::model_started(task, 4, "forged-source"));

    REQUIRE(!mismatch.has_value());
    REQUIRE(mismatch.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.value().status == agent::TaskStatus::AwaitingModel);
    REQUIRE(!state.value().model_call_in_flight);
    REQUIRE(state.value().usage.model_rounds == 0);
    REQUIRE(state.value().last_sequence == 3);

    auto invalid_request = fixtures::model_started(task, 4, "prepared-source");
    auto& request = std::get<agent::ModelCallStartedPayload>(
                        invalid_request.payload)
                        .request;
    request.evidence.items.push_back(request.evidence.items.front());
    const auto invalid = agent::reduce_event(state.value(), invalid_request);

    REQUIRE(!invalid.has_value());
    REQUIRE(invalid.error().code == agent::ErrorCode::InvalidTransition);
    REQUIRE(state.value().last_sequence == 3);
}

TEST_CASE(reducer_binds_model_request_messages_timeout_and_system_prompt) {
    const auto task = std::string("request-binding");
    const std::vector<agent::RuntimeEvent> prefix{
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source-1")};
    const auto state = agent::replay_events(prefix);
    REQUIRE(state.has_value());

    auto wrong_messages = fixtures::model_started(task, 4, "source-1");
    std::get<agent::ModelCallStartedPayload>(wrong_messages.payload)
        .request.messages.front().content.front() =
        agent::TextBlock{"forged issue"};
    const auto messages_result =
        agent::reduce_event(state.value(), wrong_messages);
    REQUIRE(!messages_result.has_value());
    REQUIRE(messages_result.error().code ==
            agent::ErrorCode::InvalidTransition);

    auto wrong_timeout = fixtures::model_started(task, 4, "source-1");
    std::get<agent::ModelCallStartedPayload>(wrong_timeout.payload)
        .request.timeout_ms = 29'999;
    const auto timeout_result =
        agent::reduce_event(state.value(), wrong_timeout);
    REQUIRE(!timeout_result.has_value());
    REQUIRE(timeout_result.error().code ==
            agent::ErrorCode::InvalidTransition);

    const auto call = fixtures::first_call();
    auto continued = prefix;
    continued.push_back(fixtures::model_started(task, 4, "source-1"));
    const auto in_flight = agent::replay_events(continued);
    REQUIRE(in_flight.has_value());
    REQUIRE(in_flight.value().last_model_request.has_value());
    REQUIRE(*in_flight.value().last_model_request ==
            std::get<agent::ModelCallStartedPayload>(
                continued.back().payload)
                .request);
    continued.push_back(fixtures::model_succeeded(
        task, 5, {agent::ToolUseBlock{call}}, agent::StopReason::ToolUse));
    continued.push_back(fixtures::tool_started(task, 6, call));
    continued.push_back(
        fixtures::tool_succeeded(task, 7, fixtures::first_result()));
    continued.push_back(fixtures::context_started(task, 8));
    continued.push_back(fixtures::context_prepared(task, 9, "source-2"));
    const auto later_state = agent::replay_events(continued);
    REQUIRE(later_state.has_value());

    const auto changed_prompt = fixtures::model_started(
        task, 10, "source-2", later_state.value().messages,
        "forged replacement prompt");
    const auto prompt_result =
        agent::reduce_event(later_state.value(), changed_prompt);
    REQUIRE(!prompt_result.has_value());
    REQUIRE(prompt_result.error().code ==
            agent::ErrorCode::InvalidTransition);
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
        {agent::StopReason::ToolUse,
         {agent::TextBlock{""}, agent::ToolUseBlock{call}}},
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

TEST_CASE(replay_rejects_unknown_canonical_stop_reason) {
    // T12 (v2 §3): the runtime/reducer can only reject forged
    // responses when they carry an unknown canonical StopReason. The
    // raw provider string is an adapter concern and lives in
    // adapters/anthropic/anthropic_messages_client_test.cpp. We keep
    // the cross-content / stop_reason matrix rejection here, but
    // drop the (stop_reason, raw_stop_reason) mismatch matrix because
    // the raw field is no longer on ModelResponse.
    const std::string task = "stop-pair-forgery";
    const auto prefix = std::vector<agent::RuntimeEvent>{
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
    };
    const auto started = agent::replay_events(prefix);
    REQUIRE(started.has_value());

    const auto forged = fixtures::model_succeeded(
        task, 5, {}, agent::StopReason::Unknown);
    const auto reduced = agent::reduce_event(started.value(), forged);
    REQUIRE(!reduced.has_value());
    REQUIRE(reduced.error().code == agent::ErrorCode::InvalidTransition);

    auto trace = prefix;
    trace.push_back(forged);
    const auto replayed = agent::replay_events(trace);
    REQUIRE(!replayed.has_value());
    REQUIRE(replayed.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(replay_rejects_invalid_or_duplicate_response_tool_calls) {
    const std::string task = "tool-call-validation";
    const auto object_arguments =
        agent::Value::object({{"path", agent::Value("src/main.cpp")}});
    const std::vector<std::vector<agent::ContentBlock>> invalid = {
        {agent::ToolUseBlock{{"", "read_file", object_arguments}}},
        {agent::ToolUseBlock{{"call-1", "", object_arguments}}},
        {agent::ToolUseBlock{
            {"call-1", "read_file", agent::Value(std::int64_t{7})}}},
        {agent::ToolUseBlock{{"call-1", "read_file", object_arguments}},
         agent::ToolUseBlock{{"call-1", "compile", object_arguments}}},
    };

    for (const auto& content : invalid) {
        const auto result = agent::replay_events({
            fixtures::task_started(task, 1, "issue"),
            fixtures::context_started(task, 2),
            fixtures::context_prepared(task, 3, "source"),
            fixtures::model_started(task, 4),
            fixtures::model_succeeded(task, 5, content,
                                      agent::StopReason::ToolUse),
        });

        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    }
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

    auto generic_after_max_tokens = accepted_max_tokens;
    generic_after_max_tokens.push_back(fixtures::event(
        task, 6,
        agent::TaskBudgetExceededPayload{
            "max_task_time_ms",
            {agent::ErrorCode::BudgetExceeded,
             "max_task_time_ms budget exceeded", false}}));
    require_invalid(std::move(generic_after_max_tokens));

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

TEST_CASE(generic_budget_terminals_accept_reconstructibly_legal_traces) {
    const agent::RuntimeError time_error{
        agent::ErrorCode::BudgetExceeded,
        "max_task_time_ms budget exceeded", false};
    const agent::RuntimeError model_error{
        agent::ErrorCode::BudgetExceeded,
        "max_model_rounds budget exceeded", false};
    const agent::RuntimeError tool_error{
        agent::ErrorCode::BudgetExceeded,
        "max_tool_calls budget exceeded", false};

    struct TimePrefix {
        const char* task;
        std::vector<agent::RuntimeEvent> events;
    };
    const std::vector<TimePrefix> time_prefixes = {
        {"time-preparing",
         {fixtures::task_started("time-preparing", 1, "issue"),
          fixtures::context_started("time-preparing", 2)}},
        {"time-model",
         {fixtures::task_started("time-model", 1, "issue"),
          fixtures::context_started("time-model", 2),
          fixtures::context_prepared("time-model", 3, "source")}},
        {"time-tool",
         {fixtures::task_started("time-tool", 1, "issue"),
          fixtures::context_started("time-tool", 2),
          fixtures::context_prepared("time-tool", 3, "source"),
          fixtures::model_started("time-tool", 4),
          fixtures::model_succeeded(
              "time-tool", 5,
              {agent::ToolUseBlock{fixtures::first_call()}},
              agent::StopReason::ToolUse)}},
    };
    for (auto prefix : time_prefixes) {
        const auto state = agent::replay_events(prefix.events);
        REQUIRE(state.has_value());
        REQUIRE(!state.value().model_call_in_flight);
        REQUIRE(!state.value().active_tool_call_id.has_value());
        prefix.events.push_back(fixtures::event(
            prefix.task, prefix.events.back().sequence + 1,
            agent::TaskBudgetExceededPayload{"max_task_time_ms", time_error}));
        const auto result = agent::replay_events(prefix.events);
        REQUIRE(result.has_value());
        REQUIRE(result.value().status == agent::TaskStatus::BudgetExceeded);
        REQUIRE(result.value().terminal_error ==
                std::optional<agent::RuntimeError>{time_error});
    }

    const std::string model_task = "round-budget";
    std::vector<agent::RuntimeEvent> model_trace = {
        fixtures::task_started(model_task, 1, "issue",
                               {1, 12, 90'000, 30'000}),
        fixtures::context_started(model_task, 2),
        fixtures::context_prepared(model_task, 3, "source-1"),
        fixtures::model_started(model_task, 4, "source-1"),
        fixtures::model_succeeded(
            model_task, 5,
            {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
        fixtures::tool_started(model_task, 6, fixtures::first_call()),
        fixtures::tool_succeeded(model_task, 7, fixtures::first_result()),
        fixtures::context_started(model_task, 8),
        fixtures::context_prepared(model_task, 9, "source-2"),
    };
    const auto model_state = agent::replay_events(model_trace);
    REQUIRE(model_state.has_value());
    REQUIRE(model_state.value().status == agent::TaskStatus::AwaitingModel);
    REQUIRE(!model_state.value().model_call_in_flight);
    REQUIRE(!model_state.value().accepted_model_stop_reason.has_value());
    REQUIRE(model_state.value().usage.model_rounds == 1);
    REQUIRE(model_state.value().usage.model_rounds ==
            model_state.value().budgets.max_model_rounds);
    model_trace.push_back(fixtures::event(
        model_task, 10,
        agent::TaskBudgetExceededPayload{"max_model_rounds", model_error}));
    const auto model_result = agent::replay_events(model_trace);
    REQUIRE(model_result.has_value());
    REQUIRE(model_result.value().status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(model_result.value().terminal_error ==
            std::optional<agent::RuntimeError>{model_error});

    const std::string tool_task = "tool-budget";
    std::vector<agent::RuntimeEvent> tool_trace = {
        fixtures::task_started(tool_task, 1, "issue",
                               {8, 1, 90'000, 30'000}),
        fixtures::context_started(tool_task, 2),
        fixtures::context_prepared(tool_task, 3, "source"),
        fixtures::model_started(tool_task, 4),
        fixtures::model_succeeded(
            tool_task, 5,
            {agent::ToolUseBlock{fixtures::first_call()},
             agent::ToolUseBlock{fixtures::second_call()}},
            agent::StopReason::ToolUse),
        fixtures::tool_started(tool_task, 6, fixtures::first_call()),
        fixtures::tool_succeeded(tool_task, 7, fixtures::first_result()),
    };
    const auto tool_state = agent::replay_events(tool_trace);
    REQUIRE(tool_state.has_value());
    REQUIRE(tool_state.value().status == agent::TaskStatus::AwaitingTool);
    REQUIRE(tool_state.value().accepted_model_stop_reason ==
            agent::StopReason::ToolUse);
    REQUIRE(!tool_state.value().active_tool_call_id.has_value());
    REQUIRE(tool_state.value().next_tool_index == 1);
    REQUIRE(tool_state.value().next_tool_index <
            tool_state.value().pending_tool_calls.size());
    REQUIRE(tool_state.value().usage.tool_calls == 1);
    REQUIRE(tool_state.value().usage.tool_calls ==
            tool_state.value().budgets.max_tool_calls);
    tool_trace.push_back(fixtures::event(
        tool_task, 8,
        agent::TaskBudgetExceededPayload{"max_tool_calls", tool_error}));
    const auto tool_result = agent::replay_events(tool_trace);
    REQUIRE(tool_result.has_value());
    REQUIRE(tool_result.value().status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(tool_result.value().terminal_error ==
            std::optional<agent::RuntimeError>{tool_error});
}

TEST_CASE(time_budget_terminal_rejects_awaiting_tool_after_all_work_is_processed) {
    const std::string task = "post-tool-time-budget";
    std::vector<agent::RuntimeEvent> events = {
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::model_started(task, 4),
        fixtures::model_succeeded(
            task, 5, {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
        fixtures::tool_started(task, 6, fixtures::first_call()),
        fixtures::tool_succeeded(task, 7, fixtures::first_result()),
    };
    const auto prefix = agent::replay_events(events);
    REQUIRE(prefix.has_value());
    REQUIRE(prefix.value().status == agent::TaskStatus::AwaitingTool);
    REQUIRE(!prefix.value().active_tool_call_id.has_value());
    REQUIRE(prefix.value().next_tool_index ==
            prefix.value().pending_tool_calls.size());
    events.push_back(fixtures::event(
        task, 8,
        agent::TaskBudgetExceededPayload{
            "max_task_time_ms",
            {agent::ErrorCode::BudgetExceeded,
             "max_task_time_ms budget exceeded", false}}));

    const auto result = agent::replay_events(events);

    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(generic_budget_terminal_rejects_unknown_or_empty_names) {
    for (const auto& name : {std::string{}, std::string{"unknown_budget"}}) {
        const std::string task = name.empty() ? "empty-budget" : "unknown-budget";
        const auto result = agent::replay_events({
            fixtures::task_started(task, 1, "issue"),
            fixtures::context_started(task, 2),
            fixtures::event(
                task, 3,
                agent::TaskBudgetExceededPayload{
                    name,
                    {agent::ErrorCode::BudgetExceeded,
                     name + " budget exceeded", false}}),
        });
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    }
}

TEST_CASE(generic_budget_terminal_requires_exact_payload_for_each_name) {
    struct LegalPrefix {
        const char* task;
        const char* name;
        const char* message;
        std::vector<agent::RuntimeEvent> events;
    };
    const std::vector<LegalPrefix> prefixes = {
        {"tamper-time", "max_task_time_ms", "max_task_time_ms budget exceeded",
         {fixtures::task_started("tamper-time", 1, "issue"),
          fixtures::context_started("tamper-time", 2)}},
        {"tamper-model", "max_model_rounds", "max_model_rounds budget exceeded",
         {fixtures::task_started("tamper-model", 1, "issue",
                                 {1, 12, 90'000, 30'000}),
          fixtures::context_started("tamper-model", 2),
          fixtures::context_prepared("tamper-model", 3, "source-1"),
          fixtures::model_started("tamper-model", 4, "source-1"),
          fixtures::model_succeeded(
              "tamper-model", 5,
              {agent::ToolUseBlock{fixtures::first_call()}},
              agent::StopReason::ToolUse),
          fixtures::tool_started("tamper-model", 6, fixtures::first_call()),
          fixtures::tool_succeeded("tamper-model", 7, fixtures::first_result()),
          fixtures::context_started("tamper-model", 8),
          fixtures::context_prepared("tamper-model", 9, "source-2")}},
        {"tamper-tool", "max_tool_calls", "max_tool_calls budget exceeded",
         {fixtures::task_started("tamper-tool", 1, "issue",
                                 {8, 1, 90'000, 30'000}),
          fixtures::context_started("tamper-tool", 2),
          fixtures::context_prepared("tamper-tool", 3, "source"),
          fixtures::model_started("tamper-tool", 4),
          fixtures::model_succeeded(
              "tamper-tool", 5,
              {agent::ToolUseBlock{fixtures::first_call()},
               agent::ToolUseBlock{fixtures::second_call()}},
              agent::StopReason::ToolUse),
          fixtures::tool_started("tamper-tool", 6, fixtures::first_call()),
          fixtures::tool_succeeded("tamper-tool", 7, fixtures::first_result())}},
    };

    for (const auto& prefix : prefixes) {
        const std::vector<agent::RuntimeError> tampered = {
            {agent::ErrorCode::InvalidInput, prefix.message, false},
            {agent::ErrorCode::BudgetExceeded, "tampered budget message", false},
            {agent::ErrorCode::BudgetExceeded, prefix.message, true},
        };
        for (const auto& error : tampered) {
            auto events = prefix.events;
            events.push_back(fixtures::event(
                prefix.task, events.back().sequence + 1,
                agent::TaskBudgetExceededPayload{prefix.name, error}));
            const auto result = agent::replay_events(events);
            REQUIRE(!result.has_value());
            REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
        }
    }
}

TEST_CASE(generic_budget_terminal_distinguishes_time_from_count_guards) {
    struct RuntimePrefix {
        const char* task;
        std::vector<agent::RuntimeEvent> events;
    };
    const std::vector<RuntimePrefix> prefixes = {
        {"budget-created",
         {fixtures::task_started("budget-created", 1, "issue",
                                 {1, 1, 90'000, 30'000})}},
        {"budget-model-active",
          {fixtures::task_started("budget-model-active", 1, "issue",
                                  {2, 1, 90'000, 30'000}),
           fixtures::context_started("budget-model-active", 2),
           fixtures::context_prepared("budget-model-active", 3, "source-1"),
           fixtures::model_started("budget-model-active", 4, "source-1")}},
        {"budget-tool-active",
         {fixtures::task_started("budget-tool-active", 1, "issue",
                                 {1, 1, 90'000, 30'000}),
          fixtures::context_started("budget-tool-active", 2),
          fixtures::context_prepared("budget-tool-active", 3, "source"),
          fixtures::model_started("budget-tool-active", 4),
          fixtures::model_succeeded(
              "budget-tool-active", 5,
              {agent::ToolUseBlock{fixtures::first_call()},
               agent::ToolUseBlock{fixtures::second_call()}},
              agent::StopReason::ToolUse),
          fixtures::tool_started("budget-tool-active", 6,
                                 fixtures::first_call())}},
    };
    const std::vector<std::pair<std::string, std::string>> count_budgets = {
        {"max_model_rounds", "max_model_rounds budget exceeded"},
        {"max_tool_calls", "max_tool_calls budget exceeded"},
    };
    for (const auto& prefix : prefixes) {
        for (const auto& budget : count_budgets) {
            auto events = prefix.events;
            events.push_back(fixtures::event(
                prefix.task, events.back().sequence + 1,
                agent::TaskBudgetExceededPayload{
                    budget.first,
                    {agent::ErrorCode::BudgetExceeded, budget.second, false}}));
            const auto result = agent::replay_events(events);
            REQUIRE(!result.has_value());
            REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
        }
    }

    {
        auto events = prefixes.front().events;
        events.push_back(fixtures::event(
            prefixes.front().task, 2,
            agent::TaskBudgetExceededPayload{
                "max_task_time_ms",
                {agent::ErrorCode::BudgetExceeded,
                 "max_task_time_ms budget exceeded", false}}));
        const auto result = agent::replay_events(events);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
    }
    for (std::size_t index = 1; index < prefixes.size(); ++index) {
        auto events = prefixes[index].events;
        events.push_back(fixtures::event(
            prefixes[index].task, events.back().sequence + 1,
            agent::TaskBudgetExceededPayload{
                "max_task_time_ms",
                {agent::ErrorCode::BudgetExceeded,
                 "max_task_time_ms budget exceeded", false}}));
        const auto result = agent::replay_events(events);
        REQUIRE(result.has_value());
        REQUIRE(result.value().status == agent::TaskStatus::BudgetExceeded);
    }
}

TEST_CASE(model_round_budget_terminal_requires_exhausted_count) {
    const std::string task = "unexhausted-model-budget";
    const auto result = agent::replay_events({
        fixtures::task_started(task, 1, "issue"),
        fixtures::context_started(task, 2),
        fixtures::context_prepared(task, 3, "source"),
        fixtures::event(
            task, 4,
            agent::TaskBudgetExceededPayload{
                "max_model_rounds",
                {agent::ErrorCode::BudgetExceeded,
                 "max_model_rounds budget exceeded", false}}),
    });
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(tool_call_budget_terminal_requires_exhausted_pending_work_and_status) {
    const agent::RuntimeError error{
        agent::ErrorCode::BudgetExceeded,
        "max_tool_calls budget exceeded", false};
    const std::string unexhausted_task = "unexhausted-tool-budget";
    auto unexhausted = std::vector<agent::RuntimeEvent>{
        fixtures::task_started(unexhausted_task, 1, "issue"),
        fixtures::context_started(unexhausted_task, 2),
        fixtures::context_prepared(unexhausted_task, 3, "source"),
        fixtures::model_started(unexhausted_task, 4),
        fixtures::model_succeeded(
            unexhausted_task, 5,
            {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
    };
    unexhausted.push_back(fixtures::event(
        unexhausted_task, 6,
        agent::TaskBudgetExceededPayload{"max_tool_calls", error}));

    const std::string no_pending_task = "no-pending-tool-budget";
    auto no_pending = std::vector<agent::RuntimeEvent>{
        fixtures::task_started(no_pending_task, 1, "issue",
                               {8, 1, 90'000, 30'000}),
        fixtures::context_started(no_pending_task, 2),
        fixtures::context_prepared(no_pending_task, 3, "source"),
        fixtures::model_started(no_pending_task, 4),
        fixtures::model_succeeded(
            no_pending_task, 5,
            {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
        fixtures::tool_started(no_pending_task, 6, fixtures::first_call()),
        fixtures::tool_succeeded(no_pending_task, 7, fixtures::first_result()),
    };
    no_pending.push_back(fixtures::event(
        no_pending_task, 8,
        agent::TaskBudgetExceededPayload{"max_tool_calls", error}));

    const std::string wrong_status_task = "wrong-status-tool-budget";
    auto wrong_status = std::vector<agent::RuntimeEvent>{
        fixtures::task_started(wrong_status_task, 1, "issue",
                               {8, 1, 90'000, 30'000}),
        fixtures::context_started(wrong_status_task, 2),
        fixtures::context_prepared(wrong_status_task, 3, "source-1"),
        fixtures::model_started(wrong_status_task, 4, "source-1"),
        fixtures::model_succeeded(
            wrong_status_task, 5,
            {agent::ToolUseBlock{fixtures::first_call()}},
            agent::StopReason::ToolUse),
        fixtures::tool_started(wrong_status_task, 6, fixtures::first_call()),
        fixtures::tool_succeeded(wrong_status_task, 7, fixtures::first_result()),
        fixtures::context_started(wrong_status_task, 8),
        fixtures::context_prepared(wrong_status_task, 9, "source-2"),
    };
    wrong_status.push_back(fixtures::event(
        wrong_status_task, 10,
        agent::TaskBudgetExceededPayload{"max_tool_calls", error}));

    for (const auto& events : {unexhausted, no_pending, wrong_status}) {
        const auto result = agent::replay_events(events);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
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
