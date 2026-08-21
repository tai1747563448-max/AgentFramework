#include "adapters/json/value_json.h"
#include "adapters/persistence/event_json.h"
#include "adapters/persistence/jsonl_event_store.h"
#include "application/state_reducer.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
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
            std::move(task_id),
            "2026-08-17T12:00:00.000Z",
            "corr-" + std::to_string(sequence),
            std::move(payload)};
}

agent::RuntimeEvent task_started(const std::string& task_id,
                                 std::uint64_t sequence,
                                 const std::string& issue) {
    return event(task_id, sequence,
                 agent::TaskStartedPayload{issue, u8"E:/工作区/项目",
                                           {8, 12, 90'000, 30'000}});
}

agent::EvidencePack evidence() {
    return {{{u8"源码-一", u8"证据内容",
              agent::Value::object({
                  {"line", agent::Value(std::int64_t{7})},
                  {"scores", agent::Value::array(
                                 {agent::Value(0.75), agent::Value(true),
                                  agent::Value()})}})}}};
}

agent::ModelRequest request(std::string issue = "issue") {
    const auto path_schema =
        agent::Value::object({{"type", agent::Value("string")}});
    const auto properties =
        agent::Value::object({{"path", path_schema}});
    const auto input_schema = agent::Value::object(
        {{"type", agent::Value("object")}, {"properties", properties}});
    std::vector<agent::Message> messages = {
        {agent::Role::User, {agent::TextBlock{std::move(issue)}}}};
    std::vector<agent::ToolDefinition> tools = {
        {"read_file", u8"读取文件", input_schema}};
    return {u8"你是编码代理。", std::move(messages), std::move(tools),
            30'000, evidence()};
}

agent::ModelRequest complex_codec_request() {
    auto result = request();
    result.messages = {
        {agent::Role::System, {agent::TextBlock{u8"严格执行。"}}},
        {agent::Role::User,
         {agent::TextBlock{u8"检查警告"},
          agent::ToolResultBlock{{"prior-call", u8"旧结果", false}}}},
    };
    return result;
}

agent::ToolCall tool_call() {
    return {"call-1", "read_file",
            agent::Value::object({
                {"path", u8"src/入口.cpp"},
                {"line", agent::Value(std::int64_t{9})}})};
}

agent::ModelResponse response_with_tool() {
    return {{agent::TextBlock{u8"先读取"}, agent::ToolUseBlock{tool_call()}},
            agent::StopReason::ToolUse,
            "tool_use",
            123,
            45,
            "provider-request-一"};
}

std::vector<agent::RuntimeEvent> completed_text_trace(
    const std::string& task_id,
    const std::string& issue,
    const std::string& final_text) {
    const auto durable_task_id = valid_task_id(task_id);
    return {
        task_started(durable_task_id, 1, issue),
        event(durable_task_id, 2, agent::ContextPreparationStartedPayload{}),
        event(durable_task_id, 3, agent::ContextPreparedPayload{evidence()}),
        event(durable_task_id, 4,
              agent::ModelCallStartedPayload{request(issue)}),
        event(durable_task_id, 5,
              agent::ModelCallSucceededPayload{
                  {{agent::TextBlock{final_text}}, agent::StopReason::EndTurn,
                   "end_turn", 120, 12, "provider-request-final"}}),
        event(durable_task_id, 6, agent::TaskCompletedPayload{final_text}),
    };
}

std::string read_all(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string as_jsonl(const std::vector<agent::RuntimeEvent>& events) {
    std::string result;
    for (const auto& item : events) {
        result += agent::event_to_json(item).dump();
        result += '\n';
    }
    return result;
}

}  // namespace fixtures

TEST_CASE(jsonl_round_trip_preserves_replay_state_and_unicode) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"jsonl-中文路径"));
    agent::JsonlEventStore store(temp.path());
    const auto events = fixtures::completed_text_trace(
        "task-unicode", u8"修复警告", u8"完成");
    for (const auto& item : events) {
        REQUIRE(store.append(item).has_value());
    }

    const auto file = store.event_path(events.front().task_id);
    REQUIRE(file.has_value());
    const auto loaded = store.read_file(file.value());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == events);
    const auto state = agent::replay_events(loaded.value());
    REQUIRE(state.has_value());
    REQUIRE(state.value().final_text == std::optional<std::string>{u8"完成"});

    const auto bytes = fixtures::read_all(file.value());
    REQUIRE(!bytes.empty());
    REQUIRE(bytes.back() == '\n');
    REQUIRE(bytes.find("SENTINEL_SECRET_MUST_NOT_APPEAR") == std::string::npos);
}

