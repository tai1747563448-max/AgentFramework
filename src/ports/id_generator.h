#pragma once

#include <string>

namespace agent {

class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    virtual std::string next_task_id() = 0;
    virtual std::string next_correlation_id() = 0;
};

}  // namespace agent
