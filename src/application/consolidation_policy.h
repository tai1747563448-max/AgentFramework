#pragma once

#include "application/memory_policy.h"
#include "domain/result.h"

#include <chrono>
#include <cstddef>
#include <string>

namespace agent {

// T1 (stage 1): the periodic-consolidation decision lives in its own
// type so it is independently testable and doesn't pollute
// MemoryMaintenanceScheduler (which owns the worker thread + commit
// machinery). evaluate() is pure: no clocks, no I/O — caller passes the
// "now" timestamp and the relevant state slice.
class ConsolidationPolicy {
public:
    enum class Reason {
        None,                     // no trigger fired
        DisabledByConfig,         // master/sub switch off
        Throttled,                // rate_limit not elapsed
        TokenThresholdExceeded,   // T2
        TimeElapsedSinceLast,     // T3
        FloorForced,              // time_elapsed upper bound
        FirstEver,                // cold-start shortcut
        TopicDivergence,          // T1: subject drifted past threshold
    };

    struct Decision {
        bool should_fire{false};
        Reason reason{Reason::None};
        std::string detail;
    };

    Decision evaluate(const ConsolidationTriggers& config,
                      const std::string& last_consolidated_at_utc,
                      std::size_t tokens_since_last_consolidation,
                      std::chrono::system_clock::time_point now,
                      double topic_divergence = 0.0) const;

    static const char* reason_name(Reason reason);
};

} // namespace agent
