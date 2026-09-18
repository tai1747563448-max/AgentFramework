#include "application/streaming_tool_scheduler.h"

#include <utility>

namespace agent {

PreDispatchResult schedule_pre_dispatch(
    const std::vector<ToolCall>& calls,
    ToolGateway& tools,
    const ToolExecutionContext& context) {
    PreDispatchResult result;
    result.futures.resize(calls.size());
    result.pre_dispatched.assign(calls.size(), false);
    for (std::size_t index = 0; index < calls.size(); ++index) {
        const auto& call = calls[index];
        if (!tools.tool_is_concurrency_safe(call.name)) {
            continue;
        }
        result.pre_dispatched[index] = true;
        result.futures[index] = std::async(
            std::launch::async,
            [&tools, call, context] {
                return tools.execute(call, context);
            });
    }
    return result;
}

void drain_pre_dispatch(PreDispatchResult& scheduled,
                       std::vector<std::optional<Result<ToolResult>>>& out_results) {
    out_results.clear();
    out_results.resize(scheduled.futures.size());
    for (std::size_t index = 0; index < scheduled.futures.size(); ++index) {
        if (!scheduled.pre_dispatched[index]) continue;
        try {
            out_results[index] = scheduled.futures[index].get();
        } catch (...) {
            out_results[index] = Result<ToolResult>::failure(
                {ErrorCode::PersistenceFailure,
                 "pre-dispatched tool threw", false});
        }
    }
}

}  // namespace agent