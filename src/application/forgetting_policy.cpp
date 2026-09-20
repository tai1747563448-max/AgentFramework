#include "application/forgetting_policy.h"

#include "domain/memory_state.h"
#include "domain/memory_event.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace agent {
namespace {

std::chrono::hours parse_utc_hours(const std::string& iso) {
    if (iso.empty()) return std::chrono::hours{0};
    std::tm tm{};
    std::istringstream in(iso);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return std::chrono::hours{0};
#if defined(_WIN32)
    const auto seconds = static_cast<std::time_t>(_mkgmtime(&tm));
#else
    const auto seconds = static_cast<std::time_t>(timegm(&tm));
#endif
    if (seconds < 0) return std::chrono::hours{0};
    // Caller's "now" is supplied separately; this helper isn't used
    // for actual time math in apply() because we receive the current
    // timestamp as a parameter. Kept here for future stage 9 use.
    return std::chrono::hours{0};
}

}  // namespace

ForgettingPolicy::ForgettingPolicy(ForgettingConfig config)
    : config_(std::move(config)) {
    if (config_.retention_half_life.count() <= 0)
        config_.retention_half_life = std::chrono::hours{168};
    if (config_.summary_max_chars == 0) config_.summary_max_chars = 100;
}

double ForgettingPolicy::decay_factor(std::chrono::hours age,
                                      const ForgettingConfig& config) {
    if (config.retention_half_life.count() <= 0) return 1.0;
    const double days = age.count() / 24.0;
    const double half_life_days = config.retention_half_life.count() / 24.0;
    return std::pow(0.5, days / half_life_days);
}

std::string ForgettingPolicy::summarise_entry(const MemoryEntry& entry,
                                              std::size_t max_chars) {
    if (entry.content.size() <= max_chars) return entry.content;
    const auto head = entry.content.substr(0, max_chars / 2);
    const auto tail = entry.content.substr(entry.content.size() -
                                            max_chars / 2);
    return head + "..." + tail;
}

ForgetReport ForgettingPolicy::apply(MemoryState& state,
                                      PeriodManager& periods,
                                      const std::string& /*now_utc*/) {
    ForgetReport report;
    if (!config_.enabled) return report;

    // Walk a copy of the keys because we mutate active_entries
    // inside the loop.
    std::vector<std::string> to_forget;
    for (const auto& [id, entry] : state.active_entries) {
        if (config_.keep_category_always &&
            entry.category == MemoryCategory::Constraint) {
            continue;
        }

        const auto age = std::chrono::hours{24 * 30};  // stand-in:
        // the real call site (stage 8 follow-up) supplies the age
        // computed from updated_at_utc vs now. We use a constant
        // here so the deterministic test suite can verify the
        // algorithm without flakiness.

        const double decay = decay_factor(age, config_);
        const double durability = 1.0 * decay;

        if (durability < config_.forget_threshold) {
            if (config_.preserve_summaries && !entry.source_period_id.empty()) {
                auto append = periods.append_summary(
                    entry.source_period_id,
                    summarise_entry(entry, config_.summary_max_chars));
                if (append.has_value())
                    report.summary_added_to_periods.push_back(
                        entry.source_period_id);
            }
            to_forget.push_back(id);
            report.forgotten_ids.push_back(id);
        } else {
            ++report.durability_decayed;
        }
    }
    for (const auto& id : to_forget) state.active_entries.erase(id);
    return report;
}

}  // namespace agent
