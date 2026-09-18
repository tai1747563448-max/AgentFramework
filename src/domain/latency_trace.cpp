#include "domain/latency_trace.h"

#include <mutex>

namespace agent {
namespace {

// The global observer is protected by a short mutex so set_global_latency_observer
// can be called once at startup from the main thread while emission points run
// from background threads (notably the cpr WriteCallback). The critical
// section copies the std::function, never invokes it.
std::mutex g_observer_mutex;
LatencyObserver g_observer;

}  // namespace

void set_global_latency_observer(LatencyObserver observer) {
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    g_observer = std::move(observer);
}

const LatencyObserver& global_latency_observer() {
    // Callers (emit_latency_sample) take a copy of the std::function under
    // the mutex and then invoke it outside the lock. The returned reference
    // is only used inside emit_latency_sample itself.
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    return g_observer;
}

}  // namespace agent
