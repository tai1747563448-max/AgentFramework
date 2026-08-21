#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace test {

class FakeModel final : public agent::ModelClient {
public:
    explicit FakeModel(std::vector<agent::ModelResponse> responses)
        : responses_(std::move(responses)) {}

    explicit FakeModel(agent::RuntimeError error)
        : error_(std::move(error)) {}

    agent::Result<agent::ModelResponse> complete(
        const agent::ModelRequest& request) override {
        requests.push_back(request);
        if (error_.has_value()) {
            return agent::Result<agent::ModelResponse>::failure(*error_);
        }
        if (next_response_ >= responses_.size()) {
            return agent::Result<agent::ModelResponse>::failure(
                {agent::ErrorCode::ProtocolFailure,
                 "fake model response script exhausted", false});
        }
        return agent::Result<agent::ModelResponse>::success(
            responses_.at(next_response_++));
    }

    std::vector<agent::ModelRequest> requests;

private:
    std::vector<agent::ModelResponse> responses_;
    std::optional<agent::RuntimeError> error_;
    std::size_t next_response_{0};
};

class FakeTools final : public agent::ToolGateway {
public:
    FakeTools() = default;
    explicit FakeTools(std::vector<agent::ToolDefinition> definitions)
        : definitions_(std::move(definitions)) {}
    explicit FakeTools(std::vector<agent::ToolResult> results) {
        results_.reserve(results.size());
        for (auto& result : results) {
            results_.push_back(
                agent::Result<agent::ToolResult>::success(std::move(result)));
        }
    }
    explicit FakeTools(agent::RuntimeError error) {
        results_.push_back(
            agent::Result<agent::ToolResult>::failure(std::move(error)));
    }

    std::vector<agent::ToolDefinition> definitions() const override {
        ++definitions_calls;
        return definitions_;
    }

    agent::Result<agent::ToolResult> execute(
        const agent::ToolCall& call,
        const agent::ToolExecutionContext& context) override {
        executed_calls.push_back(call);
        executed_contexts.push_back(context);
        if (next_result_ >= results_.size()) {
            return agent::Result<agent::ToolResult>::failure(
                {agent::ErrorCode::DependencyUnavailable,
                 "fake tool result script exhausted", false});
        }
        return results_.at(next_result_++);
    }

    std::vector<std::string> executed_ids() const {
        std::vector<std::string> ids;
        ids.reserve(executed_calls.size());
        for (const auto& call : executed_calls) {
            ids.push_back(call.id);
        }
        return ids;
    }

    mutable std::size_t definitions_calls{0};
    std::vector<agent::ToolCall> executed_calls;
    std::vector<agent::ToolExecutionContext> executed_contexts;

private:
    std::vector<agent::ToolDefinition> definitions_;
    std::vector<agent::Result<agent::ToolResult>> results_;
    std::size_t next_result_{0};
};

class FakeKnowledge final : public agent::KnowledgeProvider {
public:
    explicit FakeKnowledge(agent::EvidencePack evidence)
        : evidence_(std::move(evidence)) {}

    explicit FakeKnowledge(agent::RuntimeError error)
        : error_(std::move(error)) {}

    agent::Result<agent::EvidencePack> retrieve(
        const agent::TaskState& state) override {
        retrieved_states.push_back(state);
        if (error_.has_value()) {
            return agent::Result<agent::EvidencePack>::failure(*error_);
        }
        return agent::Result<agent::EvidencePack>::success(evidence_);
    }

    std::vector<agent::TaskState> retrieved_states;

private:
    agent::EvidencePack evidence_;
    std::optional<agent::RuntimeError> error_;
};

class MemoryEventStore : public agent::EventStore {
public:
    agent::Result<void> append(const agent::RuntimeEvent& event) override {
        events.push_back(event);
        return agent::Result<void>::success();
    }

    agent::Result<std::vector<agent::RuntimeEvent>> read_file(
        const std::filesystem::path&) const override {
        return agent::Result<std::vector<agent::RuntimeEvent>>::success(events);
    }

    std::vector<agent::EventKind> kinds() const {
        std::vector<agent::EventKind> result;
        result.reserve(events.size());
        for (const auto& event : events) {
            result.push_back(agent::event_kind(event.payload));
        }
        return result;
    }

    std::size_t count(agent::EventKind kind) const {
        std::size_t result = 0;
        for (const auto& event : events) {
            if (agent::event_kind(event.payload) == kind) {
                ++result;
            }
        }
        return result;
    }

    std::vector<agent::RuntimeEvent> events;
};

class FailingEventStore final : public MemoryEventStore {
public:
    explicit FailingEventStore(std::size_t fail_on_append)
        : fail_on_append_(fail_on_append) {}

    agent::Result<void> append(const agent::RuntimeEvent& event) override {
        ++append_attempts;
        if (append_attempts == fail_on_append_) {
            return agent::Result<void>::failure(
                {agent::ErrorCode::PersistenceFailure,
                 "scripted event store append failure", true});
        }
        return MemoryEventStore::append(event);
    }

    std::size_t append_attempts{0};

private:
    std::size_t fail_on_append_;
};

