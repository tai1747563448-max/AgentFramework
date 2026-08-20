#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/http_transport.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/empty/empty_tool_gateway.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "cli/cli_app.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/model_client.h"
#include "test_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace test {

class FakeModel final : public agent::ModelClient {
public:
    explicit FakeModel(std::vector<agent::ModelResponse> responses)
        : responses_(std::move(responses)) {}

    agent::Result<agent::ModelResponse> complete(
        const agent::ModelRequest&) override {
        if (next_response_ >= responses_.size()) {
            return agent::Result<agent::ModelResponse>::failure(
                {agent::ErrorCode::ProtocolFailure,
                 "fake model response script exhausted", false});
        }
        return agent::Result<agent::ModelResponse>::success(
            responses_.at(next_response_++));
    }

private:
    std::vector<agent::ModelResponse> responses_;
    std::size_t next_response_{0};
};

class FakeClock final : public agent::Clock {
public:
    std::string now_utc() const override {
        return "2026-08-18T00:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        return 1'000;
    }
};

class FakeIds final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return "task-00000000000000000000000000000009";
    }

    std::string next_correlation_id() override {
        return "corr-" + std::to_string(next_correlation_++);
    }

private:
    std::size_t next_correlation_{1};
};

class FakeCancellation final : public agent::Cancellation {
public:
    bool requested() const noexcept override {
        return false;
    }
};

class SecretFailingHttp final : public agent::HttpTransport {
public:
    explicit SecretFailingHttp(std::string sentinel)
        : sentinel_(std::move(sentinel)) {}

    agent::Result<agent::HttpResponse> post(
        const agent::HttpRequest& request) override {
        ++post_calls;
        const auto credential = request.headers.find("x-api-key");
        received_expected_credential =
            credential != request.headers.end() && credential->second == sentinel_;
        return agent::Result<agent::HttpResponse>::failure(
            {agent::ErrorCode::TransportFailure,
             "transport exposed " + sentinel_, true});
    }

    bool received_expected_credential{false};
    std::size_t post_calls{0};

private:
    std::string sentinel_;
};

struct RealRuntimeFixture {
    agent::EmptyToolGateway tools;
    agent::EmptyKnowledgeProvider knowledge;
    FakeClock clock;
    FakeIds ids;
    FakeCancellation cancellation;
    agent::RuntimeEngine engine;

    RealRuntimeFixture(agent::ModelClient& model,
                       agent::JsonlEventStore& store)
        : engine(model, tools, knowledge, store, clock, ids, cancellation) {}

    RealRuntimeFixture(const RealRuntimeFixture&) = delete;
    RealRuntimeFixture& operator=(const RealRuntimeFixture&) = delete;
    RealRuntimeFixture(RealRuntimeFixture&&) = delete;
    RealRuntimeFixture& operator=(RealRuntimeFixture&&) = delete;

    agent::RuntimeResult run(
        const agent::RunRequest& request,
        const agent::RuntimeProgressObserver& observer = {}) {
        return engine.run(request, observer);
    }
};

}  // namespace test

namespace fixtures {

agent::RuntimeBudgets budgets() {
    return {4, 4, 30'000, 5'000};
}

agent::RunRequest run_request(std::string issue,
                              const std::filesystem::path& workspace) {
    return {std::move(issue), workspace.generic_u8string(),
            u8"你是编码代理。", budgets()};
}

agent::ModelResponse text_response(std::string text) {
    return {{agent::TextBlock{std::move(text)}}, agent::StopReason::EndTurn,
            "end_turn", 5, 3, "provider-request-integration"};
}

test::RealRuntimeFixture real_runtime_with(
    agent::ModelClient& model,
    agent::JsonlEventStore& store) {
    return test::RealRuntimeFixture(model, store);
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

}  // namespace fixtures

TEST_CASE(fake_end_to_end_writes_and_replays_unicode_task) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"运行时-回放"));
    agent::JsonlEventStore store(temp.path());
    test::FakeModel model({fixtures::text_response(u8"已完成")});
    auto runtime = fixtures::real_runtime_with(model, store);

    const auto result = runtime.run(
        fixtures::run_request(u8"修复 C++ 警告", temp.path()));

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->final_text == std::optional<std::string>{u8"已完成"});
    const auto event_path = store.event_path(result.state->task_id);
    REQUIRE(event_path.has_value());
    const auto loaded = store.read_file(event_path.value());
    REQUIRE(loaded.has_value());
    const auto replayed = agent::replay_events(loaded.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == *result.state);
}

