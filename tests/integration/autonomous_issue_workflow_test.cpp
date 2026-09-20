#include "adapters/build/cmake_tool_gateway.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/process/direct_process_runner.h"
#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/tools/composite_tool_gateway.h"
#include "adapters/workspace/workspace_text.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "application/task_evaluator.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/model_client.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace test {

class FakeClock final : public agent::Clock {
public:
    std::string now_utc() const override {
        return "2026-08-21T12:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        return 1'000;
    }
};

class FixedIds final : public agent::IdGenerator {
public:
    explicit FixedIds(std::string task_id) : task_id_(std::move(task_id)) {}

    std::string next_task_id() override {
        return task_id_;
    }

    std::string next_correlation_id() override {
        return "corr-workflow-" + std::to_string(next_correlation_++);
    }

private:
    std::string task_id_;
    std::size_t next_correlation_{1};
};

class NeverCancelled final : public agent::Cancellation {
public:
    bool is_cancelled() const noexcept override { return false; }
    void cancel() noexcept override {}
};

class FailOneAppendStore final : public agent::EventStore {
public:
    FailOneAppendStore(agent::JsonlEventStore& durable,
                       std::size_t fail_on_append)
        : durable_(durable), fail_on_append_(fail_on_append) {}

    agent::Result<void> append(const agent::RuntimeEvent& event) override {
        ++append_attempts;
        if (append_attempts == fail_on_append_) {
            return agent::Result<void>::failure(
                {agent::ErrorCode::PersistenceFailure,
                 "scripted post-effect append failure", true});
        }
        return durable_.append(event);
    }

    agent::Result<std::vector<agent::RuntimeEvent>> read_file(
        const std::filesystem::path& path) const override {
        return durable_.read_file(path);
    }

    std::size_t append_attempts{0};

private:
    agent::JsonlEventStore& durable_;
    std::size_t fail_on_append_;
};

class ScriptedCodingModel final : public agent::ModelClient {
public:
    ScriptedCodingModel(std::size_t first_step, std::string original_hash)
        : step_(first_step), original_hash_(std::move(original_hash)) {}

    agent::Result<agent::ModelResponse> complete(
        const agent::ModelRequest& request) override {
        requests.push_back(request);
        REQUIRE(request.evidence.items.empty());
        REQUIRE(request.tools.size() == 8);

        switch (step_++) {
        case 0:
            return success(tool_response(read_call("call-read-before"),
                                         "provider-read-before"));
        case 1: {
            const auto& result = last_tool_result(request);
            REQUIRE(!result.is_error);
            const auto json = nlohmann::json::parse(result.content);
            REQUIRE(json.at("sha256") == original_hash_);
            REQUIRE(json.at("content").get<std::string>().find(
                        "left - right") != std::string::npos);
            return success(tool_response(
                replace_call("call-fix", original_hash_), "provider-fix"));
        }
        case 2: {
            const auto& result = last_tool_result(request);
            REQUIRE(result.is_error);
            const auto json = nlohmann::json::parse(result.content);
            REQUIRE(json.at("error").at("code") == "conflict");
            saw_recovery_conflict = true;
            return success(tool_response(read_call("call-read-reconciled"),
                                         "provider-read-reconciled"));
        }
        case 3: {
            const auto& result = last_tool_result(request);
            REQUIRE(!result.is_error);
            const auto json = nlohmann::json::parse(result.content);
            REQUIRE(json.at("sha256") ==
                    agent::workspace::sha256_hex(fixed_source()));
            REQUIRE(json.at("content").get<std::string>().find(
                        "left + right") != std::string::npos);
            return success(tool_response(configure_call("call-configure"),
                                         "provider-configure"));
        }
        case 4:
            require_process_result(request, "configure");
            return success(tool_response(build_call("call-build"),
                                         "provider-build"));
        case 5:
            require_process_result(request, "build");
            return success(tool_response(test_call("call-test"),
                                         "provider-test"));
        case 6:
            require_process_result(request, "test");
            saw_passing_test = true;
            return success(final_response());
        default:
            return agent::Result<agent::ModelResponse>::failure(
                {agent::ErrorCode::ProtocolFailure,
                 "scripted model exhausted", false});
        }
    }

