#pragma once

#include "ports/permission.h"

#include <map>
#include <string>

namespace agent {

// StaticPermission implements the mode + per-tool rules without any IO.
// It is the only adapter T11 ships: T11 deliberately defers the
// classifier ("auto" mode) and the REPL-driven confirm loop to later
// commits - this commit focuses on the data model and the wiring.
class StaticPermission final : public Permission {
public:
    StaticPermission();
    explicit StaticPermission(PermissionMode mode);

    PermissionMode mode() const noexcept { return mode_; }
    void set_mode(PermissionMode mode) noexcept { mode_ = mode; }

    // allow / deny / ask rules keyed by tool name. The map is consulted
    // before mode-based defaults so users can override single tools
    // without changing the overall posture.
    const std::map<std::string, PermissionDecision>& rules() const {
        return rules_;
    }
    void set_rule(std::string tool, PermissionDecision decision) {
        rules_[std::move(tool)] = decision;
    }
    void clear_rules() { rules_.clear(); }

    PermissionDecision check(
        const ToolCall& call,
        const ToolExecutionContext& context) const override;

private:
    PermissionMode mode_;
    std::map<std::string, PermissionDecision> rules_;
};

// Render the mode + rule set as a small JSON string for /permissions
// output. Stable, human-readable, no embedded secrets.
std::string serialise_permission_state(const StaticPermission& permission);

}  // namespace agent
