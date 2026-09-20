#include "adapters/persistence/jsonl_period_store.h"
#include "application/period_manager.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

// Deterministic clock + id so tests are reproducible.
class FixedClock final : public agent::Clock {
public:
    explicit FixedClock(std::string fixed_utc) : fixed_(std::move(fixed_utc)) {}
    std::string now_utc() const override { return fixed_; }
    std::int64_t monotonic_ms() const override { return 0; }
    void set(const std::string& value) { fixed_ = value; }

private:
    std::string fixed_;
};

class CounterIdGenerator final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return "task-" + std::to_string(++counter);
    }
    std::string next_correlation_id() override {
        return "period-" + std::to_string(++counter) +
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }

private:
    std::uint64_t counter{0};
};

}  // namespace

TEST_CASE(period_manager_open_period_persists_to_disk) {
    test::ScopedTempDir temp("mgr-open");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto opened = mgr.open_period();
    REQUIRE(opened.has_value());
    REQUIRE(opened.value().period_id == "period-1aaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(opened.value().started_at_utc == "2026-09-15T10:00:00.000Z");
    REQUIRE(opened.value().state == agent::PeriodState::Open);

    // Second open gets a fresh id.
    const auto second = mgr.open_period();
    REQUIRE(second.has_value());
    REQUIRE(second.value().period_id != opened.value().period_id);
}

TEST_CASE(period_manager_attach_session_is_idempotent) {
    test::ScopedTempDir temp("mgr-attach");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto opened = mgr.open_period().value();
    const auto sid = std::string("session-11111111111111111111111111111111");
    REQUIRE(mgr.attach_session(opened.period_id, sid).has_value());
    REQUIRE(mgr.attach_session(opened.period_id, sid).has_value());

    const auto found = mgr.find(opened.period_id).value();
    REQUIRE(found.has_value());
    REQUIRE(found.value().session_ids.size() == 1);
    REQUIRE(found.value().sessions_count == 1);
}

TEST_CASE(period_manager_attach_session_rejects_unknown_period) {
    test::ScopedTempDir temp("mgr-attach-unknown");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto result = mgr.attach_session(
        "period-doesnotexist", "session-x");
    REQUIRE(!result.has_value());
}

TEST_CASE(period_manager_close_period_records_end_time_and_blocks_attach) {
    test::ScopedTempDir temp("mgr-close");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto opened = mgr.open_period().value();
    clock.set("2026-09-15T11:00:00.000Z");
    REQUIRE(mgr.close_period(opened.period_id).has_value());

    const auto after = mgr.find(opened.period_id).value();
    REQUIRE(after.has_value());
    REQUIRE(after.value().state == agent::PeriodState::Closed);
    REQUIRE(after.value().ended_at_utc == "2026-09-15T11:00:00.000Z");

    const auto attach = mgr.attach_session(opened.period_id, "session-x");
    REQUIRE(!attach.has_value());
}

TEST_CASE(period_manager_append_summary_accumulates_with_separator) {
    test::ScopedTempDir temp("mgr-summary");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto opened = mgr.open_period().value();
    REQUIRE(mgr.append_summary(opened.period_id, "first chunk").has_value());
    REQUIRE(mgr.append_summary(opened.period_id, "second chunk").has_value());

    const auto found = mgr.find(opened.period_id).value();
    REQUIRE(found.has_value());
    REQUIRE(found.value().episodic_summary ==
            "first chunk\n\nsecond chunk");
}

TEST_CASE(period_manager_accumulates_token_counter_without_disk_write) {
    test::ScopedTempDir temp("mgr-tokens");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto opened = mgr.open_period().value();
    REQUIRE(mgr.accumulate_tokens(opened.period_id, 100).has_value());
    REQUIRE(mgr.accumulate_tokens(opened.period_id, 50).has_value());

    const auto found = mgr.find(opened.period_id).value();
    REQUIRE(found.has_value());
    REQUIRE(found.value().tokens_in_period == 150);
}

TEST_CASE(period_manager_list_open_periods_excludes_closed) {
    test::ScopedTempDir temp("mgr-list");
    agent::JsonlPeriodStore store(temp.path() / "runtime");
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);

    const auto open = mgr.open_period().value();
    const auto to_close = mgr.open_period().value();
    REQUIRE(mgr.close_period(to_close.period_id).has_value());

    const auto list = mgr.list_open_periods().value();
    REQUIRE(list.size() == 1);
    REQUIRE(list.at(0).period_id == open.period_id);
}

TEST_CASE(period_manager_loads_existing_periods_on_construction) {
    test::ScopedTempDir temp("mgr-reload");
    const auto root = temp.path() / "runtime";
    {
        agent::JsonlPeriodStore writer(root);
        FixedClock clock("2026-09-15T10:00:00.000Z");
        CounterIdGenerator ids;
        agent::PeriodManager mgr(writer, clock, ids);
        REQUIRE(mgr.open_period().has_value());
    }
    // Construct a second manager on the same root: should see the
    // previously-opened period without explicit restore.
    agent::JsonlPeriodStore reader(root);
    FixedClock clock("2026-09-15T10:00:00.000Z");
    CounterIdGenerator ids;
    agent::PeriodManager mgr(reader, clock, ids);
    REQUIRE(mgr.cached_period_count() == 1);
}
