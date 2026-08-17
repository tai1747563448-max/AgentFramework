#include "application/runtime_engine.h"
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
#include <string>
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

    std::vector<agent::ToolDefinition> definitions() const override {
        return definitions_;
    }

    agent::Result<agent::ToolResult> execute(const agent::ToolCall&) override {
        return agent::Result<agent::ToolResult>::failure(
            {agent::ErrorCode::DependencyUnavailable,
             "tool execution is outside the Task 5 path", false});
    }

private:
    std::vector<agent::ToolDefinition> definitions_;
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
    std::string now_utc() const override {
        return "2026-08-17T12:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        return 1'000;
    }
};

class FakeIds final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return "task-1";
    }

    std::string next_correlation_id() override {
        return "corr-" + std::to_string(next_correlation_++);
    }

private:
    std::size_t next_correlation_{1};
};

class FakeCancellation final : public agent::Cancellation {
public:
    explicit FakeCancellation(bool requested) : requested_(requested) {}

    bool requested() const noexcept override {
        return requested_;
    }

private:
    bool requested_;
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
                  Store events_value = Store{})
        : model(std::move(model_value)),
          tools(std::move(tools_value)),
          knowledge(std::move(knowledge_value)),
          events(std::move(events_value)),
          cancel(false),
          engine(model, tools, knowledge, events, clock, ids, cancel) {}

    EngineFixture(const EngineFixture&) = delete;
    EngineFixture& operator=(const EngineFixture&) = delete;
    EngineFixture(EngineFixture&&) = delete;
    EngineFixture& operator=(EngineFixture&&) = delete;

    agent::RuntimeResult run(const agent::RunRequest& request) {
        return engine.run(request);
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
