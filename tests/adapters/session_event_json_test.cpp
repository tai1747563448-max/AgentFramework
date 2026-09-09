#include "adapters/persistence/session_event_json.h"
#include "application/session_reducer.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

constexpr const char* kSessionId =
    "session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kTaskId =
    "task-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

agent::SessionEvent event(std::uint64_t sequence,
                          agent::SessionEventPayload payload) {
    return {1, sequence, kSessionId,
            "2026-09-06T12:00:00.000Z",
            "corr-session-" + std::to_string(sequence),
            std::move(payload)};
}

std::vector<agent::Message> messages() {
    const agent::ToolCall call{
        "call-1", "read_file",
        agent::Value::object({{"path", agent::Value("src/main.cpp")}})};
    return {
        {agent::Role::User, {agent::TextBlock{u8"检查入口"}}},
        {agent::Role::Assistant,
         {agent::TextBlock{u8"读取"}, agent::ToolUseBlock{call}}},
        {agent::Role::User,
         {agent::ToolResultBlock{
             {"call-1", u8"文件内容", false}}}},
        {agent::Role::Assistant, {agent::TextBlock{u8"完成"}}},
    };
}

}  // namespace fixtures

TEST_CASE(session_event_json_round_trip_preserves_every_payload) {
    const std::vector<agent::SessionEventPayload> payloads{
        agent::SessionStartedPayload{u8"E:/工作区", "MiniMax-M3"},
        agent::SessionTurnStartedPayload{1, fixtures::kTaskId,
                                         u8"检查入口"},
        agent::SessionTurnCommittedPayload{1, fixtures::kTaskId,
                                           fixtures::messages()},
        agent::SessionTurnFailedPayload{1, fixtures::kTaskId,
                                        agent::TaskStatus::BudgetExceeded,
                                        "task budget exceeded"},
        agent::SessionCompactedPayload{1, u8"The first turn was resolved."},
    };

    std::uint64_t sequence = 1;
    for (const auto& payload : payloads) {
        const auto original = fixtures::event(sequence++, payload);
        const auto decoded = agent::session_event_from_json(
            agent::session_event_to_json(original));
        REQUIRE(decoded.has_value());
        REQUIRE(decoded.value() == original);
    }
}

TEST_CASE(session_event_json_encodes_session_compaction_with_the_strict_shape) {
    const auto original = fixtures::event(
        6, agent::SessionCompactedPayload{3, "Cumulative summary."});

    const auto json = agent::session_event_to_json(original);

    REQUIRE(json.at("event_type") == "session_compacted");
    REQUIRE(json.at("payload").size() == 2);
    REQUIRE(json.at("payload").at("compacted_through_turn") == 3);
    REQUIRE(json.at("payload").at("summary") == "Cumulative summary.");
}

TEST_CASE(session_event_json_uses_stable_wire_names) {
    const auto original = fixtures::event(
        2, agent::SessionTurnStartedPayload{
               1, fixtures::kTaskId, "question"});

    const auto json = agent::session_event_to_json(original);

    REQUIRE(json.size() == 7);
    REQUIRE(json.at("schema_version") == 1);
    REQUIRE(json.at("sequence") == 2);
    REQUIRE(json.at("session_id") == fixtures::kSessionId);
    REQUIRE(json.at("event_type") == "turn_started");
    REQUIRE(json.at("payload").at("turn_index") == 1);
    REQUIRE(json.at("payload").at("task_id") == fixtures::kTaskId);
}

TEST_CASE(session_event_json_rejects_extra_keys_and_invalid_status) {
    auto extra = agent::session_event_to_json(fixtures::event(
        1, agent::SessionStartedPayload{"E:/workspace", "model"}));
    extra["unexpected"] = true;
    REQUIRE(!agent::session_event_from_json(extra).has_value());

    auto invalid_status = agent::session_event_to_json(fixtures::event(
        4, agent::SessionTurnFailedPayload{
               1, fixtures::kTaskId, agent::TaskStatus::Failed,
               "task failed"}));
    invalid_status["payload"]["status"] = "completed";
    REQUIRE(!agent::session_event_from_json(invalid_status).has_value());

    auto unsafe_summary = agent::session_event_to_json(fixtures::event(
        4, agent::SessionTurnFailedPayload{
               1, fixtures::kTaskId, agent::TaskStatus::Failed,
               "task failed"}));
    REQUIRE(unsafe_summary.at("payload").at("summary") == "task failed");
    unsafe_summary["payload"]["summary"] = "provider body with a secret";
    REQUIRE(!agent::session_event_from_json(unsafe_summary).has_value());
}

TEST_CASE(session_event_json_rejects_wrong_session_compaction_shapes) {
    auto extra = agent::session_event_to_json(fixtures::event(
        6, agent::SessionCompactedPayload{3, "Cumulative summary."}));
    extra["payload"]["unexpected"] = true;
    REQUIRE(!agent::session_event_from_json(extra).has_value());

    auto wrong_turn = agent::session_event_to_json(fixtures::event(
        6, agent::SessionCompactedPayload{3, "Cumulative summary."}));
    wrong_turn["payload"]["compacted_through_turn"] = "three";
    REQUIRE(!agent::session_event_from_json(wrong_turn).has_value());

    auto wrong_summary = agent::session_event_to_json(fixtures::event(
        6, agent::SessionCompactedPayload{3, "Cumulative summary."}));
    wrong_summary["payload"]["summary"] = 3;
    REQUIRE(!agent::session_event_from_json(wrong_summary).has_value());
}

TEST_CASE(session_event_json_rejects_invalid_committed_message_roles) {
    auto json = agent::session_event_to_json(fixtures::event(
        3, agent::SessionTurnCommittedPayload{
               1, fixtures::kTaskId, fixtures::messages()}));
    json["payload"]["messages"][0]["role"] = "system";

    REQUIRE(!agent::session_event_from_json(json).has_value());
}
