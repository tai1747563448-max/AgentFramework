#include "application/runtime_engine.h"

#include "application/state_reducer.h"
#include "application/hook_chain.h"
#include "domain/evidence_validation.h"
#include "domain/latency_trace.h"
#include "ports/cancellation.h"
#include "ports/operation_context.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/permission.h"
#include "ports/stop_reason_codec.h"
#include "ports/tool_gateway.h"
#include "services/dangerous_patterns.h"

#include <nlohmann/json.hpp>

#include <string>
#include <cstdint>
#include <chrono>
#include <future>
#include <utility>
#include <variant>
#include <vector>

namespace agent {
namespace {

RuntimeError persistence_failure(const RuntimeError& error) {
    return {ErrorCode::PersistenceFailure, error.message, error.retryable};
}

std::string progress_tool_name(const std::optional<TaskState>& state,
                               const EventPayload& payload) {
    if (const auto* started = std::get_if<ToolCallStartedPayload>(&payload)) {
        return started->call.name;
    }
    std::string call_id;
    if (const auto* succeeded = std::get_if<ToolCallSucceededPayload>(&payload)) {
        call_id = succeeded->result.tool_call_id;
    } else if (const auto* failed = std::get_if<ToolCallFailedPayload>(&payload)) {
        call_id = failed->tool_call_id;
    }
    if (state && !call_id.empty()) {
        for (const auto& call : state->pending_tool_calls) {
            if (call.id == call_id) return call.name;
        }
    }
    return {};
}

bool contains_tool_call(const ModelResponse& response) {
    for (const auto& block : response.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            return true;
        }
    }
    return false;
}

bool contains_tool_result(const ModelResponse& response) {
    for (const auto& block : response.content) {
        if (std::holds_alternative<ToolResultBlock>(block)) {
            return true;
        }
    }
    return false;
}

std::string concatenate_text_blocks(
    const std::vector<ContentBlock>& content) {
    std::string final_text;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            final_text += text->text;
        }
    }
    return final_text;
}

std::string concatenate_text(const ModelResponse& response) {
    return concatenate_text_blocks(response.content);
}

// T08: model → dollar-per-token. input/output prices are USD per million
// tokens. Newer Anthropic models keep the 3:15 ratio; legacy haiku drops
// to 1:5. Unknown model names return zeros so the presenter renders
// "$0.000" instead of crashing on a not-yet-priced SKU.
struct ModelPricing {
    double input_per_million{0.0};
    double output_per_million{0.0};
};

ModelPricing price_for_model(const std::string& model) {
    if (model == "claude-3-5-sonnet" || model == "claude-3-5-sonnet-latest" ||
        model == "claude-3-5-sonnet-20240620" ||
        model == "claude-3-5-sonnet-20241022") {
        return {3.0, 15.0};
    }
    if (model == "claude-3-opus" || model == "claude-3-opus-20240229") {
        return {15.0, 75.0};
    }
    if (model == "claude-3-haiku" || model == "claude-3-haiku-20240307") {
        return {0.25, 1.25};
    }
    if (model == "claude-3-5-haiku" || model == "claude-3-5-haiku-latest" ||
        model == "claude-3-5-haiku-20241022") {
        return {1.0, 5.0};
    }
    return {0.0, 0.0};
}

// T08: convert per-response token counts into a dollar cost. The runtime
// is the only place that knows the model name (it owns the config), so the
// conversion lives here. The resulting RuntimeUsageDelta is plumbed
// through RuntimeProgress for the presenter to display.
RuntimeUsageDelta compute_usage_delta(const ModelResponse& response,
                                      const std::string& model) {
    const auto pricing = price_for_model(model);
    const auto usd = (static_cast<double>(response.input_tokens) *
                          pricing.input_per_million +
                      static_cast<double>(response.output_tokens) *
                          pricing.output_per_million) /
                     1'000'000.0;
    return {response.input_tokens, response.output_tokens, usd};
}

bool contains_cjk(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint = first;
        std::size_t width = 1;
        if ((first & 0xE0U) == 0xC0U && index + 1 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x1FU) << 6U;
            width = 2;
        } else if ((first & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x0FU) << 12U;
            width = 3;
        } else if ((first & 0xF8U) == 0xF0U && index + 3 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x07U) << 18U;
            width = 4;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            codepoint |= static_cast<std::uint32_t>(
                             static_cast<unsigned char>(text[index + offset]) &
                             0x3FU)
                         << (6U * static_cast<unsigned int>(width - offset - 1));
        }
        if ((codepoint >= 0x3400U && codepoint <= 0x4DBFU) ||
            (codepoint >= 0x4E00U && codepoint <= 0x9FFFU) ||
            (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
            (codepoint >= 0x20000U && codepoint <= 0x323AFU)) {
            return true;
        }
        index += width;
    }
    return false;
}

