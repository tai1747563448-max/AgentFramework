#pragma once

#include "domain/model_types.h"
#include "domain/result.h"
#include "domain/value.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

// ToolExecutionContext is intentionally narrow: it carries only the workspace
// root the call was issued against. LatencyTraceSink instrumentation is wired
// through process globals (see domain/latency_trace.h) rather than threaded
// through this struct, matching the existing T0 trace design.
struct ToolExecutionContext {
    std::string workspace_utf8;
};

// T04 (v2 §1 / §0.1): five core methods + two opt-in flags.
//
// The interface replaces cc-haha's 30-method Tool.ts contract, which is a 794
// line React/Ink UI integration file rather than a portable design pattern.
// latency deliberately keeps the surface small: tool authors must implement
// name / description / input_schema / execute; concurrency safety and
// read-only are opt-in with fail-closed defaults so a new tool cannot
// accidentally weaken parallelism or permission checks without explicit
// acknowledgement.
//
// renderToolUseMessage / renderToolResultMessage / getActivityDescription /
// checkPermissions live on the port but are not part of the 5-core surface;
// they are intentionally absent in v1 because the terminal presenter already
// drives rendering from the structured ToolResult and ToolCallStart payload,
// and T11 (Permission) will add checkPermissions in a later commit.
class Tool {
public:
    virtual ~Tool() = default;

    // Four required core methods.
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual Value input_schema() const = 0;
    virtual Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) = 0;

    // Two opt-in flags. Both default to false (fail-closed). Tools that
    // promise no shared mutable state with siblings may opt into concurrency
    // safety so RuntimeEngine::AwaitingTool can dispatch them via std::async;
    // tools that do not mutate the workspace, or whose mutations are
    // strictly additive, may opt into read-only so T11's permission check
    // can skip a confirmation prompt.
    virtual bool isConcurrencySafe() const { return false; }
    virtual bool isReadOnly() const { return false; }
};

// Lightweight description struct used to build a Tool via the build_tool()
// factory. Capturing the execute function as std::function lets the gateway
// implementation stay simple while still allowing the factory to enforce
// fail-closed defaults.
struct ToolBlueprint {
    std::string name;
    std::string description;
    Value input_schema;
    std::function<Result<ToolResult>(const ToolCall&,
                                    const ToolExecutionContext&)>
        execute;
    bool concurrency_safe{false};
    bool read_only{false};
};

// build_tool wires a blueprint into a concrete Tool subclass. The factory
// returns nullptr when the blueprint is missing required fields so callers
// can fail at construction time rather than at first dispatch. The returned
// Tool exposes the four required core methods by value and the two opt-in
// flags as constants; the blueprint is consumed once and the resulting
// object owns no shared state with the caller.
std::unique_ptr<Tool> build_tool(ToolBlueprint blueprint);

// ToolGateway is the higher-level dispatch surface RuntimeEngine uses. It
// intentionally stays narrow: model-facing definitions, single-call execute,
// and per-tool metadata so AwaitingTool can group concurrent calls without
// exposing Tool instances to the runtime.
class ToolGateway {
public:
    virtual ~ToolGateway() = default;
    virtual std::vector<ToolDefinition> definitions() const = 0;
    virtual Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) = 0;

    // Fail-closed defaults so a new ToolGateway that forgets to override
    // these cannot accidentally enable parallel execution or skip
    // permission prompts.
    virtual bool tool_is_concurrency_safe(
        const std::string& name) const {
        (void)name;
        return false;
    }
    virtual bool tool_is_read_only(
        const std::string& name) const {
        (void)name;
        return false;
    }
};

}  // namespace agent