class FakeClock final : public agent::Clock {
public:
    FakeClock() = default;
    explicit FakeClock(std::vector<std::int64_t> monotonic_values)
        : monotonic_values_(std::move(monotonic_values)) {}

    std::string now_utc() const override {
        return "2026-08-17T12:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        if (monotonic_values_.empty()) {
            return 1'000;
        }
        const auto index = next_monotonic_ < monotonic_values_.size()
                               ? next_monotonic_++
                               : monotonic_values_.size() - 1;
        return monotonic_values_.at(index);
    }

private:
    std::vector<std::int64_t> monotonic_values_;
    mutable std::size_t next_monotonic_{0};
};

class FakeIds final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return "task-00000000000000000000000000000001";
    }

    std::string next_correlation_id() override {
        return "corr-" + std::to_string(next_correlation_++);
    }

private:
    std::size_t next_correlation_{1};
};

class FakeCancellation final : public agent::Cancellation {
public:
    explicit FakeCancellation(bool requested = false) : requested_(requested) {}
    explicit FakeCancellation(std::vector<bool> requested_values)
        : requested_values_(std::move(requested_values)) {}

    bool requested() const noexcept override {
        if (!requested_values_.empty()) {
            const auto index = next_requested_ < requested_values_.size()
                                   ? next_requested_++
                                   : requested_values_.size() - 1;
            return requested_values_.at(index);
        }
        return requested_;
    }

private:
    bool requested_{false};
    std::vector<bool> requested_values_;
    mutable std::size_t next_requested_{0};
};

template <typename Store = MemoryEventStore>
struct EngineFixture {
    FakeModel model;
    FakeTools tools;
    FakeKnowledge knowledge;
    Store events;
    FakeClock clock;
    FakeIds ids;
    FakeCancellation cancel;
    agent::RuntimeEngine engine;

    EngineFixture(FakeModel model_value,
                  FakeTools tools_value,
                  FakeKnowledge knowledge_value,
                  Store events_value = Store{},
                  FakeClock clock_value = FakeClock{},
                  FakeCancellation cancel_value = FakeCancellation{})
        : model(std::move(model_value)),
          tools(std::move(tools_value)),
          knowledge(std::move(knowledge_value)),
          events(std::move(events_value)),
          clock(std::move(clock_value)),
          cancel(std::move(cancel_value)),
          engine(model, tools, knowledge, events, clock, ids, cancel) {}

    EngineFixture(const EngineFixture&) = delete;
    EngineFixture& operator=(const EngineFixture&) = delete;
    EngineFixture(EngineFixture&&) = delete;
    EngineFixture& operator=(EngineFixture&&) = delete;

    agent::RuntimeResult run(const agent::RunRequest& request) {
        return engine.run(request, {});
    }

    agent::RuntimeResult run(
        const agent::RunRequest& request,
        const agent::RuntimeProgressObserver& observer) {
        return engine.run(request, observer);
    }
};

}  // namespace test

namespace fixtures {

agent::RuntimeBudgets budgets() {
    return {8, 12, 90'000, 30'000};
}

agent::RunRequest run_request(std::string issue) {
    return {std::move(issue), u8"E:/工作区/项目", u8"你是编码代理。", budgets()};
}

agent::ModelResponse response(std::vector<agent::ContentBlock> content) {
    return {std::move(content), agent::StopReason::EndTurn, "end_turn",
            21, 8, "provider-request-1"};
}

agent::ModelResponse text_response(std::string text) {
    return response({agent::TextBlock{std::move(text)}});
}

agent::ModelResponse stopped_response(std::vector<agent::ContentBlock> content,
                                      agent::StopReason stop_reason,
                                      std::string raw_stop_reason) {
    auto result = response(std::move(content));
    result.stop_reason = stop_reason;
    result.raw_stop_reason = std::move(raw_stop_reason);
    return result;
}

agent::ToolCall call(std::string id, std::string name) {
    return {std::move(id), std::move(name),
            agent::Value::object({{"path", agent::Value("src/main.cpp")}})};
}

agent::ModelResponse tool_response(std::vector<agent::ToolCall> calls) {
    std::vector<agent::ContentBlock> content;
    content.reserve(calls.size());
    for (auto& tool_call : calls) {
        content.push_back(agent::ToolUseBlock{std::move(tool_call)});
    }
    auto result = response(std::move(content));
    result.stop_reason = agent::StopReason::ToolUse;
    result.raw_stop_reason = "tool_use";
    return result;
}

agent::EvidencePack evidence() {
    return {{{"source-1", "first evidence",
              agent::Value::object({{"rank", agent::Value(std::int64_t{1})}})},
             {"source-2", "second evidence",
              agent::Value::object({{"rank", agent::Value(std::int64_t{2})}})}}};
}

std::vector<agent::ToolDefinition> tool_definitions() {
    return {{"read_file", "read a file",
             agent::Value::object({{"type", agent::Value("object")}})},
            {"run_build", "run the build",
             agent::Value::object({{"type", agent::Value("object")}})}};
}

template <typename Store>
test::EngineFixture<Store> engine_with(Store store) {
    return test::EngineFixture<Store>(
        test::FakeModel({text_response("done")}), test::FakeTools{},
        test::FakeKnowledge(agent::EvidencePack{}), std::move(store));
}

}  // namespace fixtures