    static std::string broken_source() {
        return "#include \"calculator.h\"\n"
               "int add(int left, int right) {\n"
               "    return left - right;\n"
               "}\n";
    }

    static std::string fixed_source() {
        return "#include \"calculator.h\"\n"
               "int add(int left, int right) {\n"
               "    return left + right;\n"
               "}\n";
    }

    std::vector<agent::ModelRequest> requests;
    bool saw_recovery_conflict{false};
    bool saw_passing_test{false};

private:
    static agent::Result<agent::ModelResponse> success(
        agent::ModelResponse response) {
        return agent::Result<agent::ModelResponse>::success(
            std::move(response));
    }

    static agent::ToolCall read_call(std::string id) {
        return {std::move(id), "read_file",
                agent::Value::object(
                    {{"path", "calculator.cpp"},
                     {"start_line", std::int64_t{1}},
                     {"max_lines", std::int64_t{100}}})};
    }

    static agent::ToolCall replace_call(std::string id,
                                        const std::string& hash) {
        return {std::move(id), "replace_text",
                agent::Value::object(
                    {{"path", "calculator.cpp"},
                     {"old_text", "left - right"},
                     {"new_text", "left + right"},
                     {"expected_occurrences", std::int64_t{1}},
                     {"expected_sha256", hash}})};
    }

    static agent::ToolCall configure_call(std::string id) {
        return {std::move(id), "configure_project",
                agent::Value::object({{"configuration", "Debug"}})};
    }

    static agent::ToolCall build_call(std::string id) {
        return {std::move(id), "build_project",
                agent::Value::object(
                    {{"configuration", "Debug"},
                     {"target", "calculator_tests"}})};
    }

    static agent::ToolCall test_call(std::string id) {
        return {std::move(id), "run_tests",
                agent::Value::object(
                    {{"configuration", "Debug"},
                     {"test_name", "calculator.correct"}})};
    }

    static agent::ModelResponse tool_response(agent::ToolCall call,
                                              std::string request_id) {
        return {{agent::ToolUseBlock{std::move(call)}},
                agent::StopReason::ToolUse, 20, 8,
                std::move(request_id)};
    }

    static agent::ModelResponse final_response() {
        return {{agent::TextBlock{
                     "Fixed calculator addition and verified calculator.correct."}},
                agent::StopReason::EndTurn, 30, 10,
                "provider-final"};
    }

    static const agent::ToolResult& last_tool_result(
        const agent::ModelRequest& request) {
        REQUIRE(!request.messages.empty());
        const auto& message = request.messages.back();
        REQUIRE(message.role == agent::Role::User);
        REQUIRE(message.content.size() == 1);
        const auto* block =
            std::get_if<agent::ToolResultBlock>(&message.content.front());
        REQUIRE(block != nullptr);
        return block->result;
    }

    static void require_process_result(const agent::ModelRequest& request,
                                       const char* operation) {
        const auto& result = last_tool_result(request);
        const auto json = nlohmann::json::parse(result.content);
        REQUIRE(json.at("operation") == operation);
        if (result.is_error) {
            throw std::runtime_error(std::string("unexpected ") + operation +
                                     " failure: " + result.content);
        }
        REQUIRE(json.at("exit_code") == 0);
        REQUIRE(json.at("timed_out") == false);
    }

    std::size_t step_;
    std::string original_hash_;
};

}  // namespace test

namespace fixtures {

constexpr const char* kTaskId =
    "task-0000000000000000000000000000000e";

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

agent::ToolCall build_tool_call(std::string id,
                                std::string name,
                                agent::Value::Object arguments) {
    return {std::move(id), std::move(name),
            agent::Value::object(std::move(arguments))};
}

std::size_t count_kind(const std::vector<agent::RuntimeEvent>& events,
                       agent::EventKind kind) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (agent::event_kind(event.payload) == kind) {
            ++count;
        }
    }
    return count;
}

}  // namespace fixtures

