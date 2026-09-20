#include "application/period_probe.h"
#include "domain/period_state.h"
#include "test_support.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

agent::Period make_open(const std::string& id, const std::string& summary) {
    agent::Period p;
    p.period_id = id;
    p.started_at_utc = "2026-09-15T10:00:00.000Z";
    p.state = agent::PeriodState::Open;
    p.episodic_summary = summary;
    return p;
}

}  // namespace

TEST_CASE(period_probe_returns_no_attach_when_no_open_periods) {
    agent::PeriodProbe probe;
    const auto decision = probe.decide({}, "anything goes here", {});
    REQUIRE(!decision.attach);
    REQUIRE(decision.period_id.empty());
}

TEST_CASE(period_probe_attach_when_prompt_closely_matches_summary) {
    // Strong overlap: the prompt and summary share most tokens.
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "RAG architecture decision");
    const auto decision = probe.decide(
        {existing},
        "RAG architecture decision",
        {});
    REQUIRE(decision.attach);
    REQUIRE(decision.period_id == existing.period_id);
}

TEST_CASE(period_probe_no_attach_when_topics_diverge) {
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "user prefers functional programming in Rust");
    const auto decision = probe.decide(
        {existing},
        "what is the weather today",
        {});
    REQUIRE(!decision.attach);
    REQUIRE(decision.similarity < 0.3);
}

TEST_CASE(period_probe_chinese_period_attaches_chinese_prompt) {
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "用户讨论周期管理");
    const auto decision = probe.decide(
        {existing}, "周期管理细节", {});
    REQUIRE(decision.attach);
    REQUIRE(decision.period_id == existing.period_id);
}

TEST_CASE(period_probe_chinese_period_rejects_english_prompt) {
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "用户讨论长期记忆与周期管理");
    const auto decision = probe.decide(
        {existing}, "how does memory retrieval work", {});
    REQUIRE(!decision.attach);
}

TEST_CASE(period_probe_picks_highest_similarity_among_multiple_periods) {
    agent::PeriodProbe probe;
    const auto unrelated = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "user likes dark roast coffee");
    const auto related = make_open(
        "period-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "RAG retrieval augmented generation");
    const auto decision = probe.decide(
        {unrelated, related},
        "RAG retrieval augmented generation",
        {});
    REQUIRE(decision.attach);
    REQUIRE(decision.period_id == related.period_id);
}

TEST_CASE(period_probe_respects_lower_threshold_for_attachment) {
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "we discussed TDD workflow");
    // One shared token: TDD. Lower the threshold to force attach.
    agent::PeriodConfig low_threshold;
    low_threshold.auto_attach_threshold = 0.1;
    const auto decision = probe.decide(
        {existing}, "TDD workflow",
        low_threshold);
    REQUIRE(decision.attach);
}

TEST_CASE(period_probe_invalid_utf8_prompt_returns_no_attach) {
    agent::PeriodProbe probe;
    const auto existing = make_open(
        "period-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "anything");
    std::string broken;
    broken.push_back(static_cast<char>(0xff));
    broken.push_back('a');
    const auto decision = probe.decide({existing}, broken, {});
    REQUIRE(!decision.attach);
}