TEST_CASE(engine_completes_single_model_turn) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::response(
            {agent::TextBlock{"do"}, agent::TextBlock{"ne"}})}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->final_text == std::optional<std::string>{"done"});
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::TaskCompleted};
    REQUIRE(fixture.events.kinds() == expected_kinds);
}

TEST_CASE(runtime_progress_type_is_exactly_the_safe_four_field_projection) {
    const agent::RuntimeProgress progress{
        "task-00000000000000000000000000000001", 7,
        agent::EventKind::ModelCallSucceeded,
        agent::TaskStatus::AwaitingModel};
    const auto& [task_id, sequence, event_kind, status] = progress;

    static_assert(std::is_same_v<
                  decltype(agent::RuntimeProgress::task_id), std::string>);
    static_assert(std::is_same_v<
                  decltype(agent::RuntimeProgress::sequence), std::uint64_t>);
    static_assert(std::is_same_v<
                  decltype(agent::RuntimeProgress::event_kind),
                  agent::EventKind>);
    static_assert(std::is_same_v<
                  decltype(agent::RuntimeProgress::status),
                  agent::TaskStatus>);
    REQUIRE(task_id == "task-00000000000000000000000000000001");
    REQUIRE(sequence == 7);
    REQUIRE(event_kind == agent::EventKind::ModelCallSucceeded);
    REQUIRE(status == agent::TaskStatus::AwaitingModel);
}

TEST_CASE(engine_reports_post_reduce_progress_only_after_each_durable_append) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::text_response("done")}), test::FakeTools{},
        test::FakeKnowledge(agent::EvidencePack{}));
    std::vector<agent::RuntimeProgress> observed;

    const auto result = fixture.run(
        fixtures::run_request("fix warning"),
        [&](const agent::RuntimeProgress& progress) {
            REQUIRE(fixture.events.events.size() == progress.sequence);
            const auto replayed = agent::replay_events(fixture.events.events);
            REQUIRE(replayed.has_value());
            REQUIRE(replayed.value().task_id == progress.task_id);
            REQUIRE(replayed.value().last_sequence == progress.sequence);
            REQUIRE(replayed.value().status == progress.status);
            observed.push_back(progress);
        });

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(observed.size() == fixture.events.events.size());
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::TaskCompleted};
    const std::vector<agent::TaskStatus> expected_statuses{
        agent::TaskStatus::Created,
        agent::TaskStatus::PreparingContext,
        agent::TaskStatus::AwaitingModel,
        agent::TaskStatus::AwaitingModel,
        agent::TaskStatus::AwaitingModel,
        agent::TaskStatus::Completed};
    REQUIRE(observed.size() == expected_kinds.size());
    for (std::size_t index = 0; index < observed.size(); ++index) {
        REQUIRE(observed[index].task_id ==
                "task-00000000000000000000000000000001");
        REQUIRE(observed[index].sequence == index + 1);
        REQUIRE(observed[index].event_kind == expected_kinds[index]);
        REQUIRE(observed[index].status == expected_statuses[index]);
        REQUIRE(observed[index].event_kind ==
                agent::event_kind(fixture.events.events[index].payload));
    }
}

TEST_CASE(engine_never_reports_a_rejected_event_store_append) {
    auto fixture = fixtures::engine_with(test::FailingEventStore(4));
    std::vector<agent::RuntimeProgress> observed;

    const auto result = fixture.run(
        fixtures::run_request("fix warning"),
        [&](const agent::RuntimeProgress& progress) {
            REQUIRE(fixture.events.events.size() == progress.sequence);
            observed.push_back(progress);
        });

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixture.events.append_attempts == 4);
    REQUIRE(fixture.events.events.size() == 3);
    REQUIRE(observed.size() == 3);
    REQUIRE(observed.back().sequence == result.state->last_sequence);
    REQUIRE(observed.back().status == result.state->status);
    REQUIRE(observed.back().event_kind == agent::EventKind::ContextPrepared);
}

