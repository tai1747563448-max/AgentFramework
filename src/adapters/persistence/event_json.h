#pragma once

#include "domain/result.h"
#include "domain/runtime_event.h"

#include <nlohmann/json_fwd.hpp>

#include <vector>

namespace agent {

nlohmann::json event_to_json(const RuntimeEvent& event);
Result<RuntimeEvent> event_from_json(const nlohmann::json& json);
nlohmann::json message_list_to_json(const std::vector<Message>& messages);
Result<std::vector<Message>> message_list_from_json(
    const nlohmann::json& json);

}  // namespace agent
