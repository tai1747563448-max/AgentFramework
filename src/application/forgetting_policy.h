#pragma once

#include "application/period_manager.h"
#include "domain/memory_state.h"
#include "domain/result.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace agent {

// Stage 8: ForgettingPolicy decides when a MemoryEntry should be
// forgotten, and what summary (if any) should be retained at the
// Period level. Default is OFF — the user must opt in via the
// long_term_memory / forgetting config block.
//
// Stage 8 ships the algorithm and the tests; LLM-based summary
// generation is left as a follow-up — the policy currently emits a
// deterministic extract of the entry's content (head + tail, like
// SessionSummarizer) so the behaviour is fully testable without
// model calls.
struct ForgettingConfig {
    bool enabled{false};
    bool preserve_summaries{true};
    std::chrono::hours retention_half_life{168};     // 7 days
    double forget_threshold{0.1};
    bool keep_category_always{true};                 // Constraint never forgotten
    std::size_t summary_max_chars{100};
};

struct ForgetReport {
    std::vector<std::string> forgotten_ids;
    std::vector<std::string> summary_added_to_periods;
    std::size_t durability_decayed{0};
};

class ForgettingPolicy {
public:
    explicit ForgettingPolicy(ForgettingConfig config = {});

    ForgetReport apply(MemoryState& state,
                       PeriodManager& periods,
                       const std::string& now_utc_iso8601);

    static double decay_factor(std::chrono::hours age,
                               const ForgettingConfig& config);

    static std::string summarise_entry(const MemoryEntry& entry,
                                       std::size_t max_chars);

private:
    ForgettingConfig config_;
};

}  // namespace agent