TEST_CASE(real_issue_workflow_recovers_after_a_post_edit_persistence_crash) {
    test::ScopedTempDir root(
        std::filesystem::u8path(u8"agent-自主工作流"));
    const auto workspace = root.path() / std::filesystem::u8path(u8"工作区");
    const auto runtime_root = root.path() / "runtime";
    const auto outside = root.path() / "outside-sentinel.txt";
    std::filesystem::create_directories(workspace / ".git");
    std::filesystem::create_directories(runtime_root);

    root.write_text(
        std::filesystem::relative(workspace / "CMakeLists.txt", root.path()),
        "cmake_minimum_required(VERSION 3.21)\n"
        "project(CalculatorRecovery LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 17)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "add_executable(calculator_tests calculator_test.cpp calculator.cpp)\n"
        "enable_testing()\n"
        "add_test(NAME calculator.correct COMMAND calculator_tests)\n");
    root.write_text(
        std::filesystem::relative(workspace / "calculator.h", root.path()),
        "#pragma once\n"
        "int add(int left, int right);\n");
    root.write_text(
        std::filesystem::relative(workspace / "calculator.cpp", root.path()),
        test::ScriptedCodingModel::broken_source());
    root.write_text(
        std::filesystem::relative(workspace / "calculator_test.cpp",
                                  root.path()),
        "#include \"calculator.h\"\n"
        "int main() { return add(20, 22) == 42 ? 0 : 1; }\n");
    root.write_text(
        std::filesystem::relative(workspace / ".git/sentinel", root.path()),
        "git-sentinel\n");
    root.write_text(
        std::filesystem::relative(runtime_root / "sentinel", root.path()),
        "runtime-sentinel\n");
    root.write_text("outside-sentinel.txt", "outside-sentinel\n");
    agent::DirectProcessRunner process;

    agent::CMakeToolGateway baseline_build(process, 120'000);
    const agent::ToolExecutionContext context{workspace.generic_u8string()};
    auto baseline = baseline_build.execute(
        fixtures::build_tool_call(
            "baseline-configure", "configure_project",
            {{"configuration", "Debug"}}),
        context);
    REQUIRE(baseline.has_value() && !baseline.value().is_error);
    baseline = baseline_build.execute(
        fixtures::build_tool_call(
            "baseline-build", "build_project",
            {{"configuration", "Debug"}, {"target", "calculator_tests"}}),
        context);
    REQUIRE(baseline.has_value() && !baseline.value().is_error);
    baseline = baseline_build.execute(
        fixtures::build_tool_call(
            "baseline-test", "run_tests",
            {{"configuration", "Debug"},
             {"test_name", "calculator.correct"}}),
        context);
    REQUIRE(baseline.has_value() && baseline.value().is_error);

    const auto original_hash = agent::workspace::sha256_hex(
        test::ScriptedCodingModel::broken_source());
    agent::JsonlEventStore durable(runtime_root);
    test::FailOneAppendStore interrupted_store(durable, 13);
    agent::WorkspaceToolGateway file_tools(runtime_root);
    agent::CMakeToolGateway build_tools(process, 120'000);
    agent::CompositeToolGateway tools({file_tools, build_tools});
    agent::EmptyKnowledgeProvider knowledge;
    test::ScriptedCodingModel first_model(0, original_hash);
    test::FakeClock first_clock;
    test::FixedIds first_ids(fixtures::kTaskId);
    test::NeverCancelled cancellation;
    agent::RuntimeEngine first_engine(
        first_model, tools, knowledge, interrupted_store, first_clock,
        first_ids, cancellation);
    const agent::RunRequest request{
        "Fix calculator.cpp add so 20 + 22 equals 42, then build and run "
        "calculator.correct.",
        workspace.generic_u8string(), "You are a careful coding agent.",
        {8, 8, 180'000, 30'000}};

    const auto interrupted = first_engine.run(request, {});

    REQUIRE(interrupted.state.has_value());
    REQUIRE(interrupted.fatal_error.has_value());
    REQUIRE(interrupted.fatal_error->code ==
            agent::ErrorCode::PersistenceFailure);
    REQUIRE(interrupted.state->status == agent::TaskStatus::AwaitingTool);
    REQUIRE(interrupted.state->active_tool_call_id ==
            std::optional<std::string>{"call-fix"});
    REQUIRE(interrupted.state->last_sequence == 12);
    REQUIRE(interrupted_store.append_attempts == 13);
    REQUIRE(fixtures::read_all(workspace / "calculator.cpp") ==
            test::ScriptedCodingModel::fixed_source());

    const auto event_path = durable.event_path(fixtures::kTaskId);
    REQUIRE(event_path.has_value());
    const auto durable_prefix = durable.read_file(event_path.value());
    REQUIRE(durable_prefix.has_value());
    REQUIRE(durable_prefix.value().size() == 12);
    REQUIRE(agent::event_kind(durable_prefix.value().back().payload) ==
            agent::EventKind::ToolCallStarted);

    agent::WorkspaceToolGateway resumed_file_tools(runtime_root);
    agent::CMakeToolGateway resumed_build_tools(process, 120'000);
    agent::CompositeToolGateway resumed_tools(
        {resumed_file_tools, resumed_build_tools});
    agent::EmptyKnowledgeProvider resumed_knowledge;
    test::ScriptedCodingModel resumed_model(2, original_hash);
    test::FakeClock resumed_clock;
    test::FixedIds resumed_ids(
        "task-ffffffffffffffffffffffffffffffff");
    agent::RuntimeEngine resumed_engine(
        resumed_model, resumed_tools, resumed_knowledge, durable,
        resumed_clock, resumed_ids, cancellation);

    const auto resumed = resumed_engine.resume(
        {durable_prefix.value(), "a changed prompt must not replace durable"},
        {});

    REQUIRE(resumed.state.has_value());
    REQUIRE(!resumed.fatal_error.has_value());
    REQUIRE(resumed.state->task_id == fixtures::kTaskId);
    REQUIRE(resumed.state->status == agent::TaskStatus::Completed);
    REQUIRE(resumed.state->final_text ==
            std::optional<std::string>{
                "Fixed calculator addition and verified calculator.correct."});
    REQUIRE(resumed.state->usage.model_rounds == 7);
    REQUIRE(resumed.state->usage.tool_calls == 6);
    REQUIRE(resumed_model.saw_recovery_conflict);
    REQUIRE(resumed_model.saw_passing_test);
    REQUIRE(resumed_model.requests.size() == 5);
    for (const auto& model_request : first_model.requests) {
        REQUIRE(model_request.evidence.items.empty());
    }
    for (const auto& model_request : resumed_model.requests) {
        REQUIRE(model_request.evidence.items.empty());
        REQUIRE(model_request.system_prompt ==
                "You are a careful coding agent.");
    }

    const auto final_events = durable.read_file(event_path.value());
    REQUIRE(final_events.has_value());
    REQUIRE(final_events.value().size() == 42);
    REQUIRE(fixtures::count_kind(final_events.value(),
                                 agent::EventKind::TaskStarted) == 1);
    REQUIRE(fixtures::count_kind(final_events.value(),
                                 agent::EventKind::ModelCallStarted) == 7);
    REQUIRE(fixtures::count_kind(final_events.value(),
                                 agent::EventKind::ToolCallStarted) == 6);
    for (std::size_t index_value = 0;
         index_value < final_events.value().size(); ++index_value) {
        REQUIRE(final_events.value()[index_value].sequence == index_value + 1);
        REQUIRE(final_events.value()[index_value].task_id == fixtures::kTaskId);
    }
    const auto replayed = agent::replay_events(final_events.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == *resumed.state);
    const auto evaluation =
        agent::evaluate_task_events(final_events.value());
    REQUIRE(evaluation.has_value());
    REQUIRE(evaluation.value().passed);
    REQUIRE(evaluation.value().model_rounds == 7);
    REQUIRE(evaluation.value().tool_calls == 6);
    REQUIRE(evaluation.value().evidence_rounds == 7);
    REQUIRE(evaluation.value().evidence_items == 0);
    REQUIRE(evaluation.value().model_requests_with_evidence == 0);
    REQUIRE(evaluation.value().tool_error_results == 1);

    REQUIRE(fixtures::read_all(workspace / "calculator.cpp") ==
            test::ScriptedCodingModel::fixed_source());
    REQUIRE(fixtures::read_all(workspace / ".git/sentinel") ==
            "git-sentinel\n");
    REQUIRE(fixtures::read_all(runtime_root / "sentinel") ==
            "runtime-sentinel\n");
    REQUIRE(fixtures::read_all(outside) == "outside-sentinel\n");
}
