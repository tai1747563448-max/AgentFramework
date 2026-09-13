#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string_view>

namespace agent {

// A byte bound alone still permits deeply nested JSON to exhaust recursive
// conversion/dump/destruction. Abort parsing before that nesting is materialized.
inline nlohmann::json parse_bounded_stream_json(std::string_view text) {
    return nlohmann::json::parse(
        text.begin(), text.end(),
        [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 64) throw std::invalid_argument("stream JSON nesting exceeds limit");
            return true;
        });
}

}  // namespace agent
