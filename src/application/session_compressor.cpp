#include "application/session_compressor.h"

#include "domain/runtime_error.h"

#include <utility>

namespace agent {

SessionCompressor::SessionCompressor(PeriodManager& periods,
                                     SessionSummarizer& summarizer)
    : periods_(periods), summarizer_(summarizer) {}

Result<CompressResult> SessionCompressor::compress(
    const std::string& session_id,
    const std::string& period_id,
    const std::string& transcript) {
    if (session_id.empty() || period_id.empty())
        return Result<CompressResult>::failure(
            {ErrorCode::InvalidInput,
             "session_id and period_id are required", false});

    auto attach = periods_.attach_session(period_id, session_id);
    if (!attach.has_value())
        return Result<CompressResult>::failure(attach.error());

    const auto summary = summarizer_.summarize(transcript);
    auto append = periods_.append_summary(period_id, summary);
    if (!append.has_value())
        return Result<CompressResult>::failure(append.error());

    return Result<CompressResult>::success(
        CompressResult{session_id, period_id, summary});
}

}  // namespace agent
