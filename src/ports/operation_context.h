#pragma once

#include "ports/cancellation.h"

#include <chrono>

namespace agent {

// OperationContext carries the lifetime-bounded cancellation and deadline
// state shared by every RAG operation. The plan's T2 step requires the
// retrieval, pack verification, and sidecar subprocess paths to consult this
// context instead of relying on a single ad-hoc timeout per call site.
//
// Contract:
//   - cancellation may be null; the context still bounds work via deadline.
//   - The deadline is expressed in the same monotonic clock the runtime uses
//     (steady_clock). It is set once at the start of an operation; subsequent
//     stages of the same turn re-derive their own deadline from it.
//   - remaining_ms() returns the signed milliseconds until the deadline; a
//     negative value means the deadline has already expired.
//   - cancelled() is the canonical pre-emption check and short-circuits via
//     the optional Cancellation pointer when one is provided.
struct OperationContext {
    const Cancellation* cancellation{nullptr};
    // Default deadline is "infinity": a default-constructed context bounds no
    // work, which is what callers using the legacy 1-argument forwarders
    // expect. Production call sites always set a real deadline via
    // make_operation_context.
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};

    bool cancelled() const noexcept {
        return (cancellation && cancellation->requested()) ||
               expired();
    }
    bool expired() const noexcept {
        return std::chrono::steady_clock::now() >= deadline;
    }
    std::int64_t remaining_ms() const noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   deadline - now)
            .count();
    }
};

// Build a context that expires after ``timeout`` unless cancelled. Useful
// for ports that historically took a bare timeout integer and want to adopt
// the new context without ripping out their existing call sites.
inline OperationContext make_operation_context(const Cancellation* cancellation,
                                               std::chrono::milliseconds timeout) {
    return OperationContext{
        cancellation,
        std::chrono::steady_clock::now() + timeout};
}

}  // namespace agent
