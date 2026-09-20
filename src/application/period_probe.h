#pragma once

#include "domain/period_state.h"
#include "domain/result.h"

#include <string>
#include <vector>

namespace agent {

// T3 (stage 3): PeriodProbe decides whether a brand-new session
// belongs to an existing open Period or should start a new one. The
// heuristic is weighted-Jaccard over the prompt's tokens vs the
// Period's episodic summary token set. Configurable threshold; the
// default 0.3 leans towards attaching rather than starting fresh.
//
// Stage 3 keeps this as a pure function. SessionEngine integration
// (calling the probe, attaching the session id, writing the summary
// pool update) lands in stage 4 along with the SessionStore change.
struct PeriodConfig {
    double auto_attach_threshold{0.3};
};

class PeriodProbe {
public:
    struct AttachDecision {
        bool attach{false};
        std::string period_id;        // populated when attach == true
        double similarity{0.0};
        std::string reason;
    };

    AttachDecision decide(const std::vector<Period>& open_periods,
                          const std::string& current_prompt,
                          const PeriodConfig& config) const;
};

}  // namespace agent