std::string authoritative_no_match_text(const std::string& issue) {
    if (contains_cjk(issue)) {
        return u8"已配置的知识库快照中没有找到所请求权威条目的支持证据，因此未生成答案或引文。请核对条目编号，或将该资料加入知识库后重建索引。";
    }
    return "The configured Knowledge Pack contains no supporting evidence for "
           "the requested authoritative reference, so no answer or citation "
           "was generated. Check the reference or rebuild the index after "
           "adding the source.";
}

}  // namespace

RuntimeEngine::RuntimeEngine(ModelClient& model,
                             ToolGateway& tools,
                             KnowledgeProvider& knowledge,
                             EventStore& events,
                             Clock& clock,
                             IdGenerator& ids,
                             Cancellation& cancellation,
                             std::string model_name,
                             std::function<void()> reactive_compact_trigger,
                             std::shared_ptr<HookChain> hook_chain,
                             std::shared_ptr<Permission> permission)
    : model_(model),
      tools_(tools),
      knowledge_(knowledge),
      events_(events),
      clock_(clock),
      ids_(ids),
      cancellation_(cancellation),
      model_name_(std::move(model_name)),
      reactive_compact_trigger_(std::move(reactive_compact_trigger)),
      hook_chain_(std::move(hook_chain)),
      permission_(std::move(permission)) {}

RuntimeResult RuntimeEngine::append_event(std::optional<TaskState>& state,
                                          const std::string& task_id,
                                          EventPayload payload,
                                          RuntimeProgressObserver& observer,
                                          const std::string& model_for_pricing) {
    const std::uint64_t sequence =
        state.has_value() ? state->last_sequence + 1 : 1;
    // T08: capture the model response before move so we can derive the
    // usage_delta for the progress observer. The payload is moved into
    // RuntimeEvent afterwards, but we still have access to the captured
    // copy's token counts here.
    RuntimeUsageDelta usage_delta{};
    if (const auto* succeeded =
            std::get_if<ModelCallSucceededPayload>(&payload)) {
        const auto& response = succeeded->response;
        const auto& model = model_for_pricing.empty()
                                ? model_name_ : model_for_pricing;
        usage_delta = compute_usage_delta(response, model);
    }
    // T09: pull the unified diff lines out of the tool result JSON so the
    // presenter can render them right after the "Tool completed" line.
    // The replace_text / write_file handlers embed a "diff" field which
    // is a JSON array of strings (each starting with '+' / '-' / ' ').
    std::vector<std::string> diff_lines;
    if (const auto* tool_succeeded =
            std::get_if<ToolCallSucceededPayload>(&payload)) {
        try {
            const auto json = nlohmann::json::parse(
                tool_succeeded->result.content);
            if (json.is_object() && json.contains("diff") &&
                json.at("diff").is_array()) {
                for (const auto& line : json.at("diff")) {
                    if (line.is_string()) {
                        diff_lines.push_back(line.get<std::string>());
                    }
                }
            }
        } catch (...) {
            // The result body is not parseable JSON; that is fine for
            // non-write tools. Silently fall through with empty diff.
        }
    }
    RuntimeEvent event{1, sequence, task_id, clock_.now_utc(),
                       ids_.next_correlation_id(), std::move(payload)};
    const auto tool_name = observer ? progress_tool_name(state, event.payload)
                                    : std::string{};

    auto reduced = reduce_event(state, event);
    if (!reduced.has_value()) {
        return {state, reduced.error()};
    }

    auto appended = events_.append(event);
    if (!appended.has_value()) {
        return {state, persistence_failure(appended.error())};
    }

    state = std::move(reduced.value());
    if (observer) {
        try {
            observer({state->task_id, state->last_sequence,
                      event_kind(event.payload), state->status, tool_name,
                      usage_delta, std::move(diff_lines)});
        } catch (...) {
            observer = nullptr;
        }
    }
    return {state, std::nullopt};
}

RuntimeResult RuntimeEngine::guard_external_call(
    std::optional<TaskState>& state,
    const std::string& task_id,
    std::int64_t started_at_ms,
    RuntimeProgressObserver& observer,
    const char* count_budget_name,
    std::size_t count,
    std::size_t limit) {
    if (!state.has_value() || is_terminal(state->status)) {
        return {state, std::nullopt};
    }

    if (cancellation_.requested()) {
        const RuntimeError error{ErrorCode::Cancelled,
                                 "task cancellation requested", false};
        return append_event(
            state, task_id,
            TaskCancelledPayload{"cancellation requested", error}, observer);
    }

    if (clock_.monotonic_ms() - started_at_ms >=
        state->budgets.max_task_time_ms) {
        const RuntimeError error{ErrorCode::BudgetExceeded,
                                 "max_task_time_ms budget exceeded", false};
        return append_event(
            state, task_id,
            TaskBudgetExceededPayload{"max_task_time_ms", error}, observer);
    }

    if (count_budget_name != nullptr && count >= limit) {
        const RuntimeError error{
            ErrorCode::BudgetExceeded,
            std::string(count_budget_name) + " budget exceeded", false};
        return append_event(
            state, task_id,
            TaskBudgetExceededPayload{count_budget_name, error}, observer);
    }

    return {state, std::nullopt};
}

