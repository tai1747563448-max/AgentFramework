#pragma once

#include "application/period_manager.h"
#include "application/session_summarizer.h"
#include "domain/memory_state.h"
#include "domain/result.h"

#include <string>

namespace agent {

// Stage 7: explicit compression — user invokes a CLI command or
// SDK call to immediately summarise and persist a session, without
// waiting for the periodic trigger. Useful for "end of work day"
// moments or before starting a clearly unrelated task.
struct CompressResult {
    std::string session_id;
    std::string period_id;
    std::string summary;
};

class SessionCompressor {
public:
    SessionCompressor(PeriodManager& periods, SessionSummarizer& summarizer);

    Result<CompressResult> compress(const std::string& session_id,
                                    const std::string& period_id,
                                    const std::string& transcript);

private:
    PeriodManager& periods_;
    SessionSummarizer& summarizer_;
};

}  // namespace agent
