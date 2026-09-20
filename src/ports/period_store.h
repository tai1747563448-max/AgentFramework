#pragma once

#include "domain/period_state.h"
#include "domain/result.h"

#include <filesystem>
#include <optional>
#include <vector>

namespace agent {

// T2 (stage 2): PeriodStore is the persistence port for Period records.
// The implementation is allowed to write the full snapshot on every
// mutation — Period counts are small (a handful per workspace lifetime),
// so a periodic JSONL rewrite is cheaper than an append-log + replay
// reducer for this domain object.
class PeriodStore {
public:
    virtual ~PeriodStore() = default;

    // Replace the on-disk snapshot with the supplied set. Used both for
    // first-time writes and for periodic compaction of the file.
    virtual Result<void> write_all(
        const std::vector<Period>& periods) = 0;

    // Read all Periods from disk. Returns an empty vector on a
    // missing/empty file (a fresh install is not an error).
    virtual Result<std::vector<Period>> read_all() = 0;

    // Convenience: where the snapshot file lives. Tests assert on this
    // path; the CLI never reads it directly.
    virtual std::filesystem::path snapshot_path() const = 0;
};

}  // namespace agent
