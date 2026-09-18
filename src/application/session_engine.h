#pragma once

#include "application/runtime_engine.h"
#include "domain/result.h"
#include "domain/session_event.h"
#include "domain/session_state.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class Clock;
class IdGenerator;
class SessionStore;
class ContextCompactor;
class MemoryEngine;

using SessionRunTask = std::function<RuntimeResult(
    const RunRequest&, const RuntimeProgressObserver&)>;
using SessionResumeTask = std::function<RuntimeResult(
    const ResumeRequest&, const RuntimeProgressObserver&)>;
using SessionLoadTask = std::function<
    Result<std::optional<std::vector<RuntimeEvent>>>(const std::string&)>;

struct SessionRuntimeDefaults {
    std::string system_prompt;
    RuntimeBudgets budgets;
};

struct SessionTurnResult {
    std::optional<SessionState> session;
    std::optional<TaskState> task;
    std::optional<RuntimeError> error;
    bool compacted{false};
    std::optional<RuntimeError> warning;
};

struct SessionContextSettings {
    bool enabled{true};
    std::size_t threshold_bytes{65536};
    std::size_t hard_limit_bytes{131072};
    std::size_t retain_turns{6};
    std::size_t max_summary_bytes{8192};
    std::size_t memory_top_k{5};
    std::size_t memory_max_injected_bytes{4096};
    bool memory_enabled{true};
};

class SessionEngine {
public:
    SessionEngine(SessionStore& sessions,
                  Clock& clock,
                  IdGenerator& ids,
                  SessionRunTask run_task,
                  SessionResumeTask resume_task,
                  SessionLoadTask load_task,
                  SessionRuntimeDefaults defaults,
                  ContextCompactor* compactor = nullptr,
                  MemoryEngine* memory = nullptr,
                  SessionContextSettings context = {},
                  std::function<void()> compaction_started = {});

    Result<SessionState> create_session(const std::string& workspace_utf8,
                                        const std::string& model);
    Result<SessionState> load_session(const std::string& session_id) const;
    Result<std::vector<SessionState>> list_sessions() const;
    SessionTurnResult submit_turn(
        const std::string& session_id,
        const std::string& user_text,
        RuntimeProgressObserver observer,
        bool use_memory = true,
        const RuntimePresentationOptions& presentation = {},
        bool dry_run = false);
    SessionTurnResult recover_pending_turn(
        const std::string& session_id,
        RuntimeProgressObserver observer,
        bool use_memory = true,
        const RuntimePresentationOptions& presentation = {});

private:
    Result<SessionState> append_event(SessionState state,
                                      SessionEventPayload payload);
    Result<std::string> turn_system_prompt(const SessionState& state,
                                           const std::string& user_text,
                                           bool use_memory,
                                           RuntimePresentationOptions& presentation) const;
    RunRequest turn_request(const SessionState& state,
                             std::string system_prompt,
                             const RuntimePresentationOptions& presentation,
                             bool dry_run = false) const;
    SessionTurnResult finalize_turn(SessionState state,
                                    RuntimeResult runtime);

    SessionStore& sessions_;
    Clock& clock_;
    IdGenerator& ids_;
    SessionRunTask run_task_;
    SessionResumeTask resume_task_;
    SessionLoadTask load_task_;
    SessionRuntimeDefaults defaults_;
    ContextCompactor* compactor_;
    MemoryEngine* memory_;
    SessionContextSettings context_;
    std::function<void()> compaction_started_;
};

}  // namespace agent
