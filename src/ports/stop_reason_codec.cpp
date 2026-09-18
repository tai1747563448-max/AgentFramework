#include "ports/stop_reason_codec.h"

namespace agent {

StopReason decode_stop_reason(std::string_view raw) {
    if (raw == "end_turn") return StopReason::EndTurn;
    if (raw == "tool_use") return StopReason::ToolUse;
    if (raw == "max_tokens") return StopReason::MaxTokens;
    if (raw == "stop_sequence") return StopReason::StopSequence;
    return StopReason::Unknown;
}

std::string encode_stop_reason(StopReason reason) {
    switch (reason) {
        case StopReason::EndTurn:      return "end_turn";
        case StopReason::ToolUse:      return "tool_use";
        case StopReason::MaxTokens:    return "max_tokens";
        case StopReason::StopSequence: return "stop_sequence";
        case StopReason::Unknown:      return "unknown";
    }
    return "unknown";
}

bool is_known_stop_reason(StopReason reason) {
    return reason != StopReason::Unknown;
}

}  // namespace agent
