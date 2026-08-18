#include "adapters/system/random_id_generator.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace agent {

RandomIdGenerator::RandomIdGenerator() {
    std::random_device random;
    std::array<std::uint32_t, 8> seed_values{};
    for (auto& value : seed_values) {
        value = random();
    }
    std::seed_seq seed(seed_values.begin(), seed_values.end());
    generator_.seed(seed);
}

std::string RandomIdGenerator::next_task_id() {
    return next_id("task-");
}

std::string RandomIdGenerator::next_correlation_id() {
    return next_id("corr-");
}

std::string RandomIdGenerator::next_id(const char* prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uniform_int_distribution<std::uint64_t> distribution;
    std::ostringstream result;
    result << prefix << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << distribution(generator_) << std::setw(16)
           << distribution(generator_);
    return result.str();
}

}  // namespace agent
