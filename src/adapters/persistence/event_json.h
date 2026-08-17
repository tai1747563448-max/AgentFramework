#pragma once

#include "domain/result.h"
#include "domain/runtime_event.h"

#include <nlohmann/json_fwd.hpp>

namespace agent {

nlohmann::json event_to_json(const RuntimeEvent& event);
Result<RuntimeEvent> event_from_json(const nlohmann::json& json);

}  // namespace agent
