#include "adapters/persistence/memory_event_json.h"
#include "test_support.h"
#include <nlohmann/json.hpp>

namespace {
nlohmann::json fixture() {
    return nlohmann::json::parse(
        R"({"schema_version":1,"sequence":1,"timestamp":"2026-09-07T10:00:00.000Z","correlation_id":"corr-memory","event_type":"memory_upserted","payload":{"entry":{"memory_id":"memory-11111111111111111111111111111111","category":"fact","scope_utf8":"","content":"Use C++17","source_session_id":"session-11111111111111111111111111111111","source_turn_start":1,"source_turn_end":2,"created_at_utc":"2026-09-07T10:00:00.000Z","updated_at_utc":"2026-09-07T10:00:00.000Z","origin":"explicit_user"}}})");
}
} // namespace

TEST_CASE(memory_json_round_trips_all_payloads_and_category_origin_values) {
    for (const auto *category :
         {"preference", "decision", "fact", "workflow", "constraint"}) {
        for (const auto *origin : {"explicit_user", "model_consolidation"}) {
            auto json = fixture();
            json["payload"]["entry"]["category"] = category;
            json["payload"]["entry"]["origin"] = origin;
            const auto event = agent::memory_event_from_json(json);
            REQUIRE(event.has_value());
            REQUIRE(agent::memory_event_to_json(event.value()) == json);
        }
    }
    auto json = fixture();
    json["event_type"] = "memory_forgotten";
    json["payload"] = {{"memory_id", "memory-11111111111111111111111111111111"}};
    auto decoded = agent::memory_event_from_json(json);
    REQUIRE(decoded.has_value());
    REQUIRE(agent::memory_event_to_json(decoded.value()) == json);
    json["event_type"] = "session_consolidated";
    json["payload"] = {{"session_id", "session-11111111111111111111111111111111"},
                       {"through_turn", 2}};
    decoded = agent::memory_event_from_json(json);
    REQUIRE(decoded.has_value());
    REQUIRE(agent::memory_event_to_json(decoded.value()) == json);
}

TEST_CASE(memory_json_rejects_wrong_shapes_keys_types_and_invalid_domain_values) {
    for (const auto &wrong : {nlohmann::json{}, nlohmann::json::array(),
                              nlohmann::json("x"), nlohmann::json(1)}) {
        REQUIRE(!agent::memory_event_from_json(wrong).has_value());
        auto json = fixture();
        json["payload"] = wrong;
        REQUIRE(!agent::memory_event_from_json(json).has_value());
        json = fixture();
        json["payload"]["entry"] = wrong;
        REQUIRE(!agent::memory_event_from_json(json).has_value());
    }
    for (const auto *key : {"schema_version", "sequence", "timestamp", "event_type",
                            "correlation_id", "payload"}) {
        auto json = fixture();
        json.erase(key);
        REQUIRE(!agent::memory_event_from_json(json).has_value());
    }
    auto json = fixture();
    json["extra"] = true;
    REQUIRE(!agent::memory_event_from_json(json).has_value());
    json = fixture();
    json["payload"]["extra"] = true;
    REQUIRE(!agent::memory_event_from_json(json).has_value());
    json = fixture();
    json["payload"]["entry"]["extra"] = true;
    REQUIRE(!agent::memory_event_from_json(json).has_value());
    for (const auto *key : {"memory_id", "category", "scope_utf8", "content",
                            "source_session_id", "source_turn_start", "source_turn_end",
                            "created_at_utc", "updated_at_utc", "origin"}) {
        json = fixture();
        json["payload"]["entry"].erase(key);
        REQUIRE(!agent::memory_event_from_json(json).has_value());
    }
    for (const auto &wrong : {nlohmann::json(-1), nlohmann::json(1.5),
                              nlohmann::json(true), nlohmann::json("1")}) {
        for (const auto *key : {"schema_version", "sequence"}) {
            json = fixture();
            json[key] = wrong;
            REQUIRE(!agent::memory_event_from_json(json).has_value());
        }
        json = fixture();
        json["payload"]["entry"]["source_turn_start"] = wrong;
        REQUIRE(!agent::memory_event_from_json(json).has_value());
    }
    for (const auto *key : {"memory_id", "category", "content", "source_session_id",
                            "created_at_utc", "updated_at_utc", "origin"}) {
        json = fixture();
        json["payload"]["entry"][key] = "";
        REQUIRE(!agent::memory_event_from_json(json).has_value());
    }
    json = fixture();
    json["schema_version"] = 2;
    REQUIRE(!agent::memory_event_from_json(json).has_value());
    json = fixture();
    json["event_type"] = "unknown";
    REQUIRE(!agent::memory_event_from_json(json).has_value());
}
