#pragma once

#include "domain/memory_event.h"
#include <vector>

namespace agent {
class MemoryStore {
  public:
    virtual ~MemoryStore() = default;
    virtual Result<void> append(const MemoryEvent &event) = 0;
    virtual Result<std::vector<MemoryEvent>> read_all() const = 0;
    virtual Result<MemoryState> read_state() const = 0;
};
} // namespace agent
