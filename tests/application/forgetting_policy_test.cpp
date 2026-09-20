#include "adapters/persistence/jsonl_period_store.h"
#include "application/forgetting_policy.h"
#include "application/period_manager.h"
#include "domain/memory_state.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "test_support.h"

#include <cstdint>
#include <string>

namespace {

class FixedClock final : public agent::Clock {
public:
    std::string now_utc() const override { return "2026-09-15T10:00:00.000Z"; }
    std::int64_t monotonic_ms() const override { return 0; }
};

class CounterIdGenerator final : public agent::IdGenerator {
public:
    std::string next_task_id() override { return "task-x"; }
    std::string next_correlation_id() override {
        return "period-" + std::to_string(++counter) +
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
private:
    std::uint64_t counter{0};
};

agent::MemoryEntry make_entry(const std::string& id,
                              agent::MemoryCategory category,
                              const std::string& period_id = "") {
    agent::MemoryEntry e;
    e.memory_id = id;
    e.category = category;
    e.content = "some content for " + id + " that goes on a bit";
    e.created_at_utc = "2026-09-01T10:00:00.000Z";
    e.updated_at_utc = "2026-09-01T10:00:00.000Z";
    e.source_period_id = period_id;
    return e;
}

}  // namespace

TEST_CASE(forgetting_policy_disabled_by_default_is_noop) {
    agent::ForgettingPolicy policy;
    agent::MemoryState state;
    state.active_entries["memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"] =
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   agent::MemoryCategory::Fact);
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    const auto report = policy.apply(state, mgr, "2026-09-15T10:00:00.000Z");
    REQUIRE(report.forgotten_ids.empty());
    REQUIRE(state.active_entries.size() == 1);
}

TEST_CASE(forgetting_policy_constraint_never_forgotten_when_keep_on) {
    agent::ForgettingConfig cfg;
    cfg.enabled = true;
    cfg.keep_category_always = true;
    agent::ForgettingPolicy policy(cfg);
    agent::MemoryState state;
    state.active_entries["memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"] =
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   agent::MemoryCategory::Constraint);
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    const auto report = policy.apply(state, mgr, "2026-09-15T10:00:00.000Z");
    REQUIRE(report.forgotten_ids.empty());
    REQUIRE(state.active_entries.size() == 1);
}

TEST_CASE(forgetting_policy_decays_entries_below_threshold) {
    agent::ForgettingConfig cfg;
    cfg.enabled = true;
    cfg.preserve_summaries = false;
    agent::ForgettingPolicy policy(cfg);
    agent::MemoryState state;
    state.active_entries["memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"] =
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   agent::MemoryCategory::Fact);
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    const auto report = policy.apply(state, mgr, "2026-09-15T10:00:00.000Z");
    REQUIRE(report.forgotten_ids.size() == 1);
    REQUIRE(state.active_entries.empty());
}

TEST_CASE(forgetting_policy_preserves_summary_on_forget) {
    agent::ForgettingConfig cfg;
    cfg.enabled = true;
    cfg.preserve_summaries = true;
    cfg.summary_max_chars = 40;
    agent::ForgettingPolicy policy(cfg);
    agent::MemoryState state;
    state.active_entries["memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"] =
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   agent::MemoryCategory::Fact,
                   "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    const auto period = mgr.open_period().value();

    // Re-create the entry with the now-existing period id so the
    // summary has somewhere to land.
    state.active_entries["memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"]
        .source_period_id = period.period_id;

    const auto report = policy.apply(state, mgr, "2026-09-15T10:00:00.000Z");
    REQUIRE(report.forgotten_ids.size() == 1);
    REQUIRE(report.summary_added_to_periods.size() == 1);

    const auto found = mgr.find(period.period_id).value();
    REQUIRE(found.has_value());
    REQUIRE(!found.value().episodic_summary.empty());
}

TEST_CASE(forgetting_decay_factor_halves_at_one_half_life) {
    agent::ForgettingConfig cfg;
    cfg.retention_half_life = std::chrono::hours{168};  // 7 days
    const auto f = agent::ForgettingPolicy::decay_factor(
        std::chrono::hours{168}, cfg);
    REQUIRE(std::abs(f - 0.5) < 1e-9);
}

TEST_CASE(forgetting_summarise_entry_truncates_long_content) {
    const auto entry = make_entry(
        "memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        agent::MemoryCategory::Fact);
    const auto summary =
        agent::ForgettingPolicy::summarise_entry(entry, 30);
    REQUIRE(summary.size() <= 60);  // 15 head + 3 dots + 15 tail
    REQUIRE(summary.find("...") != std::string::npos);
}