TEST_CASE(throwing_progress_observer_is_disabled_without_interrupting_task) {
    test::EngineFixture throwing_fixture(
        test::FakeModel({
            fixtures::tool_response({fixtures::call("call-1", "read"),
                                     fixtures::call("call-2", "search")}),
            fixtures::text_response("done")}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(fixtures::evidence()));
    test::EngineFixture empty_observer_fixture(
        test::FakeModel({
            fixtures::tool_response({fixtures::call("call-1", "read"),
                                     fixtures::call("call-2", "search")}),
            fixtures::text_response("done")}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(fixtures::evidence()));
    std::size_t observer_calls = 0;
    agent::RuntimeProgressObserver throwing_observer =
        [&](const agent::RuntimeProgress&) {
            ++observer_calls;
            throw std::runtime_error("SENTINEL_REPORTING_FAILURE");
        };

    const auto result = throwing_fixture.run(
        fixtures::run_request("inspect code"), throwing_observer);
    const auto empty_observer_result =
        empty_observer_fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(observer_calls == 1);
    REQUIRE(static_cast<bool>(throwing_observer));
    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->final_text == std::optional<std::string>{"done"});
    REQUIRE(!result.state->terminal_error.has_value());
    REQUIRE(empty_observer_result.state.has_value());
    REQUIRE(!empty_observer_result.fatal_error.has_value());
    REQUIRE(*result.state == *empty_observer_result.state);
    REQUIRE(throwing_fixture.events.events ==
            empty_observer_fixture.events.events);
    REQUIRE(throwing_fixture.events.events.size() == 14);
    REQUIRE(throwing_fixture.model.requests.size() == 2);
    REQUIRE(throwing_fixture.knowledge.retrieved_states.size() == 2);
    REQUIRE(throwing_fixture.tools.definitions_calls == 2);
    REQUIRE((throwing_fixture.tools.executed_ids() ==
             std::vector<std::string>{"call-1", "call-2"}));
}

TEST_CASE(engine_builds_model_request_from_durable_inputs_in_order) {
    const auto expected_tools = fixtures::tool_definitions();
    test::EngineFixture fixture(
        test::FakeModel({fixtures::text_response("done")}),
        test::FakeTools(expected_tools), test::FakeKnowledge(fixtures::evidence()));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(fixture.model.requests.size() == 1);
    const auto& request = fixture.model.requests.front();
    REQUIRE(request.system_prompt == u8"你是编码代理。");
    REQUIRE(request.timeout_ms == 30'000);
    REQUIRE(request.tools == expected_tools);
    REQUIRE(request.evidence == fixtures::evidence());
    const std::vector<agent::Message> expected_messages{
        {agent::Role::User, {agent::TextBlock{"fix warning"}}}};
    REQUIRE(request.messages == expected_messages);
    const auto* started = std::get_if<agent::ModelCallStartedPayload>(
        &fixture.events.events.at(3).payload);
    REQUIRE(started != nullptr);
    REQUIRE(started->request == request);
}

TEST_CASE(event_store_failure_does_not_apply_event_or_append_failure_event) {
    auto fixture = fixtures::engine_with(test::FailingEventStore(4));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(result.state->last_sequence == 3);
    REQUIRE(result.state->status == agent::TaskStatus::AwaitingModel);
    REQUIRE(fixture.events.append_attempts == 4);
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared};
    REQUIRE(fixture.events.kinds() == expected_kinds);
    REQUIRE(fixture.model.requests.empty());
}

TEST_CASE(knowledge_failure_appends_only_specialized_terminal_event) {
    const agent::RuntimeError expected{
        agent::ErrorCode::DependencyUnavailable, "knowledge unavailable", true};
    test::EngineFixture fixture(
        test::FakeModel({fixtures::text_response("unused")}), test::FakeTools{},
        test::FakeKnowledge(expected));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Failed);
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPreparationFailed};
    REQUIRE(fixture.events.kinds() == expected_kinds);
    REQUIRE(fixture.model.requests.empty());
}

TEST_CASE(model_failure_appends_only_specialized_terminal_event) {
    const agent::RuntimeError expected{
        agent::ErrorCode::RequestTimeout, "model request timed out", true};
    test::EngineFixture fixture(test::FakeModel(expected), test::FakeTools{},
                                test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Failed);
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallFailed};
    REQUIRE(fixture.events.kinds() == expected_kinds);
}

TEST_CASE(empty_final_response_is_a_specialized_protocol_failure) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::response({})}), test::FakeTools{},
        test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("fix warning"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Failed);
    REQUIRE(result.state->terminal_error.has_value());
    REQUIRE(result.state->terminal_error->code ==
            agent::ErrorCode::ProtocolFailure);
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallFailed};
    REQUIRE(fixture.events.kinds() == expected_kinds);
    const auto* failure = std::get_if<agent::ModelCallFailedPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->error == *result.state->terminal_error);
}

TEST_CASE(engine_executes_multiple_tools_in_response_order) {
    test::EngineFixture fixture(
        test::FakeModel({
            fixtures::tool_response({fixtures::call("call-1", "read"),
                                     fixtures::call("call-2", "search")}),
            fixtures::text_response("done")}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(fixtures::evidence()));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    const std::vector<std::string> expected_ids{"call-1", "call-2"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.tools.executed_contexts.size() == 2);
    for (const auto& context : fixture.tools.executed_contexts) {
        REQUIRE(context.workspace_utf8 == u8"E:/工作区/项目");
    }
    REQUIRE(fixture.model.requests.size() == 2);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 2);
    const auto& second_request = fixture.model.requests.at(1);
    REQUIRE(second_request.evidence == fixtures::evidence());
    REQUIRE(second_request.messages.back().role == agent::Role::User);
    REQUIRE(second_request.messages.back().content.size() == 2);
    const auto* first_result = std::get_if<agent::ToolResultBlock>(
        &second_request.messages.back().content.at(0));
    const auto* second_result = std::get_if<agent::ToolResultBlock>(
        &second_request.messages.back().content.at(1));
    REQUIRE(first_result != nullptr);
    REQUIRE(second_result != nullptr);
    REQUIRE(first_result->result.tool_call_id == "call-1");
    REQUIRE(second_result->result.tool_call_id == "call-2");
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::ToolCallStarted,
        agent::EventKind::ToolCallSucceeded,
        agent::EventKind::ToolCallStarted,
        agent::EventKind::ToolCallSucceeded,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::TaskCompleted};
    REQUIRE(fixture.events.kinds() == expected_kinds);
}

