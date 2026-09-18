#pragma once

// T24 (v2 §3): streaming tool executor. The runtime kicks off
// concurrency-safe tool calls as std::async tasks the moment the
// ModelResponse lands, then hands the futures off to the AwaitingTool
// reducer so the response-acceptance / persistence path runs in
// parallel with the tool execution. Without this scheduler the tool
// work happens serially inside AwaitingTool, which is fine for one
// tool but blows up wall-clock when a response contains N reads.
//
// cc-haha's StreamingToolExecutor lives on the streaming protocol
// (every TextDelta / ToolUse event fires independently); latency's
// ModelResponse is delivered in one block, so the parallelism window
// is between response acceptance and tool result persistence rather
// than between text and tool-use chunks. The scheduler interface
// preserves the cc-haha idiom (start-tools-while-pipeline-runs) while
// admitting the protocol asymmetry.
#include "domain/model_types.h"
#include "ports/tool_gateway.h"

#include <future>
#include <memory>
#include <optional>
#include <vector>

namespace agent {

class ToolExecutionContext;

struct PreDispatchResult {
    // Future<Result<ToolResult>> indexed by the position of the
    // original ToolCall inside `calls`. The runtime future::get's
    // these in order so reducer semantics stay linear.
    std::vector<std::future<Result<ToolResult>>> futures;
    // Marks which slots were actually pre-dispatched (i.e. tools
    // whose isConcurrencySafe() returned true). Slots that did not
    // qualify are re-executed serially inside AwaitingTool.
    std::vector<bool> pre_dispatched;
};

// schedule_starts walks the calls and fires std::async for every
// concurrency-safe tool. Non-safe tools are left untouched; their
// dispatch happens later in AwaitingTool. The returned PreDispatchResult
// owns the futures, so the caller must keep it alive until all the
// get() calls have happened.
PreDispatchResult schedule_pre_dispatch(
    const std::vector<ToolCall>& calls,
    ToolGateway& tools,
    const ToolExecutionContext& context);

// drain_pre_dispatch awaits every pre_dispatched future and writes
// the results into out_results. The vector is sized to calls.size();
// pre-dispatched slots receive their future's result; non-dispatched
// slots stay nullopt so the caller knows to re-execute.
// Note: takes a non-const reference because std::future::get is a
// non-const operation that consumes the future.
void drain_pre_dispatch(PreDispatchResult& scheduled,
                       std::vector<std::optional<Result<ToolResult>>>& out_results);

}  // namespace agent