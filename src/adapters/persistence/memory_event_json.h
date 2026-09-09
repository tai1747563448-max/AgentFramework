#pragma once

#include "domain/memory_event.h"
#include <nlohmann/json_fwd.hpp>

namespace agent {
nlohmann::json memory_event_to_json(const MemoryEvent &event);
Result<MemoryEvent> memory_event_from_json(const nlohmann::json &json);
} // namespace agent