TEST_CASE(engine_rejects_invalid_or_duplicate_response_tool_calls) {
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
        std::vector<agent::ToolResult> results;
        for (const auto& block : content) {
            const auto& call = std::get<agent::ToolUseBlock>(block).call;
            results.push_back({call.id, "result", false});
        }
        test::EngineFixture fixture(
            test::FakeModel({fixtures::stopped_response(
                                 content, agent::StopReason::ToolUse,
                                 "tool_use"),
                             fixtures::text_response("done")}),
            test::FakeTools(std::move(results)),
            test::FakeKnowledge(agent::EvidencePack{}));

        const auto result = fixture.run(
            fixtures::run_request("validate tool calls"));

        REQUIRE(result.state.has_value());
        REQUIRE(!result.fatal_error.has_value());
        REQUIRE(result.state->status == agent::TaskStatus::Failed);
        REQUIRE(result.state->terminal_error.has_value());
        REQUIRE(result.state->terminal_error->code ==
                agent::ErrorCode::ProtocolFailure);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallFailed) == 1);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::ToolCallStarted) == 0);
        REQUIRE(fixture.tools.executed_calls.empty());
    }
}

TEST_CASE(tool_error_result_is_not_gateway_failure) {
    test::EngineFixture fixture(
        test::FakeModel({
            fixtures::tool_response({fixtures::call("call-1", "build")}),
            fixtures::text_response("done")}),
        test::FakeTools(
            {agent::ToolResult{"call-1", "compiler failed", true}}),
        test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("build"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallFailed) == 0);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallSucceeded) == 1);
    const auto& tool_message = fixture.model.requests.at(1).messages.back();
    const auto* returned =
        std::get_if<agent::ToolResultBlock>(&tool_message.content.front());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->result.is_error);
}

TEST_CASE(tool_gateway_failure_is_the_only_terminal_failure_event) {
    const agent::RuntimeError expected{
        agent::ErrorCode::DependencyUnavailable, "tool host disconnected", true};
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read"),
             fixtures::call("call-2", "search")})}),
        test::FakeTools(expected),
        test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Failed);
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallFailed) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskFailed) == 0);
    const std::vector<std::string> expected_ids{"call-1"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
    REQUIRE(fixture.tools.definitions_calls == 1);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::ToolCallFailed);
}

TEST_CASE(model_round_budget_stops_before_the_next_model_call) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false}}),
        test::FakeKnowledge(agent::EvidencePack{}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_model_rounds = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.tools.executed_calls.size() == 1);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 2);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_model_rounds");
}

TEST_CASE(tool_call_budget_stops_before_the_next_tool_call) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read"),
             fixtures::call("call-2", "search")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(agent::EvidencePack{}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_tool_calls = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    const std::vector<std::string> expected_ids{"call-1"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallStarted) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_tool_calls");
}

TEST_CASE(wall_time_budget_stops_before_knowledge_retrieval) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::text_response("unused")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}),
        test::MemoryEventStore{}, test::FakeClock({1'000, 1'100}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_task_time_ms = 100;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(fixture.knowledge.retrieved_states.empty());
    REQUIRE(fixture.model.requests.empty());
    REQUIRE(fixture.tools.definitions_calls == 0);
    REQUIRE(fixture.tools.executed_calls.empty());
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_task_time_ms");
}

TEST_CASE(cancellation_wins_before_wall_time_and_count_guards) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::text_response("unused")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}),
        test::MemoryEventStore{}, test::FakeClock({1'000, 2'000}),
        test::FakeCancellation(true));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_task_time_ms = 1;
    request.budgets.max_model_rounds = 1;
    request.budgets.max_tool_calls = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Cancelled);
    REQUIRE(fixture.events.count(agent::EventKind::TaskCancelled) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 0);
    REQUIRE(fixture.knowledge.retrieved_states.empty());
    REQUIRE(fixture.model.requests.empty());
    REQUIRE(fixture.tools.definitions_calls == 0);
    REQUIRE(fixture.tools.executed_calls.empty());
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::TaskCancelled);
}

TEST_CASE(wall_time_guard_wins_before_model_round_limit) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false}}),
        test::FakeKnowledge(agent::EvidencePack{}), test::MemoryEventStore{},
        test::FakeClock({1'000, 1'000, 1'000, 1'000,
                         1'000, 1'000, 1'000, 1'100}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_task_time_ms = 100;
    request.budgets.max_model_rounds = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 2);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.tools.definitions_calls == 1);
    REQUIRE(fixture.tools.executed_calls.size() == 1);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_task_time_ms");
}