TEST_CASE(cli_progress_exactly_matches_real_runtime_jsonl_durable_order) {
    constexpr const char* kTaskId =
        "task-00000000000000000000000000000009";
    const std::string issue_sentinel = "SENTINEL_PRIVATE_ISSUE";
    const std::string workspace_sentinel = "SENTINEL_PRIVATE_WORKSPACE";
    const std::string metadata_sentinel = "SENTINEL_PROVIDER_METADATA";
    test::ScopedTempDir temp(std::filesystem::u8path(u8"运行时-CLI-进度"));
    agent::JsonlEventStore store(temp.path());
    auto response = fixtures::text_response(u8"已完成🙂");
    response.provider_request_id = metadata_sentinel;
    test::FakeModel model({std::move(response)});
    auto runtime = fixtures::real_runtime_with(model, store);
    std::optional<agent::RuntimeResult> captured_result;
    std::ostringstream output;
    std::ostringstream error;
    agent::CliApp cli(
        [&](const agent::RunRequest& request,
            const agent::RuntimeProgressObserver& observer) {
            captured_result = runtime.run(
                {request.issue, request.workspace_utf8, u8"你是编码代理。",
                 fixtures::budgets()},
                observer);
            return *captured_result;
        },
        [&](const std::filesystem::path& path) {
            const auto loaded = store.read_file(path);
            if (!loaded.has_value()) {
                return agent::Result<agent::TaskState>::failure(loaded.error());
            }
            return agent::replay_events(loaded.value());
        },
        output, error);

    const auto code = cli.execute(
        {"agent", "run", "--workspace", workspace_sentinel,
         "--issue", issue_sentinel});

    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(captured_result.has_value());
    REQUIRE(captured_result->state.has_value());
    REQUIRE(!captured_result->fatal_error.has_value());
    REQUIRE(captured_result->state->task_id == kTaskId);
    REQUIRE(captured_result->state->status == agent::TaskStatus::Completed);
    const auto event_path = store.event_path(kTaskId);
    REQUIRE(event_path.has_value());
    const auto loaded = store.read_file(event_path.value());
    REQUIRE(loaded.has_value());
    const std::vector<agent::EventKind> expected_kinds{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::TaskCompleted};
    REQUIRE(loaded.value().size() == expected_kinds.size());
    for (std::size_t index = 0; index < loaded.value().size(); ++index) {
        REQUIRE(loaded.value()[index].task_id == kTaskId);
        REQUIRE(loaded.value()[index].sequence == index + 1);
        REQUIRE(agent::event_kind(loaded.value()[index].payload) ==
                expected_kinds[index]);
    }
    const auto replayed = agent::replay_events(loaded.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == *captured_result->state);
    REQUIRE(output.str() ==
            "task_id=task-00000000000000000000000000000009 sequence=1 "
            "event=TaskStarted status=Created\n"
            "task_id=task-00000000000000000000000000000009 sequence=2 "
            "event=ContextPreparationStarted status=PreparingContext\n"
            "task_id=task-00000000000000000000000000000009 sequence=3 "
            "event=ContextPrepared status=AwaitingModel\n"
            "task_id=task-00000000000000000000000000000009 sequence=4 "
            "event=ModelCallStarted status=AwaitingModel\n"
            "task_id=task-00000000000000000000000000000009 sequence=5 "
            "event=ModelCallSucceeded status=AwaitingModel\n"
            "task_id=task-00000000000000000000000000000009 sequence=6 "
            "event=TaskCompleted status=Completed\n"
            "已完成🙂\n");
    REQUIRE(error.str().empty());
    REQUIRE(output.str().find(issue_sentinel) == std::string::npos);
    REQUIRE(output.str().find(workspace_sentinel) == std::string::npos);
    REQUIRE(output.str().find(metadata_sentinel) == std::string::npos);
}

TEST_CASE(provider_secret_never_reaches_cli_events_or_errors) {
    const std::string sentinel = "TASK9_SECRET_SENTINEL_DO_NOT_PRINT";
    test::ScopedTempDir temp(std::filesystem::u8path(u8"运行时-秘密边界"));
    agent::JsonlEventStore store(temp.path());
    test::SecretFailingHttp http(sentinel);
    agent::AnthropicMessagesClient model(
        {"https://provider.example", "model-id", agent::CredentialKind::ApiKey,
         sentinel, "2023-06-01", 128},
        http);
    auto runtime = fixtures::real_runtime_with(model, store);
    std::optional<agent::RuntimeResult> captured_result;
    std::ostringstream output;
    std::ostringstream error;
    agent::CliApp cli(
        [&](const agent::RunRequest& request,
            const agent::RuntimeProgressObserver& observer) {
            captured_result = runtime.run(
                {request.issue, request.workspace_utf8, u8"你是编码代理。",
                 fixtures::budgets()},
                observer);
            return *captured_result;
        },
        [&](const std::filesystem::path& path) {
            const auto loaded = store.read_file(path);
            if (!loaded.has_value()) {
                return agent::Result<agent::TaskState>::failure(loaded.error());
            }
            return agent::replay_events(loaded.value());
        },
        output, error);

    const auto code = cli.execute(
        {"agent", "run", "--workspace", temp.path().generic_u8string(),
         "--issue", u8"验证秘密边界"});

    REQUIRE(code == agent::ExitCode::TaskFailed);
    REQUIRE(http.post_calls == 1);
    REQUIRE(http.received_expected_credential);
    REQUIRE(captured_result.has_value());
    REQUIRE(captured_result->state.has_value());
    REQUIRE(!captured_result->fatal_error.has_value());
    REQUIRE(captured_result->state->status == agent::TaskStatus::Failed);
    REQUIRE(captured_result->state->terminal_error.has_value());
    REQUIRE(captured_result->state->terminal_error->message.find(sentinel) ==
            std::string::npos);
    const auto event_path = store.event_path(captured_result->state->task_id);
    REQUIRE(event_path.has_value());
    const auto persisted = fixtures::read_all(event_path.value());
    REQUIRE(!persisted.empty());
    REQUIRE(output.str().find(sentinel) == std::string::npos);
    REQUIRE(error.str().find(sentinel) == std::string::npos);
    REQUIRE(persisted.find(sentinel) == std::string::npos);
}
