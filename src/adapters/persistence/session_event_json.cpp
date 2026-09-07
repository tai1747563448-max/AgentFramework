#include "adapters/persistence/session_event_json.h"

#include "adapters/persistence/event_json.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace agent {
namespace {

using Json = nlohmann::json;

class DecodeError final : public std::runtime_error {
public:
    explicit DecodeError(const char* message) : std::runtime_error(message) {}
};

void require_exact_keys(
    const Json& json,
    std::initializer_list<std::string_view> expected) {
    if (!json.is_object() || json.size() != expected.size()) {
        throw DecodeError("unexpected object keys");
    }
    for (const auto key : expected) {
        if (!json.contains(std::string(key))) {
            throw DecodeError("missing object key");
        }
    }
}

std::string required_string(const Json& json, const char* key) {
    const auto& value = json.at(key);
    if (!value.is_string()) {
        throw DecodeError("expected string");
    }
    return value.get<std::string>();
}

std::uint64_t required_unsigned(const Json& json, const char* key) {
    const auto& value = json.at(key);
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && value.get<std::int64_t>() < 0)) {
        throw DecodeError("expected unsigned integer");
    }
    return value.get<std::uint64_t>();
}

const char* kind_name(SessionEventKind kind) {
    switch (kind) {
    case SessionEventKind::SessionStarted: return "session_started";
    case SessionEventKind::TurnStarted: return "turn_started";
    case SessionEventKind::TurnCommitted: return "turn_committed";
    case SessionEventKind::TurnFailed: return "turn_failed";
    case SessionEventKind::SessionCompacted: return "session_compacted";
    }
    throw std::invalid_argument("unknown session event kind");
}

const char* status_name(TaskStatus status) {
    switch (status) {
    case TaskStatus::Failed: return "failed";
    case TaskStatus::BudgetExceeded: return "budget_exceeded";
    case TaskStatus::Cancelled: return "cancelled";
    default: break;
    }
    throw std::invalid_argument("invalid failed turn status");
}

TaskStatus status_from_name(const std::string& name) {
    if (name == "failed") return TaskStatus::Failed;
    if (name == "budget_exceeded") return TaskStatus::BudgetExceeded;
    if (name == "cancelled") return TaskStatus::Cancelled;
    throw DecodeError("invalid failed turn status");
}

Json payload_to_json(const SessionEventPayload& payload) {
    return std::visit(
        [](const auto& typed) -> Json {
            using Payload = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Payload,
                                         SessionStartedPayload>) {
                return {{"workspace_utf8", typed.workspace_utf8},
                        {"model", typed.model}};
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnStartedPayload>) {
                return {{"turn_index", typed.turn_index},
                        {"task_id", typed.task_id},
                        {"user_text", typed.user_text}};
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnCommittedPayload>) {
                return {{"turn_index", typed.turn_index},
                        {"task_id", typed.task_id},
                        {"messages", message_list_to_json(typed.messages)}};
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnFailedPayload>) {
                return {{"turn_index", typed.turn_index},
                        {"task_id", typed.task_id},
                        {"status", status_name(typed.status)},
                        {"summary", typed.summary}};
            } else {
                return {{"compacted_through_turn", typed.compacted_through_turn},
                        {"summary", typed.summary}};
            }
        },
        payload);
}

SessionEventPayload payload_from_json(const std::string& type,
                                      const Json& json) {
    if (type == "session_started") {
        require_exact_keys(json, {"workspace_utf8", "model"});
        return SessionStartedPayload{
            required_string(json, "workspace_utf8"),
            required_string(json, "model")};
    }
    if (type == "turn_started") {
        require_exact_keys(json, {"turn_index", "task_id", "user_text"});
        return SessionTurnStartedPayload{
            required_unsigned(json, "turn_index"),
            required_string(json, "task_id"),
            required_string(json, "user_text")};
    }
    if (type == "turn_committed") {
        require_exact_keys(json, {"turn_index", "task_id", "messages"});
        auto messages = message_list_from_json(json.at("messages"));
        if (!messages.has_value()) {
            throw DecodeError("invalid committed messages");
        }
        return SessionTurnCommittedPayload{
            required_unsigned(json, "turn_index"),
            required_string(json, "task_id"),
            std::move(messages.value())};
    }
    if (type == "turn_failed") {
        require_exact_keys(
            json, {"turn_index", "task_id", "status", "summary"});
        const auto status = status_from_name(required_string(json, "status"));
        const auto summary = required_string(json, "summary");
        const auto* expected_summary = safe_session_failure_summary(status);
        if (expected_summary == nullptr || summary != expected_summary) {
            throw DecodeError("invalid failed turn summary");
        }
        return SessionTurnFailedPayload{
            required_unsigned(json, "turn_index"),
            required_string(json, "task_id"),
            status, summary};
    }
    if (type == "session_compacted") {
        require_exact_keys(json, {"compacted_through_turn", "summary"});
        return SessionCompactedPayload{
            required_unsigned(json, "compacted_through_turn"),
            required_string(json, "summary")};
    }
    throw DecodeError("unknown session event type");
}

}  // namespace

nlohmann::json session_event_to_json(const SessionEvent& event) {
    return {{"schema_version", event.schema_version},
            {"sequence", event.sequence},
            {"session_id", event.session_id},
            {"timestamp", event.timestamp_utc},
            {"event_type", kind_name(session_event_kind(event.payload))},
            {"correlation_id", event.correlation_id},
            {"payload", payload_to_json(event.payload)}};
}

Result<SessionEvent> session_event_from_json(const nlohmann::json& json) {
    try {
        require_exact_keys(
            json, {"schema_version", "sequence", "session_id", "timestamp",
                   "event_type", "correlation_id", "payload"});
        const auto schema = required_unsigned(json, "schema_version");
        if (schema > std::numeric_limits<std::uint32_t>::max()) {
            throw DecodeError("schema version out of range");
        }
        SessionEvent event{
            static_cast<std::uint32_t>(schema),
            required_unsigned(json, "sequence"),
            required_string(json, "session_id"),
            required_string(json, "timestamp"),
            required_string(json, "correlation_id"),
            payload_from_json(required_string(json, "event_type"),
                              json.at("payload"))};
        return Result<SessionEvent>::success(std::move(event));
    } catch (const std::exception&) {
        return Result<SessionEvent>::failure(
            {ErrorCode::PersistenceFailure,
             "session event validation failed", false});
    }
}

}  // namespace agent
