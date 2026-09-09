#include "application/memory_reducer.h"
#include "test_support.h"

#include <functional>
#include <limits>

namespace {
constexpr const char *id = "memory-11111111111111111111111111111111";
constexpr const char *session = "session-11111111111111111111111111111111";
constexpr const char *t1 = "2026-09-07T10:00:00.000Z";
constexpr const char *t2 = "2026-09-07T10:00:01.000Z";
agent::MemoryEntry entry() {
    return {id,
            agent::MemoryCategory::Fact,
            "E:/workspace",
            "Use C++17",
            session,
            1,
            2,
            t1,
            t1,
            agent::MemoryOrigin::ExplicitUser};
}
agent::MemoryEvent upsert(agent::MemoryEntry value = entry()) {
    return {1, 1, t1, "corr-memory", agent::MemoryUpsertedPayload{value}};
}
} // namespace

TEST_CASE(memory_reducer_replays_empty_and_upsert_update_forget_checkpoint) {
    const auto empty = agent::replay_memory_events({});
    REQUIRE(empty.has_value());
    REQUIRE(empty.value() == agent::MemoryState{});
    auto changed = entry();
    changed.content = "Use C++20";
    changed.updated_at_utc = t2;
    const std::vector<agent::MemoryEvent> events{
        upsert(),
        {1, 2, t2, "corr-update", agent::MemoryUpsertedPayload{changed}},
        {1, 3, t2, "corr-forget", agent::MemoryForgottenPayload{id}},
        {1, 4, t2, "corr-checkpoint",
         agent::SessionMemoryConsolidatedPayload{session, 2}}};
    auto updated = agent::replay_memory_events({events[0], events[1]});
    REQUIRE(updated.has_value());
    REQUIRE(updated.value().active_entries.at(id).content == "Use C++20");
    auto replayed = agent::replay_memory_events(events);
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value().last_sequence == 4);
    REQUIRE(replayed.value().last_timestamp_utc == t2);
    REQUIRE(replayed.value().active_entries.empty());
    REQUIRE(replayed.value().session_checkpoints.at(session) == 2);
    auto current = agent::MemoryState{};
    for (const auto &event : events) {
        auto next = agent::reduce_memory_event(current, event);
        REQUIRE(next.has_value());
        current = next.value();
    }
    REQUIRE(current == replayed.value());
    auto reused = upsert();
    reused.sequence = 5;
    reused.timestamp_utc = t2;
    REQUIRE(!agent::reduce_memory_event(current, reused).has_value());
}

TEST_CASE(memory_reducer_rejects_invalid_entry_shapes_and_utf8) {
    const std::vector<std::function<void(agent::MemoryEntry &)>> mutations{
        [](auto &e) { e.memory_id = "memory-ABC"; },
        [](auto &e) { e.memory_id.back() = 'A'; },
        [](auto &e) { e.category = static_cast<agent::MemoryCategory>(77); },
        [](auto &e) { e.origin = static_cast<agent::MemoryOrigin>(77); },
        [](auto &e) { e.content.clear(); },
        [](auto &e) { e.content = " \t\n"; },
        [](auto &e) { e.content = "\xC0\xAF"; },
        [](auto &e) { e.content = std::string("x\0y", 3); },
        [](auto &e) { e.scope_utf8 = " "; },
        [](auto &e) { e.scope_utf8 = "\xED\xA0\x80"; },
        [](auto &e) { e.source_session_id = "../session"; },
        [](auto &e) { e.source_turn_start = 0; },
        [](auto &e) { e.source_turn_end = 0; },
        [](auto &e) { e.source_turn_start = 3; },
        [](auto &e) { e.created_at_utc = "yesterday"; },
        [](auto &e) { e.created_at_utc = "2026-02-29T10:00:00.000Z"; },
        [](auto &e) { e.updated_at_utc = "2026-09-07T10:00:60.000Z"; },
        [](auto &e) { e.updated_at_utc = "2026-09-07T09:00:00.000Z"; },
        [](auto &e) { e.updated_at_utc = "2026-09-07T11:00:00.000Z"; }};
    for (const auto &mutate : mutations) {
        auto e = entry();
        mutate(e);
        REQUIRE(!agent::reduce_memory_event({}, upsert(e)).has_value());
    }
    auto global = entry();
    global.scope_utf8.clear();
    REQUIRE(agent::reduce_memory_event({}, upsert(global)).has_value());
}