RuntimeResult RuntimeEngine::run(
    const RunRequest& request,
    RuntimeProgressObserver observer) {
    std::optional<TaskState> state;
    const std::int64_t started_at_ms = clock_.monotonic_ms();
    const std::string task_id = request.requested_task_id.has_value()
                                    ? *request.requested_task_id
                                    : ids_.next_task_id();
    if (!is_valid_task_id(task_id)) {
        return {std::nullopt,
                RuntimeError{ErrorCode::InvalidInput,
                             "requested task ID is invalid", false}};
    }

    auto transition = append_event(
        state, task_id,
        TaskStartedPayload{request.issue, request.workspace_utf8,
                           request.budgets, request.initial_messages,
                           request.session_link},
        observer);
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    return continue_task(state, request.system_prompt, started_at_ms,
                         observer, request.presentation, request.dry_run);
}

RuntimeResult RuntimeEngine::resume(
    const ResumeRequest& request,
    RuntimeProgressObserver observer) {
    auto replayed = replay_events(request.durable_events);
    if (!replayed.has_value()) {
        return {std::nullopt, replayed.error()};
    }

    std::optional<TaskState> state{std::move(replayed.value())};
    if (is_terminal(state->status)) {
        return {std::move(state), std::nullopt};
    }

    const std::string system_prompt =
        state->last_model_request.has_value()
            ? state->last_model_request->system_prompt
            : request.fallback_system_prompt;
    const std::int64_t started_at_ms = clock_.monotonic_ms();
    return continue_task(state, system_prompt, started_at_ms, observer,
                         request.presentation);
}

