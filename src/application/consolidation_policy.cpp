#include "application/consolidation_policy.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace agent {
namespace {

// Parse "YYYY-MM-DDTHH:MM:SS.fffZ" or "YYYY-MM-DDTHH:MM:SSZ" into a
// system_clock::time_point. Returns epoch (== never consolidated) on
// any failure so the caller can take the first-ever shortcut.
std::chrono::system_clock::time_point parse_iso8601_utc(
    const std::string& text) {
    if (text.empty())
        return {};
    std::tm tm{};
    std::istringstream stream(text);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail())
        return {};
#if defined(_WIN32)
    // timegm is a POSIX extension; Windows exposes _mkgmtime instead.
    const auto seconds = static_cast<std::time_t>(_mkgmtime(&tm));
#else
    const auto seconds = static_cast<std::time_t>(timegm(&tm));
#endif
    if (seconds < 0)
        return {};
    return std::chrono::system_clock::from_time_t(seconds);
}

} // namespace

ConsolidationPolicy::Decision ConsolidationPolicy::evaluate(
    const ConsolidationTriggers& config,
    const std::string& last_consolidated_at_utc,
    std::size_t tokens_since_last_consolidation,
    std::chrono::system_clock::time_point now,
    double topic_divergence) const {
    Decision decision{};

    if (!config.enabled) {
        decision.reason = Reason::DisabledByConfig;
        decision.detail = "consolidation.enabled = false";
        return decision;
    }

    const auto last = parse_iso8601_utc(last_consolidated_at_utc);
    const bool first_ever = last_consolidated_at_utc.empty();

    if (first_ever) {
        // Cold start: we don't apply the throttle (no baseline) and we
        // don't apply the periodic floor (would force-spend LLM cost
        // within 24h of every fresh install). Only T2 can fire.
        if (config.trigger_on_token_threshold &&
            tokens_since_last_consolidation >= config.token_threshold) {
            decision.should_fire = true;
            decision.reason = Reason::FirstEver;
            decision.detail =
                "first-ever + tokens " +
                std::to_string(tokens_since_last_consolidation) + " >= " +
                std::to_string(config.token_threshold);
        } else {
            decision.detail = "first-ever, waiting for T2";
        }
        return decision;
    }

    const auto since_last = std::chrono::duration_cast<std::chrono::hours>(
        now - last);

    if (since_last < config.rate_limit) {
        decision.reason = Reason::Throttled;
        decision.detail =
            "since_last " + std::to_string(since_last.count()) +
            "h < rate_limit " + std::to_string(config.rate_limit.count()) + "h";
        return decision;
    }

    // T1 (stage 4): topic drift over the configured threshold forces
    // a close-and-consolidate cycle even before time/token signals
    // accumulate. Caller supplies a pre-computed divergence in [0, 1].
    if (config.trigger_on_topic_divergence &&
        topic_divergence >= config.topic_divergence_threshold) {
        decision.should_fire = true;
        decision.reason = Reason::TopicDivergence;
        decision.detail =
            "topic_divergence " + std::to_string(topic_divergence) +
            " >= threshold " +
            std::to_string(config.topic_divergence_threshold);
        return decision;
    }

    if (config.trigger_on_token_threshold &&
        tokens_since_last_consolidation >= config.token_threshold) {
        decision.should_fire = true;
        decision.reason = Reason::TokenThresholdExceeded;
        decision.detail =
            "tokens " + std::to_string(tokens_since_last_consolidation) +
            " >= threshold " + std::to_string(config.token_threshold);
        return decision;
    }

    if (config.trigger_on_time_elapsed &&
        since_last >= config.time_elapsed) {
        decision.should_fire = true;
        decision.reason = Reason::TimeElapsedSinceLast;
        decision.detail =
            "since_last " + std::to_string(since_last.count()) +
            "h >= time_elapsed " + std::to_string(config.time_elapsed.count()) +
            "h";
        return decision;
    }

    if (config.force_periodic_floor && since_last >= config.time_elapsed) {
        decision.should_fire = true;
        decision.reason = Reason::FloorForced;
        decision.detail =
            "floor: since_last " + std::to_string(since_last.count()) +
            "h >= " + std::to_string(config.time_elapsed.count()) + "h";
        return decision;
    }

    decision.detail =
        "no trigger fired (tokens=" +
        std::to_string(tokens_since_last_consolidation) +
        ", since_last=" + std::to_string(since_last.count()) + "h)";
    return decision;
}

const char* ConsolidationPolicy::reason_name(Reason reason) {
    switch (reason) {
    case Reason::None:                   return "none";
    case Reason::DisabledByConfig:       return "disabled_by_config";
    case Reason::Throttled:              return "throttled";
    case Reason::TokenThresholdExceeded: return "token_threshold";
    case Reason::TimeElapsedSinceLast:   return "time_elapsed";
    case Reason::FloorForced:            return "floor_forced";
    case Reason::FirstEver:              return "first_ever";
    case Reason::TopicDivergence:        return "topic_divergence";
    }
    return "unknown";
}

} // namespace agent