TEST_CASE(cancellation_before_second_tool_wins_over_exhausted_tool_budget) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read"),
             fixtures::call("call-2", "search")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(agent::EvidencePack{}), test::MemoryEventStore{},
        test::FakeClock{},
        test::FakeCancellation(
            {false, false, false, false, false, true}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_tool_calls = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Cancelled);
    const std::vector<std::string> expected_ids{"call-1"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskCancelled) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 0);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::TaskCancelled);
}

TEST_CASE(wall_time_before_second_tool_wins_over_exhausted_tool_budget) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read"),
             fixtures::call("call-2", "search")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(agent::EvidencePack{}), test::MemoryEventStore{},
        test::FakeClock(
            {1'000, 1'000, 1'000, 1'000, 1'000, 1'000, 1'100}));
    auto request = fixtures::run_request("inspect code");
    request.budgets.max_task_time_ms = 100;
    request.budgets.max_tool_calls = 1;

    const auto result = fixture.run(request);

    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    const std::vector<std::string> expected_ids{"call-1"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_task_time_ms");
}

TEST_CASE(tool_start_persistence_failure_prevents_tool_execution) {
    test::EngineFixture<test::FailingEventStore> fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false}}),
        test::FakeKnowledge(agent::EvidencePack{}),
        test::FailingEventStore(6));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(result.state->status == agent::TaskStatus::AwaitingTool);
    REQUIRE(result.state->last_sequence == 5);
    REQUIRE(fixture.events.append_attempts == 6);
    REQUIRE(fixture.tools.executed_calls.empty());
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::ModelCallSucceeded);
}

TEST_CASE(tool_success_persistence_failure_prevents_the_next_tool) {
    test::EngineFixture<test::FailingEventStore> fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read"),
             fixtures::call("call-2", "search")})}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false},
                         agent::ToolResult{"call-2", "matches", false}}),
        test::FakeKnowledge(agent::EvidencePack{}),
        test::FailingEventStore(7));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::AwaitingTool);
    REQUIRE(result.state->last_sequence == 6);
    REQUIRE(fixture.events.append_attempts == 7);
    const std::vector<std::string> expected_ids{"call-1"};
    REQUIRE(fixture.tools.executed_ids() == expected_ids);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::ToolCallStarted);
}

TEST_CASE(mismatched_successful_tool_result_becomes_direct_tool_failure) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::tool_response(
            {fixtures::call("call-1", "read")})}),
        test::FakeTools(
            {agent::ToolResult{"wrong-call", "untrusted result", false}}),
        test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("inspect code"));
    const agent::RuntimeError expected{
        agent::ErrorCode::ProtocolFailure,
        "tool result ID does not match active tool call", false};

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Failed);
    REQUIRE(result.state->last_sequence == 7);
    REQUIRE(!result.state->active_tool_call_id.has_value());
    REQUIRE(result.state->pending_tool_results.empty());
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallFailed) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallSucceeded) == 0);
    REQUIRE(fixture.events.count(agent::EventKind::TaskFailed) == 0);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::ToolCallFailed);
    const auto* failed = std::get_if<agent::ToolCallFailedPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(failed != nullptr);
    REQUIRE(failed->tool_call_id == "call-1");
    REQUIRE(failed->error == expected);
}

TEST_CASE(post_tool_context_persistence_failure_prevents_later_external_calls) {
    test::EngineFixture<test::FailingEventStore> fixture(
        test::FakeModel({
            fixtures::tool_response({fixtures::call("call-1", "read")}),
            fixtures::text_response("unused")}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false}}),
        test::FakeKnowledge(agent::EvidencePack{}),
        test::FailingEventStore(8));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::AwaitingTool);
    REQUIRE(result.state->last_sequence == 7);
    REQUIRE(fixture.events.append_attempts == 8);
    REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
    REQUIRE(fixture.model.requests.size() == 1);
    REQUIRE(fixture.tools.executed_calls.size() == 1);
    REQUIRE(fixture.events.kinds().back() == agent::EventKind::ToolCallSucceeded);
}

TEST_CASE(terminal_guard_persistence_failure_prevents_all_external_calls) {
    test::EngineFixture<test::FailingEventStore> fixture(
        test::FakeModel({fixtures::text_response("unused")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}),
        test::FailingEventStore(3), test::FakeClock{},
        test::FakeCancellation(true));

    const auto result = fixture.run(fixtures::run_request("inspect code"));

    REQUIRE(result.state.has_value());
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(result.state->status == agent::TaskStatus::PreparingContext);
    REQUIRE(result.state->last_sequence == 2);
    REQUIRE(fixture.events.append_attempts == 3);
    REQUIRE(fixture.events.count(agent::EventKind::TaskCancelled) == 0);
    REQUIRE(fixture.knowledge.retrieved_states.empty());
    REQUIRE(fixture.model.requests.empty());
    REQUIRE(fixture.tools.definitions_calls == 0);
    REQUIRE(fixture.tools.executed_calls.empty());
}