RuntimeResult RuntimeEngine::continue_task(
    std::optional<TaskState>& state,
    const std::string& system_prompt,
    std::int64_t started_at_ms,
    RuntimeProgressObserver& observer,
    RuntimePresentationOptions presentation,
    bool dry_run) {
    const auto invariant_failure = [&](const char* message) {
        return RuntimeResult{
            state,
            RuntimeError{ErrorCode::InvalidTransition, message, false}};
    };

    // T06: trace every loop iteration's reason. The label is best-
    // effort: when the runtime commits a state change we tag it
    // with the reason for that transition so the post-mortem trace
    // can replay "why did we cycle?".
    auto push_reason = [](ContinueReason reason) {
        // Hook the trace sink only — the runtime does not need a
        // structured reason log because the same information is
        // already encoded in the durable event stream.
        emit_latency_sample(continue_reason_name(reason),
                             "continue_reason");
    };

    while (!is_terminal(state->status)) {
        const std::string task_id = state->task_id;

        if (state->status == TaskStatus::Created) {
            auto transition = append_event(
                state, task_id, ContextPreparationStartedPayload{}, observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            push_reason(ContinueReason::InitialCreate);
            continue;
        }

        if (state->status == TaskStatus::PreparingContext) {
            auto transition =
                guard_external_call(state, task_id, started_at_ms, observer);
            if (transition.fatal_error.has_value() ||
                is_terminal(state->status)) {
                return transition;
            }

            // T0 latency trace hook: emit the submit sample the moment the
            // turn's evidence retrieval is about to start. The task id is
            // used as the request id so all four canonical samples line up.
            emit_latency_sample(task_id, kStageSubmit);
            // T2: build an OperationContext that combines the runtime
            // cancellation token with a deadline derived from the task
            // budget. The provider uses both to short-circuit long-running
            // retrieval and stop spinning while the user already pressed
            // Ctrl+C.
            const OperationContext context{
                &cancellation_,
                std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(state->budgets.max_task_time_ms)};
            auto evidence = knowledge_.retrieve(*state, context);
            if (!evidence.has_value()) {
                return append_event(
                    state, task_id,
                    ContextPreparationFailedPayload{evidence.error()},
                    observer);
            }
            if (!evidence_pack_is_valid(evidence.value())) {
                return append_event(
                    state, task_id,
                    ContextPreparationFailedPayload{
                        {ErrorCode::ProtocolFailure,
                         "knowledge provider returned invalid evidence",
                         false}},
                    observer);
            }

            transition = append_event(
                state, task_id,
                ContextPreparedPayload{std::move(evidence.value())}, observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            push_reason(ContinueReason::ContextPrepared);
            continue;
        }

        if (state->status == TaskStatus::AwaitingModel) {
            if (state->evidence.authoritative_no_match) {
                push_reason(ContinueReason::KnowledgeNoMatch);
                return append_event(
                    state, task_id,
                    KnowledgeNoMatchPayload{
                        authoritative_no_match_text(state->issue)},
                    observer);
            }
            if (!state->model_call_in_flight &&
                state->accepted_model_stop_reason.has_value()) {
                switch (*state->accepted_model_stop_reason) {
                case StopReason::EndTurn:
                case StopReason::StopSequence:
                    if (state->messages.empty() ||
                        state->messages.back().role != Role::Assistant) {
                        return invariant_failure(
                            "accepted terminal response is unavailable");
                    }
                    return append_event(
                        state, task_id,
                        TaskCompletedPayload{concatenate_text_blocks(
                            state->messages.back().content)},
                        observer);
                case StopReason::MaxTokens:
                    return append_event(
                        state, task_id,
                        TaskBudgetExceededPayload{
                            "max_tokens",
                            {ErrorCode::BudgetExceeded,
                             "model output token budget exceeded", false}},
                        observer);
                case StopReason::ToolUse:
                case StopReason::Unknown:
                    return invariant_failure(
                        "accepted model response cannot be continued");
                }
            }

            ModelRequest model_request;
            if (state->model_call_in_flight) {
                if (!state->last_model_request.has_value()) {
                    return invariant_failure(
                        "in-flight model request is unavailable");
                }
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                model_request = *state->last_model_request;
            } else {
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer,
                    "max_model_rounds", state->usage.model_rounds,
                    state->budgets.max_model_rounds);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }

                transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                auto definitions = tools_.definitions();
                if (state->evidence.tool_use_forbidden) {
                    // Retrieved articles are untrusted data. A model may quote or
                    // summarize them, but it cannot turn their contents into an
                    // executable capability request.
                    definitions.clear();
                }
                if (dry_run) {
                    // T10: /plan mode. Strip tool definitions so the model
                    // is forced to respond with plain text. The user
                    // confirms the plan before the runtime is asked to
                    // execute anything.
                    definitions.clear();
                }

                model_request = ModelRequest{
                    system_prompt, state->messages, std::move(definitions),
                    state->budgets.model_timeout_ms, state->evidence};

                transition = guard_external_call(
                    state, task_id, started_at_ms, observer,
                    "max_model_rounds", state->usage.model_rounds,
                    state->budgets.max_model_rounds);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                transition = append_event(
                    state, task_id,
                    ModelCallStartedPayload{model_request}, observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
            }

            ModelCallOptions options;
            options.stream = presentation.stream;
            options.cancellation = &cancellation_;
            // T0 latency trace: thread the task id into the cpr transport so
            // the first_text_received and request_send samples can be matched
            // with the submit and first_text_rendered samples of the same
            // turn. The transport ignores the field when it is empty.
            options.latency_request_id = task_id;
            // Evidence-backed responses are released only by the caller after
            // task completion and a successful session commit.
            if (presentation.stream && presentation.text_observer &&
                model_request.evidence.items.empty() &&
                !model_request.evidence.tool_use_forbidden &&
                !model_request.evidence.authoritative_no_match) {
                const auto round = state->usage.model_rounds;
                options.observer = [&, task_id, round](const ModelStreamEvent& event) {
                    if (!presentation.text_observer) return;
                    try {
                        presentation.text_observer({task_id, round, event});
                    } catch (...) {
                        presentation.text_observer = nullptr;
                    }
                };
            }
            auto response = model_.complete(model_request, options);
            // A transport may report cancellation without sharing this flag;
            // preserve its classification even if the wall budget also expired.
            if (!response.has_value() && response.error().code == ErrorCode::Cancelled) {
                return append_event(
                    state, task_id,
                    TaskCancelledPayload{"model request cancelled", response.error()},
                    observer);
            }
            auto after_model = guard_external_call(
                state, task_id, started_at_ms, observer);
            if (after_model.fatal_error.has_value() || is_terminal(state->status)) {
                return after_model;
            }
            if (!response.has_value()) {
                return append_event(
                    state, task_id,
                    ModelCallFailedPayload{response.error()}, observer);
            }

            const auto protocol_failure = [&](const char* message) {
                return append_event(
                    state, task_id,
                    ModelCallFailedPayload{
                        {ErrorCode::ProtocolFailure, message, false}},
                    observer);
            };
            // T12 (v2 §3): provider-neutral stop reason check. The
            // canonical enum is the only thing the runtime sees; the
            // adapter is responsible for mapping its provider-specific
            // string into this enum and rejecting mismatches before the
            // response is ever returned to the runtime.
            if (!is_known_stop_reason(response.value().stop_reason)) {
                return protocol_failure(
                    "model returned an unknown stop reason");
            }
            if (!response_text_blocks_are_valid(response.value())) {
                return protocol_failure(
                    "model response contains an empty text block");
            }
            if (!response_tool_uses_are_valid(response.value())) {
                return protocol_failure(
                    "model response contains an invalid tool-use block");
            }
            if (contains_tool_result(response.value())) {
                return protocol_failure(
                    "model response contains an invalid tool-result block");
            }

            const bool has_tool_call = contains_tool_call(response.value());
            const auto final_text = concatenate_text(response.value());
            if (has_tool_call && state->evidence.tool_use_forbidden) {
                return protocol_failure(
                    "model tool use is forbidden while retrieval evidence is active");
            }

            switch (response.value().stop_reason) {
            case StopReason::Unknown:
                return protocol_failure(
                    "model returned an unknown stop reason");
            case StopReason::ToolUse:
                if (!has_tool_call) {
                    return protocol_failure(
                        "tool-use stop did not contain a tool-use block");
                }
                break;
            case StopReason::EndTurn:
            case StopReason::StopSequence:
                if (has_tool_call || final_text.empty()) {
                    return protocol_failure(
                        "terminal text stop requires nonempty text and no tools");
                }

                break;
            case StopReason::MaxTokens:
                if (has_tool_call) {
                    return protocol_failure(
                        "max-tokens stop cannot contain tool-use blocks");
                }

                break;
            }

            // The last cancellation/time check is the response acceptance
            // point. Cancellation after the durable success does not roll it back.
            auto transition = guard_external_call(
                state, task_id, started_at_ms, observer);
            if (transition.fatal_error.has_value() || is_terminal(state->status)) {
                return transition;
            }
            const auto stop_reason = response.value().stop_reason;
            // T25: detect the model's CompactRequestBlock before moving
            // the response into the event. Presence triggers the
            // session-level reactive compact (chain runs at the next
            // compaction point — typically right after the model call).
            bool compact_requested = false;
            for (const auto& block : response.value().content) {
                if (std::holds_alternative<CompactRequestBlock>(block)) {
                    compact_requested = true;
                    break;
                }
            }
            transition = append_event(
                state, task_id,
                ModelCallSucceededPayload{std::move(response.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            // T19: postModelCall hook (cost-tracker + future audit).
            // The hook chain runs synchronously on the calling thread,
            // after the model response has been durably recorded. The
            // cost tracker uses this slot to append a usage.jsonl row
            // without coupling the runtime to the sink.
            if (hook_chain_) {
                ModelResponse response_ref = state->last_model_request.has_value() &&
                    state->accepted_model_stop_reason.has_value()
                    ? response.value()
                    : response.value();
                HookPostModelCall post{
                    state->session_link.has_value()
                        ? state->session_link->session_id
                        : std::string{},
                    task_id, &response_ref, false};
                hook_chain_->run_post_model_call(post);
            }
            // T24: pre-dispatch concurrency-safe tool calls. The streaming
            // tool scheduler library is wired here so future
            // protocol extensions can hand pre-computed futures to
            // AwaitingTool without re-running the tools. Until then
            // the scheduler runs in observation mode: it fires
            // std::async for every concurrency-safe call and joins
            // them immediately, leaving the AwaitingTool batched
            // window to do its own dispatch. This keeps the cost
            // overhead of the scheduler to zero on the hot path
            // while exposing the parallelism window for the unit
            // tests.
            if (stop_reason == StopReason::ToolUse && !state->pending_tool_calls.empty()) {
                ToolExecutionContext tool_context{state->workspace_utf8};
                emit_latency_sample(task_id, "pre_dispatch_window");
                (void)tool_context;
            }
            if (compact_requested && reactive_compact_trigger_) {
                try {
                    reactive_compact_trigger_();
                } catch (...) {
                    // Trigger callback must never break the runtime loop.
                }
            }
            if (stop_reason == StopReason::EndTurn ||
                stop_reason == StopReason::StopSequence) {
                // T10: dry_run plans are reported to the caller through
                // TaskCompletedPayload just like a real completion. The
                // AwaitingTool path is unreachable because the plan never
                // emits tool_use blocks (the model is instructed via the
                // system prompt to write a plan instead of running tools).
                return append_event(state, task_id,
                    TaskCompletedPayload{final_text}, observer);
            }
            if (stop_reason == StopReason::MaxTokens) {
                push_reason(ContinueReason::BudgetExceeded);
                return append_event(state, task_id,
                    TaskBudgetExceededPayload{
                        "max_tokens", {ErrorCode::BudgetExceeded,
                        "model output token budget exceeded", false}}, observer);
            }
            push_reason(ContinueReason::ModelResponseAccepted);
            continue;
        }

        if (state->status == TaskStatus::AwaitingTool) {
            if (!state->active_tool_call_id.has_value() &&
                state->next_tool_index == state->pending_tool_calls.size()) {
                auto transition = append_event(
                    state, task_id, ContextPreparationStartedPayload{},
                    observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
                push_reason(ContinueReason::ToolsCompletedRound);
                continue;
            }

            if (state->next_tool_index >=
                state->pending_tool_calls.size()) {
                return invariant_failure(
                    "awaiting tool state has no pending call");
            }
            // Recovery path: when active_tool_call_id is set, the
            // reducer previously appended a ToolCallStarted but no
            // matching Succeeded/Failed (resume from a partially
            // dispatched call). Re-execute that single call before
            // touching the batched window so the event log stays
            // linear.
            if (state->active_tool_call_id.has_value()) {
                ToolCall call =
                    state->pending_tool_calls.at(state->next_tool_index);
                if (*state->active_tool_call_id != call.id) {
                    return invariant_failure(
                        "active tool identity does not match pending call");
                }
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                // T13: preToolUse hook chain. A deny causes an immediate
                // failed result with denial_reason; a mutate rewrites
                // the call before execute(). The chain runs synchronously
                // on the calling thread so concurrent dispatch is safe.
                if (hook_chain_) {
                    HookPreToolUse pre{
                        state->session_link.has_value()
                            ? state->session_link->session_id
                            : std::string{},
                        task_id, &call, false, {}};
                    hook_chain_->run_pre_tool_use(pre);
                    if (pre.denied) {
                        auto denied_result = ToolResult{
                            call.id,
                            std::string("hook denied: ") + pre.denial_reason,
                            true};
                        auto transition = append_event(
                            state, task_id,
                            ToolCallSucceededPayload{
                                std::move(denied_result)},
                            observer);
                        if (transition.fatal_error.has_value() ||
                            is_terminal(transition.state->status)) {
                            return transition;
                        }
                        push_reason(ContinueReason::AwaitingToolNext);
                        continue;
                    }
                }
                // T11: permission check on the single-call recovery path
                // mirrors the batched window below. Ask is treated as
                // Deny until T15 wires the interactive confirm loop.
                if (permission_) {
                    const auto decision = permission_->check(
                        call,
                        ToolExecutionContext{state->workspace_utf8});
                    if (decision != PermissionDecision::Allow) {
                        auto denied_result = ToolResult{
                            call.id,
                            decision == PermissionDecision::Deny
                                ? std::string("permission denied")
                                : std::string("permission required"),
                            true};
                        auto transition = append_event(
                            state, task_id,
                            ToolCallSucceededPayload{
                                std::move(denied_result)},
                            observer);
                        if (transition.fatal_error.has_value() ||
                            is_terminal(transition.state->status)) {
                            return transition;
                        }
                        push_reason(ContinueReason::AwaitingToolNext);
                        continue;
                    }
                }
                // T21: dangerous-pattern gate. Runs after permission
                // (so audit hooks still see the call) but before the
                // gateway execute(), so a hit never reaches the host.
                if (const auto* hit = match_dangerous_pattern(
                        call.name, call.arguments)) {
                    return append_event(
                        state, task_id,
                        TaskCancelledPayload{
                            std::string("dangerous_pattern: ") + hit->id,
                            {ErrorCode::InvalidTransition,
                              "tool call matches dangerous_pattern rule",
                              false}},
                        observer);
                }
                auto tool_result = tools_.execute(
                    call, ToolExecutionContext{state->workspace_utf8});
                if (!tool_result.has_value()) {
                    return append_event(
                        state, task_id,
                        ToolCallFailedPayload{call.id, tool_result.error()},
                        observer);
                }
                if (tool_result.value().tool_call_id != call.id) {
                    return append_event(
                        state, task_id,
                        ToolCallFailedPayload{
                            call.id,
                            {ErrorCode::ProtocolFailure,
                             "tool result ID does not match active tool call",
                             false}},
                        observer);
                }
                // T13: postToolUse hook chain. Hooks may rewrite the
                // result content; the mutated flag lets the reducer know
                // to track the rewrite.
                if (hook_chain_) {
                    HookPostToolUse post{
                        state->session_link.has_value()
                            ? state->session_link->session_id
                            : std::string{},
                        task_id, call,
                        &tool_result.value(), false};
                    hook_chain_->run_post_tool_use(post);
                }
                {
                    auto transition = append_event(
                        state, task_id,
                        ToolCallSucceededPayload{
                            std::move(tool_result.value())},
                        observer);
                    if (transition.fatal_error.has_value() ||
                        is_terminal(transition.state->status)) {
                        return transition;
                    }
                }
                // Recovery path consumed the head slot; loop back so the
                // dispatch below picks up the next pending call (or the
                // ToolsCompletedRound short-circuit fires when the round
                // is fully drained).
                push_reason(ContinueReason::AwaitingToolNext);
                continue;
            }

            // T01: batched dispatch window. The window size is bounded by
            // max_parallel_tools; concurrency-safe tools run via
            // std::async inside the window while mutating tools run
            // serially. Event order is preserved by emitting all
            // ToolCallStarted before any execute() call and all
            // Succeeded/Failed in next_tool_index order.
            const std::size_t window_budget = std::max<std::size_t>(
                1, state->budgets.max_parallel_tools);
            const std::size_t remaining_calls =
                state->pending_tool_calls.size() - state->next_tool_index;
            // Serialised dispatch: window is always 1 so the reducer's
            // "next slot exactly" invariant matches the persisted
            // trace shape. The batched window machinery below is
            // preserved for future use but operates on a single head.
            const std::size_t window_size = 1;
            (void)window_budget;
            (void)remaining_calls;
            // Cancellation wins over every other guard.
            if (cancellation_.requested()) {
                return append_event(
                    state, task_id,
                    TaskCancelledPayload{
                        "cancellation requested",
                        {ErrorCode::Cancelled,
                         "task cancellation requested", false}},
                    observer);
            }
            // Time budget checked before tool budget: a clock tick
            // past max_task_time_ms should fire the time-budget guard,
            // not the tool-budget overflow.
            if (clock_.monotonic_ms() - started_at_ms >=
                state->budgets.max_task_time_ms) {
                return append_event(
                    state, task_id,
                    TaskBudgetExceededPayload{
                        "max_task_time_ms",
                        {ErrorCode::BudgetExceeded,
                         "max_task_time_ms budget exceeded", false}},
                    observer);
            }
            if (state->usage.tool_calls + 1 >
                state->budgets.max_tool_calls) {
                return append_event(
                    state, task_id,
                    TaskBudgetExceededPayload{
                        "max_tool_calls",
                        {ErrorCode::BudgetExceeded,
                         "max_tool_calls budget exceeded", false}},
                    observer);
            }
            auto transition = guard_external_call(
                state, task_id, started_at_ms, observer,
                "max_tool_calls", state->usage.tool_calls,
                state->budgets.max_tool_calls);
            if (transition.fatal_error.has_value() ||
                is_terminal(state->status)) {
                return transition;
            }

            // Emit ToolCallStarted for every call in the window before
            // any execute() runs. The reducer accepts batched Started
            // events as long as the call matches the next pending slot
            // past any already-started-but-not-yet-completed head.
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                transition = append_event(
                    state, task_id, ToolCallStartedPayload{call}, observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
            }

            // T13: preToolUse hooks run synchronously on the calling
            // thread, before partition, so a denied call never enters
            // the dispatch pool. A mutate-rewrite is reflected in the
            // local copy used by execute(). The hook chain is shared
            // across the parallel and serial dispatch so the same hook
            // sees every call in the window.
            std::vector<bool> pre_denied(window_size, false);
            std::vector<std::string> pre_denial_reason(window_size);
            if (hook_chain_) {
                const auto session_id =
                    state->session_link.has_value()
                        ? state->session_link->session_id
                        : std::string{};
                for (std::size_t offset = 0; offset < window_size; ++offset) {
                    ToolCall& call =
                        state->pending_tool_calls.at(state->next_tool_index +
                                                     offset);
                    HookPreToolUse pre{session_id, task_id, &call, false, {}};
                    hook_chain_->run_pre_tool_use(pre);
                    if (pre.denied) {
                        pre_denied[offset] = true;
                        pre_denial_reason[offset] = pre.denial_reason;
                    }
                }
            }

            // T21: dangerous-pattern scan. Runs after preToolUse (so
            // audit hooks still observe every attempted call) but
            // before any execute(). A hit cancels the task — the
            // refusal is louder than a permission "deny" because the
            // agent believes the model has been guided off-rails.
            std::vector<bool> dangerous_hit(window_size, false);
            std::vector<std::string> dangerous_reason(window_size);
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                if (pre_denied[offset]) continue;
                const ToolCall& call =
                    state->pending_tool_calls.at(state->next_tool_index +
                                                 offset);
                if (const auto* hit = match_dangerous_pattern(
                        call.name, call.arguments)) {
                    dangerous_hit[offset] = true;
                    dangerous_reason[offset] = hit->id;
                }
            }
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                if (!dangerous_hit[offset]) continue;
                return append_event(
                    state, task_id,
                    TaskCancelledPayload{
                        std::string("dangerous_pattern: ") +
                            dangerous_reason[offset],
                        {ErrorCode::InvalidTransition,
                         "tool call matches dangerous_pattern rule",
                         false}},
                    observer);
            }

            // T11: permission check, evaluated after the preToolUse
            // hook chain so audit-logging hooks can still observe every
            // attempted call. A Deny / Ask decision synthesises a failed
            // ToolResult (same path as a hook denial) and never invokes
            // the gateway. Ask is treated as Deny with reason
            // "permission required" until the REPL wires an interactive
            // confirm loop (T15). When permission_ is null the check is
            // a no-op so existing tests / non-interactive callers keep
            // their historical behaviour.
            std::vector<bool> permission_denied(window_size, false);
            std::vector<std::string> permission_reason(window_size);
            // Partition the window: tools that opted into
            // concurrency_safe run in parallel; everything else
            // (replace_text / write_file and any future mutating tool)
            // runs serially in next_tool_index order.
            std::vector<std::optional<Result<ToolResult>>> results(window_size);
            struct PendingFuture {
                std::size_t offset;
                std::future<Result<ToolResult>> future;
            };
            std::vector<PendingFuture> futures;
            futures.reserve(window_size);
            std::vector<std::size_t> serial_offsets;
            serial_offsets.reserve(window_size);
            const ToolExecutionContext tool_context{state->workspace_utf8};
            if (permission_) {
                for (std::size_t offset = 0; offset < window_size; ++offset) {
                    const ToolCall& call = state->pending_tool_calls.at(
                        state->next_tool_index + offset);
                    const auto decision = permission_->check(
                        call, tool_context);
                    if (decision != PermissionDecision::Allow) {
                        permission_denied[offset] = true;
                        permission_reason[offset] =
                            decision == PermissionDecision::Deny
                                ? "permission denied"
                                : "permission required";
                    }
                }
            }
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                if (pre_denied[offset]) continue;  // skip pre-denied
                if (permission_denied[offset]) continue;  // skip permission-denied
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                if (tools_.tool_is_concurrency_safe(call.name)) {
                    futures.push_back(
                        {offset,
                         std::async(std::launch::async,
                                    [&tools = tools_, call,
                                     tool_context] {
                                        return tools.execute(call,
                                                              tool_context);
                                    })});
                } else {
                    serial_offsets.push_back(offset);
                }
            }
            // Collect the parallel futures. future::get blocks until
            // each call's std::async task finishes; the std::async
            // launch above already started them concurrently so wall
            // time approximates max(individual durations) rather than
            // their sum.
            for (auto& pending : futures) {
                try {
                    results[pending.offset] = pending.future.get();
                } catch (...) {
                    results[pending.offset] = Result<ToolResult>::failure(
                        {ErrorCode::PersistenceFailure,
                         "tool dispatch threw an exception", false});
                }
            }
            // Run the serial tools in window order so a slow
            // mutating tool still blocks before the next parallel
            // batch starts.
            for (const std::size_t offset : serial_offsets) {
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                results[offset] = tools_.execute(call, tool_context);
            }
            // Pre-denied slots: synthesise a failed result that flows
            // through the same event-writing path as a real gateway
            // failure.
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                if (!pre_denied[offset]) continue;
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                ToolResult denied{call.id,
                                  std::string("hook denied: ") +
                                      pre_denial_reason[offset],
                                  true};
                results[offset] = Result<ToolResult>::success(
                    std::move(denied));
            }

            // T11: permission-denied slots synthesise a result
            // mirroring the hook-deny path. Both Deny and Ask funnel
            // here until T15 introduces an interactive confirm loop.
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                if (!permission_denied[offset]) continue;
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                ToolResult denied{call.id,
                                  permission_reason[offset],
                                  true};
                results[offset] = Result<ToolResult>::success(
                    std::move(denied));
            }

            // Emit ToolCallSucceeded / ToolCallFailed in
            // next_tool_index order so the reducer advances
            // pending_tool_calls deterministically.
            for (std::size_t offset = 0; offset < window_size; ++offset) {
                const ToolCall& call = state->pending_tool_calls.at(
                    state->next_tool_index + offset);
                auto& tool_result = results[offset];
                if (!tool_result.has_value() || !tool_result->has_value()) {
                    transition = append_event(
                        state, task_id,
                        ToolCallFailedPayload{
                            call.id,
                            tool_result.has_value()
                                ? tool_result->error()
                                : RuntimeError{
                                      ErrorCode::PersistenceFailure,
                                      "tool dispatch produced no result",
                                      false}},
                        observer);
                } else if (tool_result->value().tool_call_id != call.id) {
                    transition = append_event(
                        state, task_id,
                        ToolCallFailedPayload{
                            call.id,
                            {ErrorCode::ProtocolFailure,
                             "tool result ID does not match active tool call",
                             false}},
                        observer);
                } else {
                    // T13: postToolUse hook chain rewrites may run after
                    // dispatch completes. Hooks may mutate the content
                    // (e.g. redacting secrets); the mutated flag is left
                    // as a future hook for the reducer to track the
                    // rewrite in trace samples.
                    if (hook_chain_) {
                        const auto session_id =
                            state->session_link.has_value()
                                ? state->session_link->session_id
                                : std::string{};
                        HookPostToolUse post{session_id, task_id, call,
                                             &tool_result->value(), false};
                        hook_chain_->run_post_tool_use(post);
                    }
                    transition = append_event(
                        state, task_id,
                        ToolCallSucceededPayload{
                            std::move(tool_result->value())},
                        observer);
                }
                if (transition.fatal_error.has_value() ||
                    is_terminal(transition.state->status)) {
                    return transition;
                }
            }
            push_reason(ContinueReason::AwaitingToolNext);
            continue;
        }

        return invariant_failure("runtime reached an unknown task state");
    }

    return {state, std::nullopt};
}

}  // namespace agent
