#pragma once

#include "domain/memory_state.h"
#include "domain/result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

class MemoryRetriever {
public:
    // The byte budget includes separator newlines.
    Result<std::vector<std::string>> retrieve(const MemoryState& state,
                                              std::string_view workspace_utf8,
                                              std::string_view query,
                                              std::size_t top_k,
                                              std::size_t injected_byte_budget) const;
};

} // namespace agent
