#include "adapters/persistence/memory_event_json.h"

#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

namespace agent {
namespace {
using Json = nlohmann::json;
void exact(const Json &json, std::initializer_list<const char *> keys) {
    if (!json.is_object() || json.size() != keys.size())
        throw std::invalid_argument("unexpected memory keys");
    for (const auto *key : keys)
        if (!json.contains(key))
            throw std::invalid_argument("missing memory key");
}
std::string string(const Json &json, const char *key) {
    if (!json.at(key).is_string())
        throw std::invalid_argument("expected memory string");
    return json.at(key).get<std::string>();
}
std::uint64_t number(const Json &json, const char *key) {
    const auto &value = json.at(key);
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>();
    if (!value.is_number_integer() || value.get<std::int64_t>() < 0)
        throw std::invalid_argument("expected memory unsigned integer");
    return value.get<std::uint64_t>();
}
const char *category_name(MemoryCategory category) {
    switch (category) {
    case MemoryCategory::Preference:
        return "preference";
    case MemoryCategory::Decision:
        return "decision";
    case MemoryCategory::Fact:
        return "fact";
    case MemoryCategory::Workflow:
        return "workflow";
    case MemoryCategory::Constraint:
        return "constraint";
    }
    throw std::invalid_argument("invalid memory category");
}
MemoryCategory category_from(const std::string &name) {
    for (const auto category :
         {MemoryCategory::Preference, MemoryCategory::Decision, MemoryCategory::Fact,
          MemoryCategory::Workflow, MemoryCategory::Constraint}) {
        if (name == category_name(category))
            return category;
    }
    throw std::invalid_argument("invalid memory category");
}
MemoryOrigin origin_from(const std::string &name) {
    if (name == "explicit_user")
        return MemoryOrigin::ExplicitUser;
    if (name == "model_consolidation")
        return MemoryOrigin::ModelConsolidation;
    throw std::invalid_argument("invalid memory origin");
}
Json entry_json(const MemoryEntry &e) {
    return {{"memory_id", e.memory_id},
            {"category", category_name(e.category)},
            {"scope_utf8", e.scope_utf8},
            {"content", e.content},
            {"source_session_id", e.source_session_id},
            {"source_turn_start", e.source_turn_start},
            {"source_turn_end", e.source_turn_end},
            {"created_at_utc", e.created_at_utc},
            {"updated_at_utc", e.updated_at_utc},
            {"origin", e.origin == MemoryOrigin::ExplicitUser ? "explicit_user"
                                                              : "model_consolidation"}};
}
MemoryEntry entry_from(const Json &json) {
    exact(json, {"memory_id", "category", "scope_utf8", "content", "source_session_id",
                 "source_turn_start", "source_turn_end", "created_at_utc",
                 "updated_at_utc", "origin"});
    return {string(json, "memory_id"),         category_from(string(json, "category")),
            string(json, "scope_utf8"),        string(json, "content"),
            string(json, "source_session_id"), number(json, "source_turn_start"),
            number(json, "source_turn_end"),   string(json, "created_at_utc"),
            string(json, "updated_at_utc"),    origin_from(string(json, "origin"))};
}
MemoryEventPayload payload_from(const std::string &type, const Json &json) {
    if (type == "memory_upserted") {
        exact(json, {"entry"});
        return MemoryUpsertedPayload{entry_from(json.at("entry"))};
    }
    if (type == "memory_forgotten") {
        exact(json, {"memory_id"});
        return MemoryForgottenPayload{string(json, "memory_id")};
    }
    if (type == "session_consolidated") {
        exact(json, {"session_id", "through_turn"});
        return SessionMemoryConsolidatedPayload{string(json, "session_id"),
                                                number(json, "through_turn")};
    }
    throw std::invalid_argument("invalid memory event type");
}
} // namespace

nlohmann::json memory_event_to_json(const MemoryEvent &event) {
    if (!validate_memory_event(event).has_value())
        throw std::invalid_argument("invalid memory event");
    Json json{{"schema_version", event.schema_version},
              {"sequence", event.sequence},
              {"timestamp", event.timestamp_utc},
              {"correlation_id", event.correlation_id}};
    std::visit(
        [&json](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, MemoryUpsertedPayload>) {
                json["event_type"] = "memory_upserted";
                json["payload"] = {{"entry", entry_json(typed.entry)}};
            } else if constexpr (std::is_same_v<T, MemoryForgottenPayload>) {
                json["event_type"] = "memory_forgotten";
                json["payload"] = {{"memory_id", typed.memory_id}};
            } else {
                json["event_type"] = "session_consolidated";
                json["payload"] = {{"session_id", typed.session_id},
                                   {"through_turn", typed.through_turn}};
            }
        },
        event.payload);
    return json;
}

Result<MemoryEvent> memory_event_from_json(const nlohmann::json &json) {
    try {
        exact(json, {"schema_version", "sequence", "timestamp", "correlation_id",
                     "event_type", "payload"});
        const auto schema = number(json, "schema_version");
        if (schema > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("memory schema overflow");
        MemoryEvent event{static_cast<std::uint32_t>(schema), number(json, "sequence"),
                          string(json, "timestamp"), string(json, "correlation_id"),
                          payload_from(string(json, "event_type"), json.at("payload"))};
        if (!validate_memory_event(event).has_value())
            throw std::invalid_argument("invalid memory event");
        return Result<MemoryEvent>::success(std::move(event));
    } catch (const std::exception &) {
        return Result<MemoryEvent>::failure(
            {ErrorCode::PersistenceFailure, "memory event validation failed", false});
    }
}
} // namespace agent
