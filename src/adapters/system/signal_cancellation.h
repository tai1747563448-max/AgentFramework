#pragma once

#include "ports/cancellation.h"

namespace agent {

class SignalCancellation final : public Cancellation {
public:
    SignalCancellation();
    bool is_cancelled() const noexcept override;
    void cancel() noexcept override;

    // Call only when the preceding turn's worker has fully exited.
    void begin_turn() noexcept;
    // Also selects idle CLI behavior: Ctrl+C takes the default exit action.
    void end_turn() noexcept;
};

}  // namespace agent
