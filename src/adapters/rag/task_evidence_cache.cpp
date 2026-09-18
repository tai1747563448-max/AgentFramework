#include "adapters/rag/task_evidence_cache.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>

namespace agent {

bool EvidenceCacheKey::operator==(const EvidenceCacheKey& other) const noexcept {
    return task_id == other.task_id && workspace == other.workspace &&
           query == other.query && retrieval_revision == other.retrieval_revision &&
           mode == other.mode && top_k == other.top_k &&
           max_total_bytes == other.max_total_bytes;
}

bool EvidenceCacheKey::operator!=(const EvidenceCacheKey& other) const noexcept {
    return !(*this == other);
}

}  // namespace agent

namespace std {

std::size_t hash<agent::EvidenceCacheKey>::operator()(
    const agent::EvidenceCacheKey& key) const noexcept {
    auto mix = [](std::size_t seed, std::size_t value) noexcept {
        return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
    };
    std::size_t seed = std::hash<std::string>{}(key.task_id);
    seed = mix(seed, std::hash<std::string>{}(key.workspace));
    seed = mix(seed, std::hash<std::string>{}(key.query));
    seed = mix(seed, std::hash<std::string>{}(key.retrieval_revision));
    seed = mix(seed, std::hash<std::string>{}(key.mode));
    seed = mix(seed, std::hash<std::size_t>{}(key.top_k));
    seed = mix(seed, std::hash<std::size_t>{}(key.max_total_bytes));
    return seed;
}

}  // namespace std

namespace agent {

std::optional<EvidencePack> TaskEvidenceCache::lookup(
    const EvidenceCacheKey& key) const {
    if (!slot_key_.has_value() || !slot_value_.has_value()) return std::nullopt;
    if (*slot_key_ != key) return std::nullopt;
    return slot_value_;
}

void TaskEvidenceCache::store(const EvidenceCacheKey& key, EvidencePack pack) {
    // Honour the byte cap from the plan. Larger packs must not enter the
    // cache so the cached value cannot exceed the per-turn budget.
    std::size_t bytes = 0;
    for (const auto& item : pack.items) bytes += item.source_id.size() + item.content.size();
    if (bytes > kMaxEvidenceBytes) {
        clear();
        return;
    }
    slot_key_ = key;
    slot_value_ = std::move(pack);
}

void TaskEvidenceCache::clear() noexcept {
    slot_key_.reset();
    slot_value_.reset();
}

std::size_t TaskEvidenceCache::size() const noexcept {
    return slot_key_.has_value() ? 1 : 0;
}

}  // namespace agent
