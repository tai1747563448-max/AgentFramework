#pragma once

#include "domain/result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

struct MemoryPolicyConfig {
    std::size_t max_entry_bytes{4096};
    std::vector<std::string> protected_values;
};

class MemoryPolicy {
public:
    explicit MemoryPolicy(MemoryPolicyConfig config);

    Result<void> validate_candidate(std::string_view content) const;

private:
    MemoryPolicyConfig config_;
};

bool memory_opted_out(std::string_view text);

} // namespace agent