TEST_CASE(end_turn_and_stop_sequence_are_the_only_text_completion_stops) {
    const std::vector<std::pair<agent::StopReason, std::string>> cases = {
        {agent::StopReason::EndTurn, "end_turn"},
        {agent::StopReason::StopSequence, "stop_sequence"},
    };
    for (const auto& item : cases) {
        test::EngineFixture fixture(
            test::FakeModel({fixtures::stopped_response(
                {agent::TextBlock{"fi"}, agent::TextBlock{"nal"}},
                item.first, item.second)}),
            test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

        const auto result = fixture.run(fixtures::run_request("finish"));

        REQUIRE(result.state.has_value());
        REQUIRE(!result.fatal_error.has_value());
        REQUIRE(result.state->status == agent::TaskStatus::Completed);
        REQUIRE(result.state->final_text ==
                std::optional<std::string>{"final"});
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 1);
        REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 1);
    }
}

TEST_CASE(engine_rejects_mismatched_canonical_and_raw_stop_reasons) {
    struct InvalidCase {
        agent::StopReason stop_reason;
        const char* raw_stop_reason;
        std::vector<agent::ContentBlock> content;
    };
    const std::vector<InvalidCase> invalid = {
        {agent::StopReason::MaxTokens, "end_turn", {}},
        {agent::StopReason::EndTurn, "tool_use",
         {agent::TextBlock{"done"}}},
        {agent::StopReason::ToolUse, "stop_sequence",
         {agent::ToolUseBlock{fixtures::call("call-1", "read")}}},
        {agent::StopReason::StopSequence, "max_tokens",
         {agent::TextBlock{"done"}}},
    };

    for (const auto& item : invalid) {
        test::EngineFixture fixture(
            test::FakeModel({fixtures::stopped_response(
                item.content, item.stop_reason, item.raw_stop_reason)}),
            test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

        const auto result = fixture.run(fixtures::run_request("forged stop"));

        REQUIRE(result.state.has_value());
        REQUIRE(!result.fatal_error.has_value());
        REQUIRE(result.state->status == agent::TaskStatus::Failed);
        REQUIRE(result.state->terminal_error.has_value());
        REQUIRE(result.state->terminal_error->code ==
                agent::ErrorCode::ProtocolFailure);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallFailed) == 1);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::ToolCallStarted) == 0);
        REQUIRE(fixture.tools.executed_calls.empty());
    }
}

TEST_CASE(tool_use_allows_ordered_text_and_tools_but_requires_a_tool_block) {
    test::EngineFixture allowed(
        test::FakeModel({
            fixtures::stopped_response(
                {agent::TextBlock{"checking"},
                 agent::ToolUseBlock{fixtures::call("call-1", "read")}},
                agent::StopReason::ToolUse, "tool_use"),
            fixtures::text_response("done")}),
        test::FakeTools({agent::ToolResult{"call-1", "contents", false}}),
        test::FakeKnowledge(agent::EvidencePack{}));

    const auto allowed_result =
        allowed.run(fixtures::run_request("inspect code"));

    REQUIRE(allowed_result.state.has_value());
    REQUIRE(allowed_result.state->status == agent::TaskStatus::Completed);
    REQUIRE(allowed.tools.executed_ids() ==
            std::vector<std::string>{"call-1"});
    REQUIRE(allowed.model.requests.at(1).messages.at(1).content.size() == 2);

    test::EngineFixture missing_tool(
        test::FakeModel({fixtures::stopped_response(
            {agent::TextBlock{"not actually using a tool"}},
            agent::StopReason::ToolUse, "tool_use")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

    const auto missing_result =
        missing_tool.run(fixtures::run_request("inspect code"));

    REQUIRE(missing_result.state.has_value());
    REQUIRE(missing_result.state->status == agent::TaskStatus::Failed);
    REQUIRE(missing_result.state->terminal_error.has_value());
    REQUIRE(missing_result.state->terminal_error->code ==
            agent::ErrorCode::ProtocolFailure);
    REQUIRE(missing_tool.events.count(agent::EventKind::ModelCallSucceeded) == 0);
    REQUIRE(missing_tool.events.kinds().back() ==
            agent::EventKind::ModelCallFailed);
}

TEST_CASE(max_tokens_without_tools_is_budget_exceeded_after_accepted_response) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::stopped_response(
            {agent::TextBlock{"partial"}}, agent::StopReason::MaxTokens,
            "max_tokens")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("long task"));
    const agent::RuntimeError expected{
        agent::ErrorCode::BudgetExceeded,
        "model output token budget exceeded", false};

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(!result.state->final_text.has_value());
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 0);
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_tokens");
}

TEST_CASE(empty_max_tokens_response_is_accepted_before_exact_budget_terminal) {
    test::EngineFixture fixture(
        test::FakeModel({fixtures::stopped_response(
            {}, agent::StopReason::MaxTokens, "max_tokens")}),
        test::FakeTools{}, test::FakeKnowledge(agent::EvidencePack{}));

    const auto result = fixture.run(fixtures::run_request("long task"));
    const agent::RuntimeError expected{
        agent::ErrorCode::BudgetExceeded,
        "model output token budget exceeded", false};

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::BudgetExceeded);
    REQUIRE(result.state->terminal_error ==
            std::optional<agent::RuntimeError>{expected});
    REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::ModelCallFailed) == 0);
    REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 1);
    REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 0);
    const auto* succeeded = std::get_if<agent::ModelCallSucceededPayload>(
        &fixture.events.events.at(4).payload);
    REQUIRE(succeeded != nullptr);
    REQUIRE(succeeded->response.content.empty());
    const auto* exceeded = std::get_if<agent::TaskBudgetExceededPayload>(
        &fixture.events.events.back().payload);
    REQUIRE(exceeded != nullptr);
    REQUIRE(exceeded->budget_name == "max_tokens");
    REQUIRE(exceeded->error == expected);
}

