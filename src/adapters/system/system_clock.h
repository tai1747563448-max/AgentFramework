#pragma once

#include "ports/clock.h"

namespace agent {

class SystemClock final : public Clock {
public:
    std::string now_utc() const override;
    std::int64_t monotonic_ms() const override;
};

}  // namespace agent
