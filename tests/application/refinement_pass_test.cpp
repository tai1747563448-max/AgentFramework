#include "application/refinement_pass.h"
#include "domain/memory_state.h"
#include "test_support.h"

#include <string>
#include <vector>

namespace {

agent::MemoryEntry make_fact(const std::string& id, const std::string& content) {
    agent::MemoryEntry e;
    e.memory_id = id;
    e.category = agent::MemoryCategory::Fact;
    e.content = content;
    e.created_at_utc = "2026-09-15T10:00:00.000Z";
    e.updated_at_utc = "2026-09-15T10:00:00.000Z";
    return e;
}

}  // namespace

TEST_CASE(refinement_pass_disabled_reports_degraded) {
    agent::RefinementConfig cfg;
    cfg.enabled = false;
    agent::RefinementPass pass(cfg);
    const auto report = pass.run({}, {});
    REQUIRE(report.degraded);
}

TEST_CASE(refinement_pass_enabled_runs_empty_when_no_inputs) {
    agent::RefinementPass pass;
    const auto report = pass.run({}, {});
    REQUIRE(!report.degraded);
    REQUIRE(report.llm_calls == 0);
    REQUIRE(report.abstracted == 0);
    REQUIRE(report.deduced == 0);
}

TEST_CASE(refinement_pass_deduced_birth_year_from_zodiac_and_range) {
    agent::RefinementPass pass;
    const std::vector<agent::MemoryEntry> grounded{
        make_fact("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "用户属鸡"),
        make_fact("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                  "用户生于 2000-2010 之间"),
    };
    const auto deduced = agent::RefinementPass::find_deduced_facts(grounded, 10);
    REQUIRE(deduced.size() == 1);
    REQUIRE(deduced[0].find("2005") != std::string::npos);
}

TEST_CASE(refinement_pass_deduced_returns_empty_without_both_inputs) {
    agent::RefinementPass pass;
    // Only zodiac, no year range → not uniquely determined.
    const std::vector<agent::MemoryEntry> only_zodiac{
        make_fact("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "用户属鸡"),
    };
    REQUIRE(agent::RefinementPass::find_deduced_facts(only_zodiac, 10).empty());

    // Only year range, no zodiac → can't apply zodiac rule.
    const std::vector<agent::MemoryEntry> only_range{
        make_fact("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                  "用户生于 2000-2010 之间"),
    };
    REQUIRE(agent::RefinementPass::find_deduced_facts(only_range, 10).empty());
}

TEST_CASE(refinement_pass_deduced_respects_zero_budget) {
    agent::RefinementPass pass;
    const std::vector<agent::MemoryEntry> grounded{
        make_fact("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "用户属鸡"),
        make_fact("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                  "用户生于 2000-2010 之间"),
    };
    REQUIRE(agent::RefinementPass::find_deduced_facts(grounded, 0).empty());
}

TEST_CASE(refinement_pass_keeps_max_llm_call_budget_in_config) {
    agent::RefinementConfig cfg;
    cfg.max_llm_calls_per_period = 12;
    REQUIRE(cfg.max_llm_calls_per_period == 12);
}
