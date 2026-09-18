#include "application/hook_chain.h"

#include <iostream>
#include <mutex>
#include <utility>

namespace agent {
namespace {

// Audit logging is single-threaded on stdout so the lines stay coherent
// when multiple hooks fire concurrently from AwaitingTool's batched
// dispatch. The mutex lives next to the hook (not the chain) because
// each HookChain owns its own registered hooks and may share them
// across tasks; serialising the hook body is simpler than serialising
// the chain.
std::mutex g_audit_mutex;

template <typename HookVec, typename Event>
void run_chain(HookVec& hooks, Event& event) {
    // Misbehaving hooks must never break the runtime. We cannot disable
    // the offender inside the chain because the hook vector is passed
    // by const reference (the public run_* methods are const). Catch
    // and continue so the next iteration runs as normal.
    for (auto& hook : hooks) {
        if (!hook) continue;
        try {
            hook(event);
        } catch (...) {
            // Swallow — see comment above.
        }
    }
}

}  // namespace

void HookChain::add_pre_model_call(PreModelCallHook hook) {
    if (hook) pre_model_call_.push_back(std::move(hook));
}
void HookChain::add_post_model_call(PostModelCallHook hook) {
    if (hook) post_model_call_.push_back(std::move(hook));
}
void HookChain::add_pre_tool_use(PreToolUseHook hook) {
    if (hook) pre_tool_use_.push_back(std::move(hook));
}
void HookChain::add_post_tool_use(PostToolUseHook hook) {
    if (hook) post_tool_use_.push_back(std::move(hook));
}

void HookChain::run_pre_model_call(HookPreModelCall& event) const {
    run_chain(pre_model_call_, event);
}
void HookChain::run_post_model_call(HookPostModelCall& event) const {
    run_chain(post_model_call_, event);
}
void HookChain::run_pre_tool_use(HookPreToolUse& event) const {
    run_chain(pre_tool_use_, event);
}
void HookChain::run_post_tool_use(HookPostToolUse& event) const {
    run_chain(post_tool_use_, event);
}

std::size_t HookChain::size() const noexcept {
    return pre_model_call_.size() + post_model_call_.size() +
           pre_tool_use_.size() + post_tool_use_.size();
}

PreToolUseHook make_audit_logger_hook() {
    return [](HookPreToolUse& event) {
        std::lock_guard<std::mutex> lock(g_audit_mutex);
        std::cerr << "[audit] task=" << event.task_id
                  << " tool=" << event.call->name
                  << " call_id=" << event.call->id << '\n';
    };
}

}  // namespace agent