#pragma once

#include "ports/id_generator.h"

#include <mutex>
#include <random>
#include <string>

namespace agent {

class RandomIdGenerator final : public IdGenerator {
public:
    RandomIdGenerator();

    std::string next_task_id() override;
    std::string next_correlation_id() override;

private:
    std::string next_id(const char* prefix);

    std::mt19937_64 generator_;
    std::mutex mutex_;
};

}  // namespace agent
