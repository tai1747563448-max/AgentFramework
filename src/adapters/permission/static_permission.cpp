#include "adapters/permission/static_permission.h"

#include "ports/tool_gateway.h"

#include <sstream>

namespace agent {

const char* permission_mode_name(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::Default:           return "default";
        case PermissionMode::AcceptEdits:       return "acceptEdits";
        case PermissionMode::Plan:              return "plan";
        case PermissionMode::BypassPermissions: return "bypassPermissions";
        case PermissionMode::Auto:              return "auto";
    }
    return "default";
}

std::optional<PermissionMode> parse_permission_mode(std::string_view text) {
    if (text == "default") return PermissionMode::Default;
    if (text == "acceptEdits") return PermissionMode::AcceptEdits;
    if (text == "plan") return PermissionMode::Plan;
    if (text == "bypassPermissions") return PermissionMode::BypassPermissions;
    if (text == "auto") return PermissionMode::Auto;
    return std::nullopt;
}

const char* permission_decision_name(PermissionDecision decision) {
    switch (decision) {
        case PermissionDecision::Allow: return "allow";
        case PermissionDecision::Deny:  return "deny";
        case PermissionDecision::Ask:   return "ask";
    }
    return "ask";
}

StaticPermission::StaticPermission() : mode_(PermissionMode::Default) {}

StaticPermission::StaticPermission(PermissionMode mode) : mode_(mode) {}

namespace {

bool is_workspace_read_tool(const std::string& name) {
    return name == "list_files" || name == "read_file" ||
           name == "search_text";
}

bool is_workspace_write_tool(const std::string& name) {
    return name == "write_file" || name == "replace_text";
}

}  // namespace

PermissionDecision StaticPermission::check(
    const ToolCall& call,
    const ToolExecutionContext& context) const {
    (void)context;
    const auto rule = rules_.find(call.name);
    if (rule != rules_.end()) {
        return rule->second;
    }
    switch (mode_) {
        case PermissionMode::BypassPermissions:
            return PermissionDecision::Allow;
        case PermissionMode::Plan:
            return PermissionDecision::Deny;
        case PermissionMode::AcceptEdits:
            if (is_workspace_write_tool(call.name) ||
                is_workspace_read_tool(call.name)) {
                return PermissionDecision::Allow;
            }
            return PermissionDecision::Ask;
        case PermissionMode::Auto:
            // T11 ships only the static rules for "auto" mode. The
            // bashClassifier / yoloClassifier that cc-haha layers on
            // top of auto are explicitly out of scope per v2 §6 / §8 -
            // they require T22 sandbox boundaries to be safe. Until
            // then, auto == default.
            return call.name == "list_files" ||
                           call.name == "read_file" ||
                           call.name == "search_text"
                       ? PermissionDecision::Allow
                       : PermissionDecision::Ask;
        case PermissionMode::Default:
            return is_workspace_read_tool(call.name)
                       ? PermissionDecision::Allow
                       : PermissionDecision::Ask;
    }
    return PermissionDecision::Ask;
}

std::string serialise_permission_state(const StaticPermission& permission) {
    std::ostringstream stream;
    stream << "{\"mode\":\"" << permission_mode_name(permission.mode())
           << "\",\"rules\":[";
    bool first = true;
    for (const auto& [name, decision] : permission.rules()) {
        if (!first) stream << ',';
        first = false;
        stream << "{\"tool\":\"" << name
               << "\",\"decision\":\"" << permission_decision_name(decision)
               << "\"}";
    }
    stream << "]}";
    return stream.str();
}

}  // namespace agent
