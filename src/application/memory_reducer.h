#pragma once

#include "domain/memory_event.h"
#include <vector>

namespace agent {
Result<MemoryState> reduce_memory_event(const MemoryState &current,
                                        const MemoryEvent &event);
Result<MemoryState> replay_memory_events(const std::vector<MemoryEvent> &events);
} // namespace agent
