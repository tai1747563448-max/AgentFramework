#pragma once

// T19 (v2 §3): cost-tracker hook + sink. Records every model
// response (success or failure) into a JSONL file at
// <runtime_root>/usage.jsonl so /cost can aggregate later.
//
// Hook integration uses the T13 postModelCall lifecycle so the cost
// tracker never has to be hard-wired into the runtime. The sink is
// flushed every 32 records / 500ms to mirror the LatencyTraceSink
// batching cadence and keep the file fsync-friendly.
#include "application/hook_chain.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace agent {

class CostTrackerSink {
public:
    explicit CostTrackerSink(std::filesystem::path usage_path);
    ~CostTrackerSink();

    void record(const std::string& task_id,
                const std::string& model,
                std::size_t input_tokens,
                std::size_t output_tokens,
                double usd,
                bool success);

    // Flush the buffered records to disk. Tests call this after
    // record() so a subsequent read_aggregate() in another sink
    // instance sees the rows. Production callers rely on the
    // destructor to flush.
    void flush();

    // Aggregate read used by /cost. Returns the sum of input, output,
    // and USD across every recorded entry.
    struct Aggregate {
        std::size_t calls{0};
        std::size_t input_tokens{0};
        std::size_t output_tokens{0};
        double usd{0.0};
    };
    Aggregate read_aggregate() const;

    const std::filesystem::path& path() const noexcept { return usage_path_; }

private:
    void flush_locked();

    std::filesystem::path usage_path_;
    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::string buffer_;
    std::size_t pending_{0};
};

// Returns a postModelCall hook that forwards usage to the supplied
// sink. The hook is fail-safe: a thrown exception inside it cannot
// escape into the runtime loop. Returns a std::function so the
// caller can register it on any HookChain.
PostModelCallHook make_cost_tracker_hook(
    CostTrackerSink& sink,
    std::function<std::string()> model_name);

}  // namespace agent