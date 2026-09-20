#include "application/consolidation_policy.h"
#include "application/memory_policy.h"
#include "domain/memory_state.h"
#include "test_support.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>

namespace {

using agent::ConsolidationPolicy;
using agent::ConsolidationTriggers;

// Helper: a fixed "now" anchor so duration math is deterministic.
const std::chrono::system_clock::time_point kAnchor =
    std::chrono::system_clock::from_time_t(1'700'000'000);

std::string iso8601(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);  // Windows: errno_t gmtime_s(tm*, const time_t*)
#else
    gmtime_r(&t, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm);
    return buffer;
}

}  // namespace

TEST_CASE(consolidation_returns_disabled_when_master_switch_off) {
    ConsolidationTriggers config{};
    config.enabled = false;
    const ConsolidationPolicy policy;
    const auto decision = policy.evaluate(
        config, "2026-09-18T00:00:00", 1000, kAnchor);
    REQUIRE(!decision.should_fire);
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::DisabledByConfig);
}

TEST_CASE(consolidation_first_ever_only_fires_on_token_threshold) {
    ConsolidationTriggers config{};
    config.token_threshold = 100;
    const ConsolidationPolicy policy;

    // below threshold: stay idle
    auto d1 = policy.evaluate(config, "", 50, kAnchor);
    REQUIRE(!d1.should_fire);
    REQUIRE(d1.reason == ConsolidationPolicy::Reason::None);

    // at threshold: fire
    auto d2 = policy.evaluate(config, "", 100, kAnchor);
    REQUIRE(d2.should_fire);
    REQUIRE(d2.reason == ConsolidationPolicy::Reason::FirstEver);
}

TEST_CASE(consolidation_throttles_when_within_rate_limit) {
    ConsolidationTriggers config{};
    config.token_threshold = 100;
    config.rate_limit = std::chrono::hours{24};
    config.time_elapsed = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{1};
    const auto decision = policy.evaluate(
        config, iso8601(last), 9999, kAnchor);  // tokens way over threshold

    REQUIRE(!decision.should_fire);
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::Throttled);
}

TEST_CASE(consolidation_fires_t2_outside_rate_limit) {
    ConsolidationTriggers config{};
    config.token_threshold = 100;
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{48};
    const auto decision = policy.evaluate(
        config, iso8601(last), 500, kAnchor);

    REQUIRE(decision.should_fire);
    REQUIRE(decision.reason ==
            ConsolidationPolicy::Reason::TokenThresholdExceeded);
}

TEST_CASE(consolidation_fires_t3_when_time_elapsed) {
    ConsolidationTriggers config{};
    config.token_threshold = 1'000'000;  // unreachable
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{25};
    const auto decision = policy.evaluate(
        config, iso8601(last), 10, kAnchor);

    REQUIRE(decision.should_fire);
    REQUIRE(decision.reason ==
            ConsolidationPolicy::Reason::TimeElapsedSinceLast);
}

TEST_CASE(consolidation_fires_floor_even_when_other_triggers_off) {
    ConsolidationTriggers config{};
    config.trigger_on_token_threshold = false;
    config.trigger_on_time_elapsed = false;
    config.force_periodic_floor = true;
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{72};
    const auto decision = policy.evaluate(
        config, iso8601(last), 1, kAnchor);

    REQUIRE(decision.should_fire);
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::FloorForced);
}

TEST_CASE(consolidation_idle_when_floor_disabled_and_no_other_trigger) {
    ConsolidationTriggers config{};
    config.token_threshold = 1'000'000;
    config.trigger_on_token_threshold = true;
    config.trigger_on_time_elapsed = false;
    config.force_periodic_floor = false;
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{1};  // short throttle so we get past it

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{2};
    const auto decision = policy.evaluate(
        config, iso8601(last), 5, kAnchor);

    REQUIRE(!decision.should_fire);
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::None);
}

TEST_CASE(consolidation_t2_takes_priority_over_t3_when_both_eligible) {
    ConsolidationTriggers config{};
    config.token_threshold = 100;
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{48};
    const auto decision = policy.evaluate(
        config, iso8601(last), 500, kAnchor);

    REQUIRE(decision.should_fire);
    // T2 is evaluated first, so it wins.
    REQUIRE(decision.reason ==
            ConsolidationPolicy::Reason::TokenThresholdExceeded);
}

TEST_CASE(consolidation_invalid_timestamp_falls_through_to_t3) {
    // A malformed timestamp can't be trusted, so the parser returns epoch
    // and the policy treats it as "very long ago" — T3 naturally fires.
    ConsolidationTriggers config{};
    config.token_threshold = 1'000'000;  // T2 unreachable
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto decision =
        policy.evaluate(config, "not-a-valid-timestamp", 5, kAnchor);
    REQUIRE(decision.should_fire);
    REQUIRE(decision.reason ==
            ConsolidationPolicy::Reason::TimeElapsedSinceLast);
}

TEST_CASE(consolidation_reason_name_returns_stable_strings) {
    REQUIRE(std::string(ConsolidationPolicy::reason_name(
                ConsolidationPolicy::Reason::None)) == "none");
    REQUIRE(std::string(ConsolidationPolicy::reason_name(
                ConsolidationPolicy::Reason::TokenThresholdExceeded)) ==
            "token_threshold");
    REQUIRE(std::string(ConsolidationPolicy::reason_name(
                ConsolidationPolicy::Reason::FloorForced)) == "floor_forced");
}

TEST_CASE(consolidation_t1_fires_on_topic_divergence) {
    ConsolidationTriggers config{};
    config.token_threshold = 1'000'000;  // T2 unreachable
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{48};
    const auto decision = policy.evaluate(
        config, iso8601(last), 1, kAnchor,
        /*topic_divergence=*/0.8);

    REQUIRE(decision.should_fire);
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::TopicDivergence);
}

TEST_CASE(consolidation_t1_does_not_fire_below_threshold) {
    ConsolidationTriggers config{};
    config.token_threshold = 1'000'000;
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{1};  // throttled anyway
    const auto decision = policy.evaluate(
        config, iso8601(last), 1, kAnchor,
        /*topic_divergence=*/0.3);

    // Below threshold AND throttled → no fire.
    REQUIRE(!decision.should_fire);
}

TEST_CASE(consolidation_t1_takes_priority_when_threshold_exceeded) {
    ConsolidationTriggers config{};
    config.token_threshold = 100;       // T2 also eligible
    config.time_elapsed = std::chrono::hours{24};
    config.rate_limit = std::chrono::hours{24};

    const ConsolidationPolicy policy;
    const auto last = kAnchor - std::chrono::hours{48};
    const auto decision = policy.evaluate(
        config, iso8601(last), 500, kAnchor,
        /*topic_divergence=*/0.9);

    REQUIRE(decision.should_fire);
    // T1 is evaluated before T2, so it wins.
    REQUIRE(decision.reason == ConsolidationPolicy::Reason::TopicDivergence);
}

TEST_CASE(memory_state_default_construction_zeroes_new_fields) {
    agent::MemoryState state;
    REQUIRE(state.last_consolidated_at_utc.empty());
    REQUIRE(state.tokens_since_last_consolidation == 0);
}

TEST_CASE(memory_policy_config_aggregate_init_still_works) {
    // Regression guard: existing call sites use {max_bytes, {protected}}
    // brace initialization. Adding fields must not break them.
    const agent::MemoryPolicy policy({4096, {}});
    const auto result = policy.validate_candidate("hello world");
    REQUIRE(result.has_value());
}
