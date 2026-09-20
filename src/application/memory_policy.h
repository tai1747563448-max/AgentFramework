#pragma once

#include "domain/result.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

// T1 (stage 1): policy that decides whether a periodic consolidation
// pass is allowed to run. All thresholds have sensible defaults so the
// existing tests, which never construct this struct, keep working.
//
// `enabled` is the per-call kill switch; `long_term_memory` is the
// framework-level "do we even have cross-session memory?" master switch
// that lives one level up on MemoryPolicyConfig. The two are split so
// users can opt out of consolidation entirely (DeepSeek mode) without
// losing the rest of the memory infrastructure.
struct ConsolidationTriggers {
    bool enabled{true};
    bool trigger_on_topic_divergence{true};   // T1
    bool trigger_on_token_threshold{true};    // T2
    bool trigger_on_time_elapsed{true};       // T3
    std::size_t token_threshold{50'000};
    std::chrono::hours time_elapsed{24};
    std::chrono::hours rate_limit{24};
    // Lower bound: even if no other trigger fires, fire at least once
    // every `time_elapsed` so a low-throughput session still gets
    // curated eventually.
    bool force_periodic_floor{true};
    // T1 threshold on the topic-divergence signal in [0, 1].
    double topic_divergence_threshold{0.7};
};

struct MemoryPolicyConfig {
    std::size_t max_entry_bytes{4096};
    std::vector<std::string> protected_values;
    // Master switch: when false, build_engine skips the consolidator
    // entirely (DeepSeek-style event-log mode).
    bool long_term_memory{true};
    ConsolidationTriggers consolidation{};
};

class MemoryPolicy {
public:
    explicit MemoryPolicy(MemoryPolicyConfig config);

    Result<void> validate_candidate(std::string_view content) const;

private:
    MemoryPolicyConfig config_;
};

bool memory_opted_out(std::string_view text);

} // namespace agent
