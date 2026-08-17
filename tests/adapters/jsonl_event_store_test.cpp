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
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
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

agent::ModelRequest request() {
    const auto path_schema =
        agent::Value::object({{"type", agent::Value("string")}});
    const auto properties =
        agent::Value::object({{"path", path_schema}});
    const auto input_schema = agent::Value::object(
        {{"type", agent::Value("object")}, {"properties", properties}});
    std::vector<agent::Message> messages = {
        {agent::Role::System, {agent::TextBlock{u8"严格执行。"}}},
        {agent::Role::User,
         {agent::TextBlock{u8"检查警告"},
          agent::ToolResultBlock{{"prior-call", u8"旧结果", false}}}},
    };
    std::vector<agent::ToolDefinition> tools = {
        {"read_file", u8"读取文件", input_schema}};
    return {u8"你是编码代理。", std::move(messages), std::move(tools), 30'000};
}

agent::ToolCall tool_call() {
    return {"call-1", "read_file",
            agent::Value::object({
                {"path", u8"src/入口.cpp"},
                {"line", agent::Value(std::int64_t{9})}})};
}

agent::ModelResponse response_with_tool() {
    return {{agent::TextBlock{u8"先读取"}, agent::ToolUseBlock{tool_call()},
             agent::ToolResultBlock{{"embedded", u8"有序块", true}}},
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
    return {
        task_started(task_id, 1, issue),
        event(task_id, 2, agent::ContextPreparationStartedPayload{}),
        event(task_id, 3, agent::ContextPreparedPayload{evidence()}),
        event(task_id, 4, agent::ModelCallStartedPayload{request()}),
        event(task_id, 5,
              agent::ModelCallSucceededPayload{
                  {{agent::TextBlock{final_text}}, agent::StopReason::EndTurn,
                   "end_turn", 120, 12, "provider-request-final"}}),
        event(task_id, 6, agent::TaskCompletedPayload{final_text}),
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

    const auto file = store.event_path("task-unicode");
    const auto loaded = store.read_file(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == events);
    const auto state = agent::replay_events(loaded.value());
    REQUIRE(state.has_value());
    REQUIRE(state.value().final_text == std::optional<std::string>{u8"完成"});

    const auto bytes = fixtures::read_all(file);
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
        agent::ModelCallStartedPayload{fixtures::request()},
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
        const auto loaded = store.read_file(store.event_path(events.front().task_id));
        REQUIRE(loaded.has_value());
        const auto state = agent::replay_events(loaded.value());
        REQUIRE(state.has_value());
        REQUIRE(state.value().status == agent::TaskStatus::Failed);
        REQUIRE(state.value().terminal_error ==
                std::optional<agent::RuntimeError>{expected});
    };

    verify_trace("context-failure",
                 {fixtures::task_started("context-failure", 1, "issue"),
                  fixtures::event("context-failure", 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event("context-failure", 3,
                                  agent::ContextPreparationFailedPayload{
                                      context_error})},
                 context_error);
    verify_trace("model-failure",
                 {fixtures::task_started("model-failure", 1, "issue"),
                  fixtures::event("model-failure", 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event("model-failure", 3,
                                  agent::ContextPreparedPayload{fixtures::evidence()}),
                  fixtures::event("model-failure", 4,
                                  agent::ModelCallStartedPayload{fixtures::request()}),
                  fixtures::event("model-failure", 5,
                                  agent::ModelCallFailedPayload{model_error})},
                 model_error);

    const auto call = fixtures::tool_call();
    verify_trace("tool-failure",
                 {fixtures::task_started("tool-failure", 1, "issue"),
                  fixtures::event("tool-failure", 2,
                                  agent::ContextPreparationStartedPayload{}),
                  fixtures::event("tool-failure", 3,
                                  agent::ContextPreparedPayload{fixtures::evidence()}),
                  fixtures::event("tool-failure", 4,
                                  agent::ModelCallStartedPayload{fixtures::request()}),
                  fixtures::event("tool-failure", 5,
                                  agent::ModelCallSucceededPayload{
                                      fixtures::response_with_tool()}),
                  fixtures::event("tool-failure", 6,
                                  agent::ToolCallStartedPayload{call}),
                  fixtures::event("tool-failure", 7,
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
    const auto first = fixtures::task_started("task-one", 1, "issue");
    auto mixed = fixtures::event("task-two", 2,
                                 agent::ContextPreparationStartedPayload{});
    auto illegal = fixtures::task_started("task-one", 2, "again");
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

TEST_CASE(jsonl_append_reports_unwritable_runtime_root) {
    test::ScopedTempDir temp("jsonl-unwritable-root");
    const auto root_file = temp.write_text("not-a-directory", "occupied");
    agent::JsonlEventStore store(root_file);
    const auto result = store.append(
        fixtures::task_started("task-write-failure", 1, "issue"));
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(fixtures::read_all(root_file) == "occupied");
}