TEST_CASE(memory_reducer_rejects_immutable_updates_and_unchanged_or_old_content) {
    auto state = agent::reduce_memory_event({}, upsert());
    REQUIRE(state.has_value());
    const auto original = state.value();
    const std::vector<std::function<void(agent::MemoryEntry &)>> mutations{
        [](auto &e) { e.category = agent::MemoryCategory::Decision; },
        [](auto &e) { e.scope_utf8.clear(); },
        [](auto &e) { e.source_session_id.back() = '2'; },
        [](auto &e) { e.source_turn_start = 2; },
        [](auto &e) { e.source_turn_end = 3; },
        [](auto &e) { e.created_at_utc = "2026-09-07T09:00:00.000Z"; },
        [](auto &e) { e.origin = agent::MemoryOrigin::ModelConsolidation; },
        [](auto &e) { e.updated_at_utc = t1; },
        [](auto &e) { e.content = "Use C++17"; }};
    for (const auto &mutate : mutations) {
        auto e = entry();
        e.content = "Use C++20";
        e.updated_at_utc = t2;
        mutate(e);
        const agent::MemoryEvent event{1, 2, t2, "corr-update",
                                       agent::MemoryUpsertedPayload{e}};
        REQUIRE(!agent::reduce_memory_event(state.value(), event).has_value());
        REQUIRE(state.value() == original);
    }
}

TEST_CASE(memory_reducer_checks_global_order_identity_and_checkpoint_progress) {
    auto first = upsert();
    for (const auto &time : {"", "2026-13-01T00:00:00Z", "2026-09-07T10:00:00+00:00"}) {
        auto bad = first;
        bad.timestamp_utc = time;
        REQUIRE(!agent::reduce_memory_event({}, bad).has_value());
    }
    auto bad = first;
    bad.schema_version = 2;
    REQUIRE(!agent::reduce_memory_event({}, bad).has_value());
    bad = first;
    bad.sequence = 0;
    REQUIRE(!agent::reduce_memory_event({}, bad).has_value());
    bad = first;
    bad.correlation_id = "";
    REQUIRE(!agent::reduce_memory_event({}, bad).has_value());
    bad = first;
    bad.correlation_id = "\xFF";
    REQUIRE(!agent::reduce_memory_event({}, bad).has_value());
    const agent::MemoryEvent checkpoint{
        1, 1, t1, "corr", agent::SessionMemoryConsolidatedPayload{session, 3}};
    auto state = agent::reduce_memory_event({}, checkpoint);
    REQUIRE(state.has_value());
    for (const auto through : {0ULL, 2ULL, 3ULL}) {
        auto next = checkpoint;
        next.sequence = 2;
        std::get<agent::SessionMemoryConsolidatedPayload>(next.payload).through_turn =
            through;
        REQUIRE(!agent::reduce_memory_event(state.value(), next).has_value());
    }
    auto next = checkpoint;
    next.sequence = 2;
    std::get<agent::SessionMemoryConsolidatedPayload>(next.payload).through_turn = 4;
    REQUIRE(agent::reduce_memory_event(state.value(), next).has_value());
    next.sequence = 3;
    REQUIRE(!agent::reduce_memory_event(state.value(), next).has_value());
    next.sequence = 2;
    next.timestamp_utc = "2026-09-06T10:00:00.000Z";
    REQUIRE(!agent::reduce_memory_event(state.value(), next).has_value());
    next = checkpoint;
    std::get<agent::SessionMemoryConsolidatedPayload>(next.payload).session_id = "bad";
    REQUIRE(!agent::reduce_memory_event({}, next).has_value());
    next = {1, 1, t1, "corr", agent::MemoryForgottenPayload{id}};
    REQUIRE(!agent::reduce_memory_event({}, next).has_value());
    auto overflow = state.value();
    overflow.last_sequence = std::numeric_limits<std::uint64_t>::max();
    next.sequence = 0;
    REQUIRE(!agent::reduce_memory_event(overflow, next).has_value());
}
