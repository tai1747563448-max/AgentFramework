#include "adapters/mcp/json_rpc.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <sstream>
#include <string>

namespace agent::mcp {

namespace {

std::int64_t next_request_id() {
    static std::atomic<std::int64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

std::string encode_request(const JsonRpcRequest& request) {
    nlohmann::json envelope = {
        {"jsonrpc", "2.0"},
        {"method", request.method},
        {"params", request.params}};
    if (request.id.has_value()) {
        envelope["id"] = *request.id;
    }
    const auto body = envelope.dump();
    // MCP / LSP style framing: Content-Length header followed by a
    // blank line and the JSON body. This is the dominant transport
    // framing for stdio MCP servers; downstream code that prefers
    // line-delimited JSON can detect the framing and strip the
    // header before parsing.
    std::ostringstream stream;
    stream << "Content-Length: " << body.size() << "\r\n\r\n"
           << body;
    return stream.str();
}

nlohmann::json decode_envelope(std::string_view buffer) {
    // Strip the optional Content-Length header. We accept both
    // framed (with header) and plain (no header) payloads so tests
    // can drive the parser with the cheapest transport.
    const auto header_end = buffer.find("\r\n\r\n");
    std::string_view body = header_end == std::string_view::npos
                                 ? buffer
                                 : buffer.substr(header_end + 4);
    try {
        return nlohmann::json::parse(body);
    } catch (...) {
        return {{"error",
                 {{"code", -32700},
                  {"message", "parse error"},
                  {"data", std::string{body}}}}};
    }
}

}  // namespace agent::mcp
