#include "ports/stop_reason_codec.h"
#include "test_support.h"

#include <string>

using agent::decode_stop_reason;
using agent::encode_stop_reason;
using agent::is_known_stop_reason;
using agent::StopReason;

TEST_CASE(decode_stop_reason_maps_provider_tokens_to_canonical_enum) {
    REQUIRE(decode_stop_reason("end_turn") == StopReason::EndTurn);
    REQUIRE(decode_stop_reason("tool_use") == StopReason::ToolUse);
    REQUIRE(decode_stop_reason("max_tokens") == StopReason::MaxTokens);
    REQUIRE(decode_stop_reason("stop_sequence") == StopReason::StopSequence);
    REQUIRE(decode_stop_reason("unknown_token") == StopReason::Unknown);
    REQUIRE(decode_stop_reason("") == StopReason::Unknown);
}

TEST_CASE(encode_stop_reason_round_trips_through_decoder) {
    REQUIRE(decode_stop_reason(encode_stop_reason(StopReason::EndTurn)) ==
            StopReason::EndTurn);
    REQUIRE(decode_stop_reason(encode_stop_reason(StopReason::ToolUse)) ==
            StopReason::ToolUse);
    REQUIRE(decode_stop_reason(encode_stop_reason(StopReason::MaxTokens)) ==
            StopReason::MaxTokens);
    REQUIRE(decode_stop_reason(
                encode_stop_reason(StopReason::StopSequence)) ==
            StopReason::StopSequence);
    REQUIRE(decode_stop_reason(encode_stop_reason(StopReason::Unknown)) ==
            StopReason::Unknown);
}

TEST_CASE(is_known_stop_reason_returns_true_for_every_canonical_value) {
    REQUIRE(is_known_stop_reason(StopReason::EndTurn));
    REQUIRE(is_known_stop_reason(StopReason::ToolUse));
    REQUIRE(is_known_stop_reason(StopReason::MaxTokens));
    REQUIRE(is_known_stop_reason(StopReason::StopSequence));
    REQUIRE(!is_known_stop_reason(StopReason::Unknown));
}

TEST_CASE(encode_stop_reason_never_returns_empty_string) {
    // Unknown / unknown stop reasons map to the literal "unknown"
    // token so the encoder always returns a printable string. The
    // runtime / diagnostics log these tokens verbatim.
    REQUIRE(!encode_stop_reason(StopReason::Unknown).empty());
    REQUIRE(!encode_stop_reason(StopReason::EndTurn).empty());
}
