#pragma once

#include "domain/result.h"
#include "domain/value.h"

#include <nlohmann/json_fwd.hpp>

namespace agent {

nlohmann::json value_to_json(const Value& value);
Result<Value> value_from_json(const nlohmann::json& json);

}  // namespace agent