TEST_CASE(model_tool_result_blocks_fail_before_success_or_terminal_routing) {
    const auto call = fixtures::call("call-1", "read");
    const agent::ToolResultBlock tool_result{
        {"call-1", "untrusted provider tool result", false}};
    const std::vector<agent::ModelResponse> invalid_responses = {
        fixtures::stopped_response(
            {agent::TextBlock{"text"}, tool_result},
            agent::StopReason::EndTurn, "end_turn"),
        fixtures::stopped_response(
            {agent::TextBlock{"text"}, tool_result},
            agent::StopReason::StopSequence, "stop_sequence"),
        fixtures::stopped_response(
            {agent::TextBlock{"partial"}, tool_result},
            agent::StopReason::MaxTokens, "max_tokens"),
        fixtures::stopped_response(
            {agent::ToolUseBlock{call}, tool_result},
            agent::StopReason::ToolUse, "tool_use"),
    };
    const agent::RuntimeError expected{
        agent::ErrorCode::ProtocolFailure,
        "model response contains an invalid tool-result block", false};

    for (const auto& response : invalid_responses) {
        test::EngineFixture fixture(
            test::FakeModel({response}), test::FakeTools{},
            test::FakeKnowledge(agent::EvidencePack{}));

        const auto result = fixture.run(fixtures::run_request("protocol"));

        REQUIRE(result.state.has_value());
        REQUIRE(!result.fatal_error.has_value());
        REQUIRE(result.state->status == agent::TaskStatus::Failed);
        REQUIRE(result.state->terminal_error ==
                std::optional<agent::RuntimeError>{expected});
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallFailed) == 1);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::TaskBudgetExceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::ToolCallStarted) == 0);
        REQUIRE(fixture.events.kinds().back() ==
                agent::EventKind::ModelCallFailed);
        const auto* failure = std::get_if<agent::ModelCallFailedPayload>(
            &fixture.events.events.back().payload);
        REQUIRE(failure != nullptr);
        REQUIRE(failure->error == expected);
        REQUIRE(fixture.knowledge.retrieved_states.size() == 1);
        REQUIRE(fixture.tools.definitions_calls == 1);
        REQUIRE(fixture.model.requests.size() == 1);
        REQUIRE(fixture.tools.executed_calls.empty());
    }
}

TEST_CASE(inconsistent_stop_and_content_rows_fail_directly_as_model_protocol) {
    const auto call = fixtures::call("call-1", "read");
    const std::vector<agent::ModelResponse> invalid_responses = {
        fixtures::stopped_response({agent::TextBlock{"unknown"}},
                                   agent::StopReason::Unknown,
                                   "future_stop"),
        fixtures::stopped_response({}, agent::StopReason::ToolUse,
                                   "tool_use"),
        fixtures::stopped_response({agent::ToolUseBlock{call}},
                                   agent::StopReason::EndTurn,
                                   "end_turn"),
        fixtures::stopped_response({agent::ToolUseBlock{call}},
                                   agent::StopReason::StopSequence,
                                   "stop_sequence"),
        fixtures::stopped_response({agent::ToolUseBlock{call}},
                                   agent::StopReason::MaxTokens,
                                   "max_tokens"),
        fixtures::stopped_response({}, agent::StopReason::EndTurn,
                                   "end_turn"),
        fixtures::stopped_response({}, agent::StopReason::StopSequence,
                                   "stop_sequence"),
        fixtures::stopped_response({agent::TextBlock{""}},
                                   agent::StopReason::EndTurn,
                                   "end_turn"),
        fixtures::stopped_response({agent::TextBlock{""}},
                                   agent::StopReason::StopSequence,
                                   "stop_sequence"),
    };

    for (const auto& response : invalid_responses) {
        test::EngineFixture fixture(
            test::FakeModel({response}), test::FakeTools{},
            test::FakeKnowledge(agent::EvidencePack{}));

        const auto result = fixture.run(fixtures::run_request("protocol"));

        REQUIRE(result.state.has_value());
        REQUIRE(!result.fatal_error.has_value());
        REQUIRE(result.state->status == agent::TaskStatus::Failed);
        REQUIRE(result.state->terminal_error.has_value());
        REQUIRE(result.state->terminal_error->code ==
                agent::ErrorCode::ProtocolFailure);
        REQUIRE(fixture.events.count(agent::EventKind::ModelCallSucceeded) == 0);
        REQUIRE(fixture.events.count(agent::EventKind::TaskCompleted) == 0);
        REQUIRE(fixture.events.kinds().back() ==
                agent::EventKind::ModelCallFailed);
        REQUIRE(fixture.tools.executed_calls.empty());
    }
}
