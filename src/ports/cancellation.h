#pragma once

namespace agent {

class Cancellation {
public:
    virtual ~Cancellation() = default;
    virtual bool requested() const noexcept = 0;
};

}  // namespace agent
