#pragma once

#include <cstdint>
#include <string>

namespace agent {

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::string now_utc() const = 0;
    virtual std::int64_t monotonic_ms() const = 0;
};

}  // namespace agent