TEST_CASE(value_json_round_trip_preserves_recursive_types) {
    const auto value = agent::Value::object({
        {"null", agent::Value()},
        {"bool", agent::Value(true)},
        {"integer", agent::Value(std::int64_t{-42})},
        {"double", agent::Value(1.25)},
        {"string", agent::Value(u8"值")},
        {"array", agent::Value::array({
                      agent::Value(std::int64_t{3}),
                      agent::Value::object({{"nested", u8"内容"}})})}});
    const auto decoded = agent::value_from_json(agent::value_to_json(value));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value() == value);
}

TEST_CASE(event_json_round_trip_preserves_every_typed_payload) {
    const agent::RuntimeError complete_error{
        agent::ErrorCode::HttpFailure, u8"完整失败正文", true};
    const auto call = fixtures::tool_call();
    const agent::ToolResult result{"call-1", u8"工具输出", true};
    const std::vector<agent::EventPayload> payloads = {
        agent::TaskStartedPayload{u8"问题", u8"E:/工作区", {3, 4, 5, 6}},
        agent::ContextPreparationStartedPayload{},
        agent::ContextPreparedPayload{fixtures::evidence()},
        agent::ContextPreparationFailedPayload{complete_error},
        agent::ModelCallStartedPayload{fixtures::complex_codec_request()},
        agent::ModelCallSucceededPayload{fixtures::response_with_tool()},
        agent::ModelCallFailedPayload{complete_error},
        agent::ToolCallStartedPayload{call},
        agent::ToolCallSucceededPayload{result},
        agent::ToolCallFailedPayload{"call-1", complete_error},
        agent::TaskCompletedPayload{u8"最终文本"},
        agent::TaskFailedPayload{complete_error},
        agent::TaskBudgetExceededPayload{"max_tool_calls", complete_error},
        agent::TaskCancelledPayload{u8"用户取消", complete_error},
    };

    std::uint64_t sequence = 1;
    for (const auto& payload : payloads) {
        const auto original = fixtures::event("codec-task", sequence++, payload);
        const auto decoded = agent::event_from_json(agent::event_to_json(original));
        REQUIRE(decoded.has_value());
        REQUIRE(decoded.value() == original);
    }
}

