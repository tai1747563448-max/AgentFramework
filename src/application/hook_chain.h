#pragma once

#include "domain/model_types.h"
#include "domain/task_state.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {

// T13: declarative hook chain. Each Hook wraps a single async-friendly
// observer for one of four lifecycle events:
//
//   preModelCall  — fires before ModelRequest is sent; can edit
//                   system_prompt / messages / drop the call.
//   postModelCall — fires after ModelResponse arrives; can rewrite
//                   content blocks before they reach the reducer.
//   preToolUse    — fires before a ToolCall runs; can deny or rewrite
//                   arguments. Returns the (possibly mutated) call.
//   postToolUse   — fires after a ToolResult lands; can rewrite the
//                   content before it reaches the model.
//
// Hooks are invoked synchronously and must not throw. The runtime
// silently nulls a throwing observer and continues the loop. The chain
// itself is a vector because order matters: permission checks should
// run before audit loggers, which should run before the cost sink.
struct HookContext {
    std::string task_id;
    std::string session_id;
};

struct HookPreModelCall : HookContext {
    ModelRequest* request{nullptr};
    bool cancelled{false};
};

struct HookPostModelCall : HookContext {
    ModelResponse* response{nullptr};
    bool cancelled{false};
};

struct HookPreToolUse : HookContext {
    ToolCall* call{nullptr};
    bool denied{false};
    std::string denial_reason;
};

struct HookPostToolUse : HookContext {
    ToolCall call;
    ToolResult* result{nullptr};
    bool mutated{false};
};

using PreModelCallHook = std::function<void(HookPreModelCall&)>;
using PostModelCallHook = std::function<void(HookPostModelCall&)>;
using PreToolUseHook = std::function<void(HookPreToolUse&)>;
using PostToolUseHook = std::function<void(HookPostToolUse&)>;

class HookChain {
public:
    void add_pre_model_call(PreModelCallHook hook);
    void add_post_model_call(PostModelCallHook hook);
    void add_pre_tool_use(PreToolUseHook hook);
    void add_post_tool_use(PostToolUseHook hook);

    // Run hooks in registration order. Each call is wrapped in try/catch
    // so a misbehaving hook cannot break the runtime. The cancel / deny
    // flags propagate to the caller; "cancelled" / "denied" entries do
    // not short-circuit the chain so every hook sees every event.
    void run_pre_model_call(HookPreModelCall& event) const;
    void run_post_model_call(HookPostModelCall& event) const;
    void run_pre_tool_use(HookPreToolUse& event) const;
    void run_post_tool_use(HookPostToolUse& event) const;

    std::size_t size() const noexcept;

private:
    std::vector<PreModelCallHook> pre_model_call_;
    std::vector<PostModelCallHook> post_model_call_;
    std::vector<PreToolUseHook> pre_tool_use_;
    std::vector<PostToolUseHook> post_tool_use_;
};

// Built-in audit_logger hook. Logs every tool call to stdout in a stable
// format suitable for `tee` capture. Returns the hook so the caller can
// register it on a HookChain. This is the example hook referenced by the
// T13 design — production hooks (T11 Permission, T19 Cost-tracker) will
// look similar but mutate state instead of merely echoing.
PreToolUseHook make_audit_logger_hook();

}  // namespace agent