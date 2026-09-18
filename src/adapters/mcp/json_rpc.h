#pragma once

#include "domain/result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace agent::mcp {

// T14 (v2 §3): JSON-RPC 2.0 envelopes used by the Model Context
// Protocol. We support both notifications (no id, no response
// expected) and requests (id required). Errors follow the standard
// {code, message, data?} shape; the adapter layer maps MCP error
// codes to RuntimeError so the runtime does not need to know about
// JSON-RPC at all.

struct JsonRpcRequest {
    std::optional<std::int64_t> id;
    std::string method;
    nlohmann::json params;
};

struct JsonRpcResponse {
    std::int64_t id{0};
    nlohmann::json result;
};

struct JsonRpcError {
    std::int64_t id{0};
    int code{0};
    std::string message;
    nlohmann::json data;
};

inline bool is_error_response(const JsonRpcResponse& response) {
    return response.result.is_object() &&
           response.result.contains("error");
}

std::string encode_request(const JsonRpcRequest& request);

// Parse a single JSON object from a buffer that may contain a
// Content-Length-framed message (MCP / LSP convention) or a plain
// newline-delimited JSON object. Returns the parsed JSON on
// success; on parse failure returns a JsonRpcError-shaped envelope
// with code -32700 (ParseError) so callers can surface a uniform
// transport error.
nlohmann::json decode_envelope(std::string_view buffer);

}  // namespace agent::mcp
