#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/http_transport.h"
#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/empty/empty_tool_gateway.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/rag/python_rag_knowledge_provider.h"
#include "adapters/tools/composite_tool_gateway.h"
#include "adapters/workspace/workspace_text.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "cli/cli_app.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/model_client.h"
#include "ports/process_runner.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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
        const agent::ModelRequest& request) override {
        requests.push_back(request);
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

class ScriptedProcessRunner final : public agent::ProcessRunner {
public:
    explicit ScriptedProcessRunner(std::vector<agent::ProcessOutput> outputs)
        : outputs_(std::move(outputs)) {}

    agent::Result<agent::ProcessOutput> run(
        const agent::ProcessRequest& request) override {
        requests.push_back(request);
        if (next_ >= outputs_.size()) {
            return agent::Result<agent::ProcessOutput>::failure(
                {agent::ErrorCode::ProtocolFailure,
                 "scripted process output exhausted", false});
        }
        return agent::Result<agent::ProcessOutput>::success(outputs_.at(next_++));
    }

    std::vector<agent::ProcessRequest> requests;

private:
    std::vector<agent::ProcessOutput> outputs_;
    std::size_t next_{0};
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

    RealRuntimeFixture(agent::ModelClient& model,
                       agent::ToolGateway& external_tools,
                       agent::JsonlEventStore& store)
        : engine(model, external_tools, knowledge, store, clock, ids,
                 cancellation) {}

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

agent::ToolCall list_call(std::string id, std::string path) {
    return {std::move(id), "list_files",
            agent::Value::object({
                {"path", std::move(path)},
                {"recursive", false},
                {"max_results", std::int64_t{100}}})};
}

agent::ToolCall read_call(std::string id,
                          std::string path,
                          std::int64_t start_line,
                          std::int64_t max_lines) {
    return {std::move(id), "read_file",
            agent::Value::object({
                {"path", std::move(path)},
                {"start_line", start_line},
                {"max_lines", max_lines}})};
}

agent::ToolCall replace_call(std::string id,
                             std::string path,
                             std::string old_text,
                             std::string new_text,
                             std::int64_t occurrences,
                             std::string hash) {
    return {std::move(id), "replace_text",
            agent::Value::object({
                {"path", std::move(path)},
                {"old_text", std::move(old_text)},
                {"new_text", std::move(new_text)},
                {"expected_occurrences", occurrences},
                {"expected_sha256", std::move(hash)}})};
}

agent::ToolCall build_call(std::string id) {
    return {std::move(id), "build_project",
            agent::Value::object({{"configuration", "Debug"},
                                  {"target", "agent_tests"}})};
}

agent::ModelResponse tool_response(agent::ToolCall call,
                                   std::string request_id) {
    return {{agent::ToolUseBlock{std::move(call)}},
            agent::StopReason::ToolUse, "tool_use", 5, 3,
            std::move(request_id)};
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

TEST_CASE(real_workspace_gateway_completes_versioned_file_workflow) {
    test::ScopedTempDir root("runtime-real-workspace-tools");
    const auto workspace = root.path() / "workspace";
    const auto runtime_root = root.path() / "runtime_data";
    const auto outside = root.path() / "outside.txt";
    std::filesystem::create_directories(workspace / ".git");
    std::filesystem::create_directories(runtime_root);
    {
        std::ofstream(workspace / "answer.txt", std::ios::binary) << "answer=41\n";
        std::ofstream(workspace / ".git/sentinel", std::ios::binary)
            << "git-sentinel\n";
        std::ofstream(runtime_root / "sentinel", std::ios::binary)
            << "runtime-sentinel\n";
        std::ofstream(outside, std::ios::binary) << "outside-sentinel\n";
    }
    const auto original_hash = agent::workspace::sha256_hex("answer=41\n");
    test::FakeModel model({
        fixtures::tool_response(fixtures::list_call("call-list", "."),
                                "provider-request-list"),
        fixtures::tool_response(
            fixtures::read_call("call-read-before", "answer.txt", 1, 20),
            "provider-request-read-before"),
        fixtures::tool_response(
            fixtures::replace_call("call-replace", "answer.txt", "41", "42",
                                   1, original_hash),
            "provider-request-replace"),
        fixtures::tool_response(
            fixtures::read_call("call-read-after", "answer.txt", 1, 20),
            "provider-request-read-after"),
        fixtures::text_response(u8"已把答案改为 42，并重新读取验证。")});
    agent::WorkspaceToolGateway tools(runtime_root);
    agent::JsonlEventStore store(runtime_root);
    test::RealRuntimeFixture runtime(model, tools, store);

    const auto result = runtime.run(
        {u8"把答案从 41 改为 42，并重新读取验证。",
         workspace.generic_u8string(), u8"你是编码代理。",
         {5, 4, 30'000, 5'000}});

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->final_text ==
            std::optional<std::string>{u8"已把答案改为 42，并重新读取验证。"});
    REQUIRE(fixtures::read_all(workspace / "answer.txt") == "answer=42\n");
    REQUIRE(fixtures::read_all(workspace / ".git/sentinel") ==
            "git-sentinel\n");
    REQUIRE(fixtures::read_all(runtime_root / "sentinel") ==
            "runtime-sentinel\n");
    REQUIRE(fixtures::read_all(outside) == "outside-sentinel\n");

    const auto event_path = store.event_path(result.state->task_id);
    REQUIRE(event_path.has_value());
    const auto loaded = store.read_file(event_path.value());
    REQUIRE(loaded.has_value());
    std::vector<std::string> observed_tools;
    for (const auto& event : loaded.value()) {
        if (const auto* started =
                std::get_if<agent::ToolCallStartedPayload>(&event.payload)) {
            observed_tools.push_back(started->call.name);
        }
    }
    REQUIRE((observed_tools ==
             std::vector<std::string>{"list_files", "read_file",
                                      "replace_text", "read_file"}));
    const auto replayed = agent::replay_events(loaded.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == *result.state);

    REQUIRE(model.requests.size() == 5);
    for (const auto& request : model.requests) {
        REQUIRE(request.tools.size() == 5);
        REQUIRE(request.tools.at(0).name == "list_files");
        REQUIRE(request.tools.at(1).name == "read_file");
        REQUIRE(request.tools.at(2).name == "search_text");
        REQUIRE(request.tools.at(3).name == "replace_text");
        REQUIRE(request.tools.at(4).name == "write_file");
    }
}

TEST_CASE(production_equivalent_composition_exposes_five_or_eight_tools) {
    test::ScopedTempDir temp("runtime-tool-composition");
    agent::WorkspaceToolGateway files(temp.path() / "runtime_data");
    test::ScriptedProcessRunner process({});
    agent::CMakeToolGateway builds(process, 300'000);

    for (const bool rag_enabled : {false, true}) {
        std::unique_ptr<agent::KnowledgeProvider> knowledge;
        if (rag_enabled) {
            knowledge = std::make_unique<agent::PythonRagKnowledgeProvider>(
                process, agent::PythonRagConfig{
                             "python", "C:/trusted/agent_rag_cli.py",
                             "C:/trusted/knowledge.sqlite3", 5, 10});
        } else {
            knowledge = std::make_unique<agent::EmptyKnowledgeProvider>();
            const auto empty = knowledge->retrieve(agent::TaskState{});
            REQUIRE(empty.has_value());
            REQUIRE(empty.value().items.empty());
        }
        REQUIRE(process.requests.empty());

        for (const bool build_enabled : {false, true}) {
            std::vector<std::reference_wrapper<agent::ToolGateway>> gateways{
                files};
            if (build_enabled) {
                gateways.push_back(builds);
            }
            agent::CompositeToolGateway composite(std::move(gateways));
            const auto definitions = composite.definitions();
            const std::vector<std::string> expected =
                build_enabled
                    ? std::vector<std::string>{
                          "list_files", "read_file", "search_text",
                          "replace_text", "write_file", "configure_project",
                          "build_project", "run_tests"}
                    : std::vector<std::string>{
                          "list_files", "read_file", "search_text",
                          "replace_text", "write_file"};
            REQUIRE(definitions.size() == expected.size());
            for (std::size_t index = 0; index < expected.size(); ++index) {
                REQUIRE(definitions.at(index).name == expected.at(index));
            }
        }
    }
}

TEST_CASE(runtime_persists_failed_build_result_then_successful_retry) {
    test::ScopedTempDir root("runtime-build-retry");
    const auto workspace = root.path() / "workspace";
    const auto runtime_root = root.path() / "runtime_data";
    std::filesystem::create_directories(workspace);
    test::ScriptedProcessRunner process({
        {1, false, 11, "", "first build failed", false, false},
        {0, false, 9, "second build passed", "", false, false}});
    agent::WorkspaceToolGateway files(runtime_root);
    agent::CMakeToolGateway builds(process, 300'000);
    agent::CompositeToolGateway tools({files, builds});
    agent::JsonlEventStore store(runtime_root);
    test::FakeModel model({
        fixtures::tool_response(fixtures::build_call("call-build-fail"),
                                "provider-request-build-fail"),
        fixtures::tool_response(fixtures::build_call("call-build-pass"),
                                "provider-request-build-pass"),
        fixtures::text_response("build verified")});
    test::RealRuntimeFixture runtime(model, tools, store);

    const auto result = runtime.run(
        {"build and test the project", workspace.generic_u8string(),
         "inspect, edit, and verify", {4, 2, 30'000, 5'000}});

    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->final_text ==
            std::optional<std::string>{"build verified"});
    REQUIRE(process.requests.size() == 2);
    REQUIRE(model.requests.size() == 3);
    for (const auto& request : model.requests) {
        REQUIRE(request.tools.size() == 8);
    }

    const auto* failed_result = std::get_if<agent::ToolResultBlock>(
        &model.requests.at(1).messages.back().content.front());
    REQUIRE(failed_result != nullptr);
    REQUIRE(failed_result->result.is_error);
    REQUIRE(nlohmann::json::parse(failed_result->result.content)
                .at("exit_code") == 1);
    const auto* passed_result = std::get_if<agent::ToolResultBlock>(
        &model.requests.at(2).messages.back().content.front());
    REQUIRE(passed_result != nullptr);
    REQUIRE(!passed_result->result.is_error);
    REQUIRE(nlohmann::json::parse(passed_result->result.content)
                .at("exit_code") == 0);

    const auto event_path = store.event_path(result.state->task_id);
    REQUIRE(event_path.has_value());
    const auto loaded = store.read_file(event_path.value());
    REQUIRE(loaded.has_value());
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    for (const auto& event : loaded.value()) {
        if (std::holds_alternative<agent::ToolCallSucceededPayload>(
                event.payload)) {
            ++succeeded;
        }
        if (std::holds_alternative<agent::ToolCallFailedPayload>(event.payload)) {
            ++failed;
        }
    }
    REQUIRE(succeeded == 2);
    REQUIRE(failed == 0);
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
