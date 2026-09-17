#pragma once

#include "domain/evidence_pack.h"

#include <cstddef>
#include <optional>
#include <string>

namespace agent {

// Composite key for the per-task evidence cache. The hash covers every input
// that can change the evidence returned for a query so a stale entry cannot
// leak across:
//
//   - task_id:        one cache slot per task; the cache resets when the
//                     task ends.
//   - workspace:      the corpus a workspace imports is dynamic, so the
//                     cache must be invalidated across workspaces.
//   - query:          the literal prompt issued by the user.
//   - retrieval_revision: the sidecar publishes a revision id with every
//                     ready frame; a new pack or fresh index invalidates
//                     every cached entry.
//   - mode / top_k / max_total_bytes: query-time knobs that change
//                     retrieval ordering and the byte cap.
struct EvidenceCacheKey {
    std::string task_id;
    std::string workspace;
    std::string query;
    std::string retrieval_revision;
    std::string mode;
    std::size_t top_k{0};
    std::size_t max_total_bytes{0};
    bool operator==(const EvidenceCacheKey& other) const noexcept;
};

}  // namespace agent

namespace std {
template <>
struct hash<agent::EvidenceCacheKey> {
    std::size_t operator()(const agent::EvidenceCacheKey& key) const noexcept;
};
}  // namespace std

namespace agent {

// TaskEvidenceCache keeps at most one evidence pack per task in flight. The
// plan forbids a global cross-task cache; this implementation honours the
// limit by storing a single optional pack and using a hash-keyed lookup only
// to defend against accidental multi-slot reuse.
class TaskEvidenceCache {
public:
    // Look up an entry; nullopt when the key does not match or no entry is
    // present. The caller still receives the returned value before the
    // lease/revision guard in PersistentRagKnowledgeProvider is consulted;
    // the cache key is constructed with the live revision at lookup time.
    std::optional<EvidencePack> lookup(const EvidenceCacheKey& key) const;

    // Insert or replace the entry. The lease is the verifier-provided
    // retrieval revision; the cache stores it alongside the value so a
    // stale read can be detected even when the verifier has not yet
    // observed the pack change.
    void store(const EvidenceCacheKey& key, EvidencePack pack);

    // Drop the entry unconditionally. Called when the task ends or the
    // pack verifier rotates.
    void clear() noexcept;

    // Number of stored entries. Always 0 or 1 today; the helper exists so
    // tests can assert eviction behaviour.
    std::size_t size() const noexcept;

    // Maximum allowed total evidence bytes. The cache rejects inserts
    // whose evidence exceeds the limit; the limit is exposed so the
    // caller can pre-flight without writing.
    static constexpr std::size_t kMaxEvidenceBytes = 32'768;

private:
    std::optional<EvidenceCacheKey> slot_key_;
    std::optional<EvidencePack> slot_value_;
};

}  // namespace agent
