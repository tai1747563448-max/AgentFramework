#pragma once

#include "domain/result.h"
#include "domain/session_event.h"

#include <nlohmann/json_fwd.hpp>

namespace agent {

nlohmann::json session_event_to_json(const SessionEvent& event);
Result<SessionEvent> session_event_from_json(const nlohmann::json& json);

}  // namespace agent
