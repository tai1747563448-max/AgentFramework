#pragma once

#include "ports/cancellation.h"

namespace agent {

class SignalCancellation final : public Cancellation {
public:
    SignalCancellation();
    bool requested() const noexcept override;
};

}  // namespace agent
