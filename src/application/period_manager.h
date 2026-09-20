#pragma once

#include "domain/period_state.h"
#include "domain/result.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class Clock;
class IdGenerator;
class PeriodStore;

// T2 (stage 2): PeriodManager is the only place that mutates Period
// records. It keeps an in-memory index for fast lookups and flushes to
// the PeriodStore on every write (the snapshot is small and writes are
// rare). Concurrency is single-thread at the engine level today, but
// the mutex is here so the service can survive future multi-session
// parallelism without surprises.
class PeriodManager {
public:
    PeriodManager(PeriodStore& store, Clock& clock, IdGenerator& ids);

    Result<Period> open_period();
    Result<void> attach_session(const std::string& period_id,
                                const std::string& session_id);
    Result<void> close_period(const std::string& period_id);
    Result<void> append_summary(const std::string& period_id,
                                const std::string& summary);
    Result<void> accumulate_tokens(const std::string& period_id,
                                   std::size_t tokens);

    Result<std::vector<Period>> list_open_periods() const;
    Result<std::optional<Period>> find(const std::string& period_id) const;

    // Test/CLI hooks: number of currently cached periods.
    std::size_t cached_period_count() const;

private:
    void flush_locked();
    std::vector<Period> snapshot_locked() const;

    PeriodStore& store_;
    Clock& clock_;
    IdGenerator& ids_;
    mutable std::mutex mutex_;
    std::map<std::string, Period> periods_;
};

}  // namespace agent