TEST_CASE(event_json_uses_the_versioned_wire_keys_and_names) {
    const auto original = fixtures::event(
        "wire-task", 4, agent::ModelCallStartedPayload{fixtures::request()});
    const auto json = agent::event_to_json(original);
    REQUIRE(json.size() == 7);
    REQUIRE(json.at("schema_version") == 1);
    REQUIRE(json.at("sequence") == 4);
    REQUIRE(json.at("task_id") == "wire-task");
    REQUIRE(json.at("timestamp") == "2026-08-17T12:00:00.000Z");
    REQUIRE(json.at("event_type") == "model_call_started");
    REQUIRE(json.at("correlation_id") == "corr-4");
    REQUIRE(json.at("payload").at("request").at("timeout_ms") == 30'000);
    REQUIRE(json.at("payload").at("request").at("evidence").at("items").size() ==
            1);
    REQUIRE(json.at("payload").at("request").at("evidence").at("items").at(0)
                .at("source_id") == u8"源码-一");
}

TEST_CASE(event_json_rejects_an_unexpected_top_level_key) {
    auto json = agent::event_to_json(
        fixtures::task_started("extra-key-task", 1, "issue"));
    json["unexpected_extension"] = true;

    const auto decoded = agent::event_from_json(json);
    test::ScopedTempDir temp("jsonl-extra-top-level-key");
    const auto file = temp.write_text("events.jsonl", json.dump() + "\n");
    agent::JsonlEventStore store(temp.path());
    const auto loaded = store.read_file(file);

    REQUIRE(!decoded.has_value());
    REQUIRE(decoded.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(event_json_preserves_the_full_unsigned_sequence_range) {
    auto original = fixtures::task_started("wide-sequence", 1, "issue");
    original.sequence = std::numeric_limits<std::uint64_t>::max();
    const auto decoded = agent::event_from_json(agent::event_to_json(original));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value() == original);
}

TEST_CASE(jsonl_preserves_specialized_failure_terminal_errors) {
    const agent::RuntimeError context_error{
        agent::ErrorCode::DependencyUnavailable, u8"知识库不可用", true};
    const agent::RuntimeError model_error{
        agent::ErrorCode::RequestTimeout, u8"模型超时", true};
    const agent::RuntimeError tool_error{
        agent::ErrorCode::TransportFailure, u8"工具连接中断", false};

    const auto verify_trace = [](std::string name,
                                 std::vector<agent::RuntimeEvent> events,
                                 const agent::RuntimeError& expected) {
        test::ScopedTempDir temp(std::filesystem::u8path(name));
        agent::JsonlEventStore store(temp.path());
        for (const auto& item : events) {
            REQUIRE(store.append(item).has_value());
        }
        const auto event_path = store.event_path(events.front().task_id);
        REQUIRE(event_path.has_value());
        const auto loaded = store.read_file(event_path.value());
        REQUIRE(loaded.has_value());
        const auto state = agent::replay_events(loaded.value());
        REQUIRE(state.has_value());
        REQUIRE(state.value().status == agent::TaskStatus::Failed);
        REQUIRE(state.value().terminal_error ==
                std::optional<agent::RuntimeError>{expected});
    };

    const auto context_task = fixtures::valid_task_id("context-failure");
    verify_trace("context-failure",
                 {fixtures::task_started(context_task, 1, "issue"),
                  fixtures::event(context_task, 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event(context_task, 3,
                                  agent::ContextPreparationFailedPayload{
                                      context_error})},
                 context_error);
    const auto model_task = fixtures::valid_task_id("model-failure");
    verify_trace("model-failure",
                 {fixtures::task_started(model_task, 1, "issue"),
                  fixtures::event(model_task, 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event(model_task, 3,
                                  agent::ContextPreparedPayload{fixtures::evidence()}),
                  fixtures::event(model_task, 4,
                                  agent::ModelCallStartedPayload{fixtures::request()}),
                  fixtures::event(model_task, 5,
                                  agent::ModelCallFailedPayload{model_error})},
                 model_error);

    const auto call = fixtures::tool_call();
    const auto tool_task = fixtures::valid_task_id("tool-failure");
    verify_trace("tool-failure",
                 {fixtures::task_started(tool_task, 1, "issue"),
                  fixtures::event(tool_task, 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event(tool_task, 3,
                                  agent::ContextPreparedPayload{fixtures::evidence()}),
                  fixtures::event(tool_task, 4,
                                  agent::ModelCallStartedPayload{fixtures::request()}),
                  fixtures::event(tool_task, 5,
                                  agent::ModelCallSucceededPayload{
                                      fixtures::response_with_tool()}),
                  fixtures::event(tool_task, 6,
                                  agent::ToolCallStartedPayload{call}),
                  fixtures::event(tool_task, 7,
                                  agent::ToolCallFailedPayload{"call-1", tool_error})},
                 tool_error);
}

TEST_CASE(jsonl_rejects_duplicate_sequence) {
    test::ScopedTempDir temp("jsonl-duplicate");
    auto events = fixtures::completed_text_trace("task-duplicate", "issue", "done");
    events.at(2).sequence = 2;
    const auto original_bytes = fixtures::as_jsonl(events);
    const auto file = temp.write_text("events.jsonl", original_bytes);
    agent::JsonlEventStore store(temp.path());
    const auto loaded = store.read_file(file);
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixtures::read_all(file) == original_bytes);
}

TEST_CASE(jsonl_rejects_missing_sequence) {
    test::ScopedTempDir temp("jsonl-gap");
    auto events = fixtures::completed_text_trace("task-gap", "issue", "done");
    events.at(2).sequence = 4;
    const auto file = temp.write_text("events.jsonl", fixtures::as_jsonl(events));
    agent::JsonlEventStore store(temp.path());
    const auto loaded = store.read_file(file);
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(jsonl_rejects_invalid_json_missing_keys_and_unknown_schema) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"invalid", "{not-json}\n"},
        {"missing-key",
         R"({"schema_version":1,"sequence":1,"task_id":"task","timestamp":"now","event_type":"task_started","payload":{}})"
         "\n"},
        {"unknown-schema",
         R"({"schema_version":2,"sequence":1,"task_id":"task","timestamp":"now","event_type":"task_started","correlation_id":"corr","payload":{"issue":"x","workspace_utf8":"E:/","budgets":{"max_model_rounds":1,"max_tool_calls":1,"max_task_time_ms":1,"model_timeout_ms":1}}})"
         "\n"},
    };

    for (const auto& item : cases) {
        test::ScopedTempDir temp(item.first);
        const auto file = temp.write_text("events.jsonl", item.second);
        agent::JsonlEventStore store(temp.path());
        const auto loaded = store.read_file(file);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(jsonl_rejects_mixed_task_ids_and_illegal_transitions) {
    const auto first_task = fixtures::valid_task_id("task-one");
    const auto second_task = fixtures::valid_task_id("task-two");
    const auto first = fixtures::task_started(first_task, 1, "issue");
    auto mixed = fixtures::event(second_task, 2,
                                 agent::ContextPreparationStartedPayload{});
    auto illegal = fixtures::task_started(first_task, 2, "again");
    const std::vector<std::pair<std::string, std::vector<agent::RuntimeEvent>>> cases = {
        {"mixed", {first, mixed}}, {"illegal", {first, illegal}}};

    for (const auto& item : cases) {
        test::ScopedTempDir temp(item.first);
        const auto file = temp.write_text("events.jsonl",
                                          fixtures::as_jsonl(item.second));
        agent::JsonlEventStore store(temp.path());
        const auto loaded = store.read_file(file);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(jsonl_rejects_forged_max_tokens_with_end_turn_raw_stop_reason) {
    test::ScopedTempDir temp("jsonl-stop-pair-forgery");
    const auto task = fixtures::valid_task_id("stop-pair-forgery");
    const agent::RuntimeError budget_error{
        agent::ErrorCode::BudgetExceeded,
        "model output token budget exceeded", false};
    const std::vector<agent::RuntimeEvent> forged = {
        fixtures::task_started(task, 1, "issue"),
        fixtures::event(task, 2, agent::ContextPreparationStartedPayload{}),
        fixtures::event(task, 3,
                        agent::ContextPreparedPayload{fixtures::evidence()}),
        fixtures::event(task, 4,
                        agent::ModelCallStartedPayload{fixtures::request()}),
        fixtures::event(
            task, 5,
            agent::ModelCallSucceededPayload{
                {{agent::TextBlock{"partial"}}, agent::StopReason::MaxTokens,
                 "end_turn", 120, 12, "forged-provider-request"}}),
        fixtures::event(
            task, 6,
            agent::TaskBudgetExceededPayload{"max_tokens", budget_error}),
    };
    const auto file =
        temp.write_text("events.jsonl", fixtures::as_jsonl(forged));
    agent::JsonlEventStore store(temp.path());

    const auto loaded = store.read_file(file);

    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(jsonl_rejects_invalid_or_duplicate_response_tool_calls) {
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

    for (std::size_t index = 0; index < invalid.size(); ++index) {
        test::ScopedTempDir temp("jsonl-tool-call-validation");
        const auto task = fixtures::valid_task_id(
            "tool-call-" + std::to_string(index));
        const std::vector<agent::RuntimeEvent> forged = {
            fixtures::task_started(task, 1, "issue"),
            fixtures::event(task, 2,
                            agent::ContextPreparationStartedPayload{}),
            fixtures::event(task, 3,
                            agent::ContextPreparedPayload{fixtures::evidence()}),
            fixtures::event(task, 4,
                            agent::ModelCallStartedPayload{fixtures::request()}),
            fixtures::event(
                task, 5,
                agent::ModelCallSucceededPayload{
                    {invalid.at(index), agent::StopReason::ToolUse, "tool_use",
                     120, 12, "forged-provider-request"}}),
        };
        const auto file =
            temp.write_text("events.jsonl", fixtures::as_jsonl(forged));
        agent::JsonlEventStore store(temp.path());

        const auto loaded = store.read_file(file);

        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(jsonl_rejects_forged_invalid_context_evidence) {
    test::ScopedTempDir temp("jsonl-invalid-evidence");
    const auto task = fixtures::valid_task_id("invalid-evidence");
    auto invalid = fixtures::evidence();
    invalid.items.push_back(invalid.items.front());
    const std::vector<agent::RuntimeEvent> forged{
        fixtures::task_started(task, 1, "issue"),
        fixtures::event(task, 2,
                        agent::ContextPreparationStartedPayload{}),
        fixtures::event(task, 3,
                        agent::ContextPreparedPayload{std::move(invalid)})};
    const auto file =
        temp.write_text("events.jsonl", fixtures::as_jsonl(forged));
    agent::JsonlEventStore store(temp.path());

    const auto loaded = store.read_file(file);

    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(jsonl_rejects_model_request_evidence_that_differs_from_context) {
    for (const bool duplicate_request_evidence : {false, true}) {
        test::ScopedTempDir temp("jsonl-request-evidence-binding");
        const auto task = fixtures::valid_task_id(
            duplicate_request_evidence ? "invalid-request-evidence"
                                       : "mismatched-request-evidence");
        auto request = fixtures::request();
        request.evidence.items.front().source_id = u8"伪造来源";
        request.evidence.items.front().content = u8"另一份有效证据";
        if (duplicate_request_evidence) {
            request.evidence.items.push_back(request.evidence.items.front());
        }
        const std::vector<agent::RuntimeEvent> forged{
            fixtures::task_started(task, 1, "issue"),
            fixtures::event(task, 2,
                            agent::ContextPreparationStartedPayload{}),
            fixtures::event(
                task, 3,
                agent::ContextPreparedPayload{fixtures::evidence()}),
            fixtures::event(
                task, 4,
                agent::ModelCallStartedPayload{std::move(request)})};
        const auto file =
            temp.write_text("events.jsonl", fixtures::as_jsonl(forged));
        agent::JsonlEventStore store(temp.path());

        const auto loaded = store.read_file(file);

        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(jsonl_rejects_model_request_messages_or_timeout_not_bound_to_task) {
    for (const bool forge_messages : {false, true}) {
        test::ScopedTempDir temp("jsonl-request-state-binding");
        const auto task = fixtures::valid_task_id(
            forge_messages ? "forged-messages" : "forged-timeout");
        auto request = fixtures::request("issue");
        if (forge_messages) {
            request.messages.front().content.front() =
                agent::TextBlock{"forged issue"};
        } else {
            request.timeout_ms = 29'999;
        }
        const std::vector<agent::RuntimeEvent> forged{
            fixtures::task_started(task, 1, "issue"),
            fixtures::event(
                task, 2, agent::ContextPreparationStartedPayload{}),
            fixtures::event(
                task, 3,
                agent::ContextPreparedPayload{fixtures::evidence()}),
            fixtures::event(
                task, 4,
                agent::ModelCallStartedPayload{std::move(request)})};
        const auto file =
            temp.write_text("events.jsonl", fixtures::as_jsonl(forged));
        agent::JsonlEventStore store(temp.path());

        const auto loaded = store.read_file(file);

        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code ==
                agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(jsonl_append_reports_unwritable_runtime_root) {
    test::ScopedTempDir temp("jsonl-unwritable-root");
    const auto root_file = temp.write_text("not-a-directory", "occupied");
    agent::JsonlEventStore store(root_file);
    const auto result = store.append(
        fixtures::task_started(
            fixtures::valid_task_id("task-write-failure"), 1, "issue"));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixtures::read_all(root_file) == "occupied");
}

TEST_CASE(jsonl_append_never_follows_an_existing_leaf_symlink) {
    test::ScopedTempDir temp("jsonl-leaf-symlink");
    const auto task = fixtures::valid_task_id("leaf-symlink");
    const auto runtime_root = temp.path() / "runtime";
    const auto task_directory = runtime_root / "tasks" / task;
    std::filesystem::create_directories(task_directory);
    const auto external =
        temp.write_text("outside/external-events.jsonl", "EXTERNAL_SENTINEL\n");
    const auto leaf = task_directory / "events.jsonl";
    std::error_code link_error;
    std::filesystem::create_symlink(external, leaf, link_error);
    if (link_error) {
        std::cout << "SKIP leaf symlink regression: environment cannot create "
                     "a file symlink ("
                  << link_error.message() << ")\n";
        return;
    }
    const auto original_external_bytes = fixtures::read_all(external);
    agent::JsonlEventStore store(runtime_root);

    const auto appended =
        store.append(fixtures::task_started(task, 1, "issue"));

    REQUIRE(!appended.has_value());
    REQUIRE(appended.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixtures::read_all(external) == original_external_bytes);
    REQUIRE(std::filesystem::is_symlink(
        std::filesystem::symlink_status(leaf)));
}

TEST_CASE(jsonl_append_rejects_a_leaf_hard_link_without_modifying_its_target) {
    test::ScopedTempDir temp("jsonl-leaf-hard-link");
    const auto task = fixtures::valid_task_id("leaf-hard-link");
    const auto runtime_root = temp.path() / "runtime";
    const auto task_directory = runtime_root / "tasks" / task;
    std::filesystem::create_directories(task_directory);
    const auto external =
        temp.write_text("outside/external-events.jsonl", "EXTERNAL_SENTINEL\n");
    const auto leaf = task_directory / "events.jsonl";
    std::error_code link_error;
    std::filesystem::create_hard_link(external, leaf, link_error);
    REQUIRE(!link_error);
    const auto original_external_bytes = fixtures::read_all(external);
    agent::JsonlEventStore store(runtime_root);

    const auto appended =
        store.append(fixtures::task_started(task, 1, "issue"));

    REQUIRE(!appended.has_value());
    REQUIRE(appended.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixtures::read_all(external) == original_external_bytes);
}

TEST_CASE(jsonl_task_read_rejects_a_leaf_hard_link) {
    test::ScopedTempDir temp("jsonl-read-hard-link");
    const auto task = fixtures::valid_task_id("read-hard-link");
    const auto runtime_root = temp.path() / "runtime";
    const auto task_directory = runtime_root / "tasks" / task;
    std::filesystem::create_directories(task_directory);
    const auto external = temp.write_text(
        "outside/events.jsonl",
        fixtures::as_jsonl({fixtures::task_started(task, 1, "issue")}));
    std::error_code link_error;
    std::filesystem::create_hard_link(
        external, task_directory / "events.jsonl", link_error);
    REQUIRE(!link_error);
    agent::JsonlEventStore store(runtime_root);

    const auto loaded = store.read_task(task);

    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(jsonl_task_read_never_follows_link_components) {
    test::ScopedTempDir temp("jsonl-read-links");
    const auto task = fixtures::valid_task_id("read-links");
    const auto runtime_root = temp.path() / "runtime";
    std::filesystem::create_directories(runtime_root / "tasks");
    const auto external_directory = temp.path() / "outside" / task;
    std::filesystem::create_directories(external_directory);
    temp.write_text(
        std::filesystem::relative(external_directory / "events.jsonl",
                                  temp.path()),
        fixtures::as_jsonl({fixtures::task_started(task, 1, "issue")}));
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        external_directory, runtime_root / "tasks" / task, link_error);
    if (link_error) {
        std::cout << "SKIP task-read directory-link regression: environment "
                     "cannot create a directory link ("
                  << link_error.message() << ")\n";
        return;
    }
    agent::JsonlEventStore store(runtime_root);

    const auto loaded = store.read_task(task);

    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(event_json_rejects_extra_keys_in_every_schema_owned_object_family) {
    using Json = nlohmann::json;
    std::vector<Json> invalid_records;

    auto empty_payload = agent::event_to_json(fixtures::event(
        "codec-empty", 2, agent::ContextPreparationStartedPayload{}));
    empty_payload["payload"]["extra"] = true;
    invalid_records.push_back(std::move(empty_payload));

    auto payload = agent::event_to_json(fixtures::task_started(
        "codec-payload", 1, "issue"));
    payload["payload"]["extra"] = true;
    invalid_records.push_back(std::move(payload));

    auto budgets = agent::event_to_json(fixtures::task_started(
        "codec-budgets", 1, "issue"));
    budgets["payload"]["budgets"]["extra"] = true;
    invalid_records.push_back(std::move(budgets));

    const agent::RuntimeError error{
        agent::ErrorCode::ProtocolFailure, "protocol", false};
    auto runtime_error = agent::event_to_json(fixtures::event(
        "codec-error", 5, agent::ModelCallFailedPayload{error}));
    runtime_error["payload"]["error"]["extra"] = true;
    invalid_records.push_back(std::move(runtime_error));

    auto request = agent::event_to_json(fixtures::event(
        "codec-request", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    request["payload"]["request"]["extra"] = true;
    invalid_records.push_back(std::move(request));

    auto message = agent::event_to_json(fixtures::event(
        "codec-message", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    message["payload"]["request"]["messages"][0]["extra"] = true;
    invalid_records.push_back(std::move(message));

    auto content_block = agent::event_to_json(fixtures::event(
        "codec-block", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    content_block["payload"]["request"]["messages"][0]["content"][0]
                 ["extra"] = true;
    invalid_records.push_back(std::move(content_block));

    auto tool_definition = agent::event_to_json(fixtures::event(
        "codec-tool-definition", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    tool_definition["payload"]["request"]["tools"][0]["extra"] = true;
    invalid_records.push_back(std::move(tool_definition));

    auto evidence_pack = agent::event_to_json(fixtures::event(
        "codec-evidence-pack", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    evidence_pack["payload"]["request"]["evidence"]["extra"] = true;
    invalid_records.push_back(std::move(evidence_pack));

    auto evidence_item = agent::event_to_json(fixtures::event(
        "codec-evidence-item", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    evidence_item["payload"]["request"]["evidence"]["items"][0]["extra"] =
        true;
    invalid_records.push_back(std::move(evidence_item));

    auto response = agent::event_to_json(fixtures::event(
        "codec-response", 5,
        agent::ModelCallSucceededPayload{fixtures::response_with_tool()}));
    response["payload"]["response"]["extra"] = true;
    invalid_records.push_back(std::move(response));

    auto tool_call = agent::event_to_json(fixtures::event(
        "codec-tool-call", 6,
        agent::ToolCallStartedPayload{fixtures::tool_call()}));
    tool_call["payload"]["call"]["extra"] = true;
    invalid_records.push_back(std::move(tool_call));

    auto tool_result = agent::event_to_json(fixtures::event(
        "codec-tool-result", 7,
        agent::ToolCallSucceededPayload{
            {"call-1", "contents", false}}));
    tool_result["payload"]["result"]["extra"] = true;
    invalid_records.push_back(std::move(tool_result));

    for (const auto& json : invalid_records) {
        const auto decoded = agent::event_from_json(json);
        REQUIRE(!decoded.has_value());
        REQUIRE(decoded.error().code == agent::ErrorCode::PersistenceFailure);
    }
}

TEST_CASE(event_json_keeps_provider_neutral_value_maps_open) {
    auto json = agent::event_to_json(fixtures::event(
        "codec-open-values", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    json["payload"]["request"]["tools"][0]["input_schema"]
        ["future_schema_keyword"] = "allowed";
    json["payload"]["request"]["evidence"]["items"][0]["metadata"]
        ["future_metadata"] = 42;
    auto tool_call_json = agent::event_to_json(fixtures::event(
        "codec-open-arguments", 6,
        agent::ToolCallStartedPayload{fixtures::tool_call()}));
    tool_call_json["payload"]["call"]["arguments"]["future_argument"] =
        true;

    const auto decoded = agent::event_from_json(json);
    const auto decoded_tool_call = agent::event_from_json(tool_call_json);

    REQUIRE(decoded.has_value());
    REQUIRE(decoded_tool_call.has_value());
    const auto& decoded_request =
        std::get<agent::ModelCallStartedPayload>(decoded.value().payload).request;
    REQUIRE(decoded_request.tools.at(0).input_schema.at("future_schema_keyword") ==
            agent::Value("allowed"));
    REQUIRE(decoded_request.evidence.items.at(0).metadata.at("future_metadata") ==
            agent::Value(std::int64_t{42}));
    const auto& decoded_call = std::get<agent::ToolCallStartedPayload>(
        decoded_tool_call.value().payload).call;
    REQUIRE(decoded_call.arguments.at("future_argument") == agent::Value(true));
}

TEST_CASE(event_json_rejects_nonpositive_decoded_budgets_and_timeouts) {
    const std::vector<std::string> fields = {
        "max_model_rounds", "max_tool_calls", "max_task_time_ms",
        "model_timeout_ms"};
    for (const auto& field : fields) {
        auto json = agent::event_to_json(fixtures::task_started(
            "codec-invalid-budget", 1, "issue"));
        json["payload"]["budgets"][field] = 0;
        const auto decoded = agent::event_from_json(json);
        REQUIRE(!decoded.has_value());
        REQUIRE(decoded.error().code == agent::ErrorCode::PersistenceFailure);
    }

    auto request = agent::event_to_json(fixtures::event(
        "codec-invalid-timeout", 4,
        agent::ModelCallStartedPayload{fixtures::request()}));
    request["payload"]["request"]["timeout_ms"] = 0;
    const auto decoded = agent::event_from_json(request);
    REQUIRE(!decoded.has_value());
    REQUIRE(decoded.error().code == agent::ErrorCode::PersistenceFailure);
}

TEST_CASE(jsonl_persistence_rejects_unsafe_task_ids_before_path_creation) {
    test::ScopedTempDir temp("jsonl-task-id-policy");
    agent::JsonlEventStore store(temp.path() / "runtime");
    const auto escaped_absolute = temp.path() / "escaped-absolute";
    const std::vector<std::string> invalid_ids = {
        escaped_absolute.generic_u8string(),
        "task-0000000000000000/000000000000000",
        "../escaped-parent",
        "task-0000000000000000000000000000000A",
        "task-0000000000000000000000000000000",
        "task-000000000000000000000000000000000",
    };

    for (const auto& invalid_id : invalid_ids) {
        const auto appended = store.append(
            fixtures::task_started(invalid_id, 1, "issue"));
        REQUIRE(!appended.has_value());
        REQUIRE(appended.error().code == agent::ErrorCode::PersistenceFailure);
    }

    REQUIRE(!std::filesystem::exists(escaped_absolute / "events.jsonl"));
    REQUIRE(!std::filesystem::exists(temp.path() / "runtime" /
                                     "escaped-parent" / "events.jsonl"));

    const auto valid_id =
        std::string{"task-0000000000000000000000000000000a"};
    const auto appended = store.append(
        fixtures::task_started(valid_id, 1, "issue"));
    REQUIRE(appended.has_value());
    REQUIRE(std::filesystem::exists(temp.path() / "runtime" / "tasks" /
                                    valid_id / "events.jsonl"));
}
