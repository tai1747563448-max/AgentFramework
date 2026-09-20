#include "application/retrieval_coefficient.h"

#include <algorithm>
#include <cmath>

namespace agent {

RetrievalCoefficientAllocator::RetrievalCoefficientAllocator(
    RetrievalCoefficientConfig config)
    : config_(std::move(config)) {
    if (config_.initial_half_life_days <= 0)
        config_.initial_half_life_days = 7.0;
    if (config_.half_life_growth_rate <= 0)
        config_.half_life_growth_rate = 0.916;
    if (config_.max_half_life_days < config_.initial_half_life_days)
        config_.max_half_life_days = 365.0;
}

double RetrievalCoefficientAllocator::mean_coefficient(
    std::size_t active_count) {
    return active_count == 0 ? 0.0 : 1.0 / static_cast<double>(active_count);
}

double RetrievalCoefficientAllocator::decay_factor(
    std::uint32_t access_count,
    const RetrievalCoefficientConfig& config) {
    // Half-life grows as log(1 + access_count * growth_rate) starting
    // from initial_half_life_days, capped at max_half_life_days.
    // decay per "tick" = 2^(-1/H_ticks).
    const auto growth = std::log1p(static_cast<double>(access_count) *
                                  config.half_life_growth_rate);
    auto h_days = config.initial_half_life_days * (1.0 + growth);
    if (h_days > config.max_half_life_days)
        h_days = config.max_half_life_days;
    // Approximate a tick = 1 day; decay factor over one tick.
    return std::pow(0.5, 1.0 / h_days);
}

void RetrievalCoefficientAllocator::on_commit(
    std::vector<MemoryEntry>& entries,
    const std::vector<std::string>& new_ids) {
    if (entries.empty()) return;
    const auto mean = mean_coefficient(entries.size());
    for (auto& e : entries) {
        // New facts receive a temporary boost relative to the mean.
        const bool is_new = std::find(new_ids.begin(), new_ids.end(),
                                       e.memory_id) != new_ids.end();
        e.retrieval_coefficient = mean;
        if (is_new) {
            e.curate_boost_remaining = config_.curate_boost_periods;
            e.retrieval_coefficient += config_.curate_boost_amount;
        }
    }
    // Renormalise to keep the global sum = 1.
    double total = 0.0;
    for (const auto& e : entries) total += e.retrieval_coefficient;
    if (total <= 0) return;
    const auto scale = 1.0 / total;
    for (auto& e : entries) e.retrieval_coefficient *= scale;
}

void RetrievalCoefficientAllocator::normalize(
    std::vector<MemoryEntry>& entries) {
    if (entries.empty()) return;
    const auto mean = mean_coefficient(entries.size());

    for (auto& e : entries) {
        // Apply decaying boost.
        if (e.curate_boost_remaining > 0) {
            --e.curate_boost_remaining;
            e.retrieval_coefficient += config_.curate_boost_amount;
        }
        const auto decay = decay_factor(e.access_count, config_);
        const auto delta = e.retrieval_coefficient - mean;
        e.retrieval_coefficient = mean + delta * decay;
    }

    // Strict renormalisation: any small drift in the sum gets corrected.
    double total = 0.0;
    for (const auto& e : entries) total += e.retrieval_coefficient;
    if (total <= 0) return;
    const auto scale = 1.0 / total;
    for (auto& e : entries) e.retrieval_coefficient *= scale;
}

void RetrievalCoefficientAllocator::on_retrieval(MemoryEntry& entry) {
    ++entry.access_count;
    entry.retrieval_difficulty =
        std::max(0.0,
                 entry.retrieval_difficulty - config_.per_retrieval_decrement);
}

}  // namespace agent
