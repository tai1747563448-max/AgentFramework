#pragma once

#include "domain/memory_state.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent {

// Stage 6: retrieval-coefficient allocation. The contract:
//   * Sum of all Active facts' coefficients == 1.0 (always).
//   * Decay factor grows with access_count (spaced repetition).
//   * Newly committed facts get a temporary boost so they don't
//     sink to the bottom on day one.
//
// This is pure logic; the caller supplies the clock. Storage layer
// just holds the per-fact coefficient on MemoryEntry.
struct RetrievalCoefficientConfig {
    double initial_difficulty{0.5};
    double per_retrieval_decrement{0.05};
    std::size_t curate_boost_periods{3};
    double curate_boost_amount{0.02};
    double initial_half_life_days{7.0};
    double half_life_growth_rate{0.916};
    double max_half_life_days{365.0};
};

class RetrievalCoefficientAllocator {
public:
    explicit RetrievalCoefficientAllocator(RetrievalCoefficientConfig config = {});

    // Allocate coefficients for a freshly committed batch of facts.
    // New facts start at the configured initial coefficient; all
    // existing entries are renormalised to keep the global sum at 1.
    void on_commit(std::vector<MemoryEntry>& entries,
                   const std::vector<std::string>& new_ids);

    // Apply one normalisation pass: every entry drifts toward the
    // mean with a decay factor derived from access_count.
    void normalize(std::vector<MemoryEntry>& entries);

    // Record a retrieval hit. Decreases difficulty and bumps
    // access_count. Caller must renormalise afterwards.
    void on_retrieval(MemoryEntry& entry);

    static double decay_factor(std::uint32_t access_count,
                               const RetrievalCoefficientConfig& config);

    static double mean_coefficient(std::size_t active_count);

private:
    RetrievalCoefficientConfig config_;
};

}  // namespace agent
