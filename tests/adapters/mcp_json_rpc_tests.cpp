#include "adapters/mcp/json_rpc.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace {

using agent::mcp::decode_envelope;
using agent::mcp::encode_request;
using agent::mcp::JsonRpcRequest;

TEST_CASE(encode_request_emits_content_length_framing) {
    JsonRpcRequest request;
    request.id = 7;
    request.method = "tools/list";
    request.params = nlohmann::json::object();
    const auto encoded = encode_request(request);
    REQUIRE(encoded.find("Content-Length: ") == 0);
    REQUIRE(encoded.find("\r\n\r\n") != std::string::npos);
    const auto body = encoded.substr(encoded.find("\r\n\r\n") + 4);
    const auto parsed = nlohmann::json::parse(body);
    REQUIRE(parsed.at("jsonrpc") == "2.0");
    REQUIRE(parsed.at("method") == "tools/list");
    REQUIRE(parsed.at("id") == 7);
    REQUIRE(parsed.at("params").is_object());
}

TEST_CASE(encode_request_omits_id_for_notification) {
    JsonRpcRequest notification;
    notification.id = std::nullopt;
    notification.method = "notifications/initialized";
    notification.params = nlohmann::json::object();
    const auto encoded = encode_request(notification);
    const auto body =
        encoded.substr(encoded.find("\r\n\r\n") + 4);
    const auto parsed = nlohmann::json::parse(body);
    REQUIRE(parsed.contains("method"));
    REQUIRE(!parsed.contains("id"));
}

TEST_CASE(decode_envelope_accepts_framed_payload) {
    const std::string raw =
        "Content-Length: 60\r\n\r\n"
        R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})";
    const auto parsed = decode_envelope(raw);
    REQUIRE(parsed.at("id") == 1);
    REQUIRE(parsed.at("result").at("tools").is_array());
    REQUIRE(parsed.at("result").at("tools").empty());
}

TEST_CASE(decode_envelope_accepts_plain_json) {
    const std::string raw =
        R"({"jsonrpc":"2.0","id":2,"result":{"ok":true}})";
    const auto parsed = decode_envelope(raw);
    REQUIRE(parsed.at("id") == 2);
    REQUIRE(parsed.at("result").at("ok") == true);
}

TEST_CASE(decode_envelope_surfaces_parse_error) {
    const auto parsed =
        decode_envelope(std::string{"not even json"});
    REQUIRE(parsed.contains("error"));
    REQUIRE(parsed.at("error").at("code") == -32700);
}

}  // namespace
