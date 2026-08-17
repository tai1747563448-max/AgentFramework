#include "adapters/json/value_json.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace agent {
namespace {

Result<Value> invalid_value(std::string message) {
    return Result<Value>::failure(
        {ErrorCode::PersistenceFailure, std::move(message), false});
}

}  // namespace

nlohmann::json value_to_json(const Value& value) {
    return std::visit(
        [](const auto& stored) -> nlohmann::json {
            using Stored = std::decay_t<decltype(stored)>;
            if constexpr (std::is_same_v<Stored, std::nullptr_t> ||
                          std::is_same_v<Stored, bool> ||
                          std::is_same_v<Stored, std::int64_t> ||
                          std::is_same_v<Stored, std::string>) {
                return stored;
            } else if constexpr (std::is_same_v<Stored, double>) {
                if (!std::isfinite(stored)) {
                    throw std::invalid_argument("Value double must be finite");
                }
                return stored;
            } else if constexpr (
                std::is_same_v<Stored, std::shared_ptr<const Value::ArrayNode>>) {
                auto json = nlohmann::json::array();
                for (const auto& item : stored->values) {
                    json.push_back(value_to_json(item));
                }
                return json;
            } else {
                auto json = nlohmann::json::object();
                for (const auto& item : stored->values) {
                    json[item.first] = value_to_json(item.second);
                }
                return json;
            }
        },
        value.storage());
}

Result<Value> value_from_json(const nlohmann::json& json) {
    try {
        if (json.is_null()) {
            return Result<Value>::success(Value{});
        }
        if (json.is_boolean()) {
            return Result<Value>::success(Value(json.get<bool>()));
        }
        if (json.is_number_unsigned()) {
            const auto value = json.get<std::uint64_t>();
            if (value > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max())) {
                return invalid_value("JSON integer is outside Value range");
            }
            return Result<Value>::success(Value(static_cast<std::int64_t>(value)));
        }
        if (json.is_number_integer()) {
            return Result<Value>::success(Value(json.get<std::int64_t>()));
        }
        if (json.is_number_float()) {
            const auto value = json.get<double>();
            if (!std::isfinite(value)) {
                return invalid_value("JSON number must be finite");
            }
            return Result<Value>::success(Value(value));
        }
        if (json.is_string()) {
            return Result<Value>::success(Value(json.get<std::string>()));
        }
        if (json.is_array()) {
            Value::Array values;
            values.reserve(json.size());
            for (const auto& item : json) {
                auto decoded = value_from_json(item);
                if (!decoded.has_value()) {
                    return decoded;
                }
                values.push_back(std::move(decoded.value()));
            }
            return Result<Value>::success(Value::array(std::move(values)));
        }
        if (json.is_object()) {
            Value::Object values;
            for (auto item = json.begin(); item != json.end(); ++item) {
                auto decoded = value_from_json(item.value());
                if (!decoded.has_value()) {
                    return decoded;
                }
                values.emplace(item.key(), std::move(decoded.value()));
            }
            return Result<Value>::success(Value::object(std::move(values)));
        }
    } catch (const nlohmann::json::exception&) {
        return invalid_value("invalid JSON value");
    }
    return invalid_value("unsupported JSON value type");
}

}  // namespace agent
