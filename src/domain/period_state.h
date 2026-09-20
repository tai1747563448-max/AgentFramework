#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace agent {

// T2 (stage 2): Period is the cross-session grouping unit. A Period is a
// time-bounded cluster of related sessions — sessions inside one Period
// share summaries and can be summarised together during REM. The Period
// itself is a thin metadata record; the long-form memory still lives in
// MemoryStore.
enum class PeriodState {
    Open,        // accepts new sessions
    Closed,      // no new sessions, summarisation done
};

struct Period {
    std::string period_id;
    std::string started_at_utc;
    std::string ended_at_utc;          // empty when state == Open
    std::vector<std::string> session_ids;
    std::string episodic_summary;      // appended each session close
    PeriodState state{PeriodState::Open};
    std::uint64_t sessions_count{0};
    std::size_t tokens_in_period{0};   // accumulated input tokens
};

inline bool operator==(const Period& a, const Period& b) {
    return std::tie(a.period_id, a.started_at_utc, a.ended_at_utc,
                    a.session_ids, a.episodic_summary, a.state,
                    a.sessions_count, a.tokens_in_period) ==
           std::tie(b.period_id, b.started_at_utc, b.ended_at_utc,
                    b.session_ids, b.episodic_summary, b.state,
                    b.sessions_count, b.tokens_in_period);
}

}  // namespace agent
