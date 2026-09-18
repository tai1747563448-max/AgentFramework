#pragma once

#include "domain/model_types.h"
#include "domain/value.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

struct ToolExecutionContext;

// T11 (v2 §3): PermissionMode controls how tool calls are authorised.
//
//   default            -- workspace tools always allowed, every other
//                         tool requires explicit approval. The REPL drives
//                         the confirm loop; non-interactive callers fall
//                         back to deny.
//   acceptEdits        -- workspace write tools allowed silently; other
//                         tools follow default rules.
//   plan               -- all tools denied. Used during /plan mode.
//   bypassPermissions  -- every tool allowed; no prompts.
//   auto               -- classifier decides per call. T11 ships the
//                         stub (always default rules); the bashClassifier
//                         / yoloClassifier are explicitly out of scope
//                         (see v2 §6 / §8).
enum class PermissionMode {
    Default,
    AcceptEdits,
    Plan,
    BypassPermissions,
    Auto,
};

const char* permission_mode_name(PermissionMode mode);

// Parses the mode from a setting-layer string. Returns std::nullopt for
// unknown values so callers can fall back to a default with a clear log
// line.
std::optional<PermissionMode> parse_permission_mode(std::string_view text);

// PermissionDecision is the verdict returned by checkPermissions. Allow
// skips the prompt; Deny short-circuits the call (a ToolResult with
// is_error=true is synthesised); Ask forces the REPL to drive a confirm
// prompt before the tool is executed.
enum class PermissionDecision {
    Allow,
    Deny,
    Ask,
};

const char* permission_decision_name(PermissionDecision decision);

// Permission is the port RuntimeEngine uses before dispatching a tool.
// Implementations are responsible for layering the user-selected mode
// over per-tool rules and surfacing Ask to a confirm hook when present.
class Permission {
public:
    virtual ~Permission() = default;
    virtual PermissionDecision check(
        const ToolCall& call,
        const ToolExecutionContext& context) const = 0;
};

}  // namespace agent
