#include "adapters/persistence/jsonl_period_store.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

agent::Period make_period(const std::string& id, agent::PeriodState state,
                          const std::string& summary = "") {
    agent::Period p;
    p.period_id = id;
    p.started_at_utc = "2026-09-15T10:00:00.000Z";
    p.ended_at_utc = (state == agent::PeriodState::Closed)
                         ? "2026-09-15T11:00:00.000Z" : "";
    p.state = state;
    p.session_ids = {"session-aaaaaaaaaaaaaaaa", "session-bbbbbbbbbbbbbbbb"};
    p.episodic_summary = summary;
    p.sessions_count = p.session_ids.size();
    p.tokens_in_period = 1234;
    return p;
}

}  // namespace

TEST_CASE(period_store_missing_snapshot_returns_empty_list) {
    test::ScopedTempDir temp("period-empty");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    const auto loaded = store.read_all();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().empty());
}

TEST_CASE(period_store_round_trips_a_single_open_period) {
    test::ScopedTempDir temp("period-single");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    const auto original = make_period(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", agent::PeriodState::Open,
        "first summary");
    REQUIRE(store.write_all({original}).has_value());
    const auto loaded = store.read_all();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().size() == 1);
    REQUIRE(loaded.value().at(0) == original);
}

TEST_CASE(period_store_round_trips_multiple_periods_preserving_state) {
    test::ScopedTempDir temp("period-multi");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    const auto open = make_period(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", agent::PeriodState::Open);
    const auto closed = make_period(
        "period-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", agent::PeriodState::Closed,
        "all done");
    REQUIRE(store.write_all({open, closed}).has_value());
    const auto loaded = store.read_all();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().size() == 2);
    bool saw_open = false, saw_closed = false;
    for (const auto& p : loaded.value()) {
        if (p.period_id == open.period_id) {
            saw_open = true;
            REQUIRE(p.state == agent::PeriodState::Open);
        }
        if (p.period_id == closed.period_id) {
            saw_closed = true;
            REQUIRE(p.state == agent::PeriodState::Closed);
            REQUIRE(p.ended_at_utc == "2026-09-15T11:00:00.000Z");
        }
    }
    REQUIRE(saw_open);
    REQUIRE(saw_closed);
}

TEST_CASE(period_store_overwrite_replaces_previous_snapshot) {
    test::ScopedTempDir temp("period-overwrite");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    REQUIRE(store.write_all(
                {make_period("period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                             agent::PeriodState::Open)}).has_value());
    REQUIRE(store.write_all({}).has_value());
    const auto loaded = store.read_all();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().empty());
}

TEST_CASE(period_store_corrupt_snapshot_returns_failure) {
    test::ScopedTempDir temp("period-corrupt");
    const auto root = temp.path() / "runtime";
    agent::JsonlPeriodStore store(root);
    REQUIRE(store.write_all({make_period(
                                "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                agent::PeriodState::Open)}).has_value());

    // Truncate the snapshot to simulate a half-written file.
    const auto path = store.snapshot_path();
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "{";

    const auto loaded = store.read_all();
    REQUIRE(!loaded.has_value());
}
