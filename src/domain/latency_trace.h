#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace agent {

// Stage identifier constants. Keep these names aligned with the constants in
// benchmarks/cli_latency_benchmark.py so the harness can match records without
// translating them. They are plain string literals so log messages and the
// JSONL sink can quote them verbatim.
inline constexpr const char* kStageProcessStart = "process_start";
inline constexpr const char* kStageMenuReady = "menu_ready";
inline constexpr const char* kStageSubmit = "submit";
inline constexpr const char* kStageRequestSend = "request_send";
inline constexpr const char* kStageFirstTextReceived = "first_text_received";
inline constexpr const char* kStageFirstTextRendered = "first_text_rendered";

// Latency trace sample for the CLI latency-parity benchmark.
// The struct is intentionally POD and JSONL-serialisable so the harness can
// write samples to disk without depending on the rest of the domain types.
struct LatencySample {
    std::string request_id;
    // Stage identifier. The set is fixed by the design spec; new values must
    // be added in benchmarks/cli_latency_benchmark.py as well.
    std::string stage;
    std::int64_t monotonic_us{0};
};

// Observer invoked for every emitted sample. The observer must not throw;
// implementations may drop samples instead of propagating exceptions.
using LatencyObserver = std::function<void(const LatencySample&)>;

// Monotonic-clock helper used at the C++ emission points. The implementation
// mirrors ports/clock.h::monotonic_ms() but exposes microsecond resolution so
// the harness can observe sub-millisecond boundaries.
inline std::int64_t latency_monotonic_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// Process-wide hook used by the harness to collect samples without having to
// thread a new observer through every option struct. The setter returns the
// previously installed observer so callers can compose or restore it. A null
// observer disables emission; the runtime cost on the hot path is a single
// pointer compare.
//
// The hook is intentionally process-global: the four canonical stages live in
// distinct modules (main, runtime engine, cpr transport, terminal presenter)
// that already share the same lifetime via the AgentFramework binary.
void set_global_latency_observer(LatencyObserver observer);
const LatencyObserver& global_latency_observer();

// Convenience wrapper that no-ops when no observer is installed. Emission
// points in the runtime call this; it is the only call they need to know
// about. The function copies the std::function under the global mutex and
// invokes the copy outside the lock so background emitters never block on
// startup wiring.
inline void emit_latency_sample(const std::string& request_id,
                                const std::string& stage) {
    LatencyObserver local;
    {
        const auto& slot = global_latency_observer();
        if (!slot) return;
        local = slot;
    }
    try {
        local(LatencySample{request_id, stage, latency_monotonic_us()});
    } catch (...) {
        // Observers must never throw. Drop the sample silently to keep the
        // hot path from being poisoned by a misbehaving sink.
    }
}

}  // namespace agent
