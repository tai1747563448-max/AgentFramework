#pragma once

namespace agent {

// Cancellation port: a symmetric request/poll pair so composition
// roots (e.g. composition::Engine) can ask an arbitrary adapter to
// flip its cancelled bit without downcasting to a concrete subclass.
//
//   is_cancelled() — observed by every long-running port (HTTP transport,
//                    retry loop, runtime state machine) to short-circuit
//                    in-flight work without waiting for a hard timeout.
//   cancel()       — set the bit programmatically; safe to call from
//                    signal handlers or any thread. Implementations must
//                    be noexcept.
class Cancellation {
public:
    virtual ~Cancellation() = default;
    virtual bool is_cancelled() const noexcept = 0;
    virtual void cancel() noexcept = 0;
};

}  // namespace agent
