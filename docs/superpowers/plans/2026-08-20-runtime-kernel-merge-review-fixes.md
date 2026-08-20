# Runtime Kernel Merge-Review Fixes Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task by task. Each task requires genuine RED evidence, focused GREEN evidence, a full offline CTest run, an independent task review, and a local `backup` push/hash check before the next task.

**Goal:** Close the two Important findings from the whole-branch Runtime Kernel review without expanding beyond the approved V1: reject provider-originated `ToolResultBlock` content and make `agent run` report safe durable progress plus the task ID needed to locate its event log.

**Architecture:** Keep the existing Domain/Application/Ports/Adapters direction. Model-response legality remains defense in depth at the Anthropic boundary, RuntimeEngine boundary, and replay Reducer boundary. Progress is an Application-owned safe projection containing only task ID, sequence, `EventKind`, and `TaskStatus`; the CLI receives it through a callback supplied to `RuntimeEngine::run`, so no event payload, error message, model text, evidence, tool data, credential, timestamp, or correlation ID crosses the reporting seam.

**Tech Stack:** C++17, MSVC 2022, CMake/CTest, existing hand-written test harness, nlohmann/json, CPR compiled but not called by offline tests.

---

## Global constraints

- Preserve the user's simple failure-state ruling: `ContextPreparationFailed`, `ModelCallFailed`, and `ToolCallFailed` directly store `terminal_error` and enter `TaskStatus::Failed`; do not add `pending_failure` or another failure status.
- Keep Python RAG/knowledge outside the C++ Runtime behind `KnowledgeProvider`; do not implement real RAG, real coding tools, retries, recovery, concurrency, HTTP service, or multi-agent behavior.
- All ordinary tests remain offline. `AGENT_ENABLE_LIVE_TESTS` stays OFF unless credentials are explicitly available; no live provider request is authorized by this plan.
- Preserve write-ahead ordering: preview reduce, append/flush, commit state, then notify progress. A preview or append failure must not notify an event that is not durable.
- Progress output is a fixed safe projection only: valid task ID, sequence, fixed `EventKind`, fixed `TaskStatus`. Never expose `RuntimeEvent::payload`, `RuntimeError::message`, issue, workspace, evidence, model text, tool arguments/results, provider body, credential, timestamp, or correlation ID.
- Progress observation is best effort and must never control the durable task: `run` owns a mutable per-run callback copy, catches any callback exception at the reporting seam, disables that callback after its first failure, and continues normal Runtime control flow and `RuntimeResult` production.
- Final CLI summaries may include the validated task ID, fixed status, and fixed `ErrorCode` name. They must not print raw terminal or fatal error messages.
- Existing exact MaxTokens causal/payload binding and legal `max_task_time_ms`, `max_model_rounds`, and `max_tool_calls` replay must remain unchanged.
- Tracked scope is limited to files named in each task. Generated build directories, SDD ledgers, reports, and review packages remain ignored.
- Commit to `feat/runtime-kernel`, push each commit to the local `backup` remote, and verify local/tracking/bare SHA equality. Do not use GitHub.

## Pre-flight consistency rulings

1. `ToolResultBlock` remains a valid internal content type for outgoing user tool-result messages and durable conversation history, but it is never legal in a `ModelResponse`. Anthropic response decoding rejects it; Engine and Reducer independently reject it so a Fake or forged JSONL log cannot bypass the boundary.
2. Stop/content matrices become exact:
   - `ToolUse`: one or more `ToolUseBlock`; other blocks may only be `TextBlock`.
   - `EndTurn` / `StopSequence`: one or more nonempty concatenated `TextBlock` bytes; no tool-use or tool-result block.
   - `MaxTokens`: no `ToolUseBlock` or `ToolResultBlock`; it may contain text and still terminates through the existing exact `max_tokens` budget event.
3. A progress callback receives `RuntimeProgress`, not `RuntimeEvent`, so the CLI cannot accidentally print payloads. `RuntimeEngine` invokes it only after the event is durable and the reduced state is committed.
4. The callback is supplied per `run` call rather than stored as global mutable Engine state; this keeps tests deterministic and avoids coupling the core to `std::cout`. The V1 is still single-threaded, but the API does not create hidden observer lifetime state.
5. The actual Windows-console Unicode process path remains a documented Minor evidence gap. This plan does not add a local HTTP server or test-only production mode merely to manufacture a successful live-looking `run`; existing UTF-8 argument/renderer unit evidence remains truthful and no real-provider smoke is claimed.

### Task 1: Reject tool-result blocks in model responses at all three trust boundaries

**Files:**
- Modify: `tests/adapters/anthropic_messages_client_test.cpp`
- Modify: `tests/adapters/jsonl_event_store_test.cpp`
- Modify: `tests/application/runtime_engine_test.cpp`
- Modify: `tests/application/state_reducer_test.cpp`
- Modify: `src/adapters/anthropic/anthropic_messages_client.cpp`
- Modify: `src/application/runtime_engine.cpp`
- Modify: `src/application/state_reducer.cpp`
- Modify: `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`
- Create/track with this task: `docs/superpowers/plans/2026-08-20-runtime-kernel-merge-review-fixes.md`

**Step 1: Write Anthropic response-boundary negative tests**

Add a Fake HTTP response whose `content` contains a provider `tool_result` block with otherwise complete fields. Assert `AnthropicMessagesClient::complete` returns `ProtocolFailure`, does not return a `ModelResponse`, and does not echo block content or the fake credential in the error. Keep or strengthen the existing outgoing user-message test proving an internal user `ToolResultBlock` still serializes to Anthropic `tool_result`.

**Step 2: Write RuntimeEngine stop/content matrix negatives**

Feed Fake Model responses containing `ToolResultBlock` for all relevant stop shapes:

- `TextBlock + ToolResultBlock + EndTurn`;
- `TextBlock + ToolResultBlock + StopSequence`;
- `TextBlock + ToolResultBlock + MaxTokens`;
- `ToolUseBlock + ToolResultBlock + ToolUse`.

For each, assert the durable trace ends with exactly one `ModelCallFailedPayload` carrying a fixed `ProtocolFailure`; it contains no `ModelCallSucceeded`, `TaskCompleted`, `TaskBudgetExceeded`, or `ToolCallStarted`, and no tool or later external call occurs. Do not assert source text.

**Step 3: Write forged replay negatives**

Build real `RuntimeEvent` sequences through `ModelCallStarted`, then append a forged `ModelCallSucceededPayload` for each matrix entry above. Assert `replay_events` fails with `InvalidTransition` at that event. Preserve positive response shapes and the exact MaxTokens terminal tests.

**Step 4: Run RED and record evidence**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target anthropic_adapter_tests runtime_engine_tests state_reducer_tests
& .\build\vs2022\Debug\anthropic_adapter_tests.exe
& .\build\vs2022\Debug\runtime_engine_tests.exe
& .\build\vs2022\Debug\state_reducer_tests.exe
```

The new tests must fail for the expected behavioral reason before production changes: provider/model/replay accepts a `ToolResultBlock`. Compilation-only failure is not sufficient unless a deliberately referenced new API is part of the test.

**Step 5: Implement the minimal three-boundary rejection**

- Anthropic decoder: return fixed `ProtocolFailure` for provider response type `tool_result`; do not parse or retain its content.
- RuntimeEngine: classify any `ToolResultBlock` in `ModelResponse` as a protocol failure before `ModelCallSucceeded`; enforce the exact stop/content matrix above.
- Reducer: make `validate_model_response` reject `ToolResultBlock` for every stop reason and enforce the same exact matrix before appending the assistant message.
- Do not change outgoing user `ToolResultBlock` mapping or durable user tool-result messages.
- Update the design document to state that `ToolResultBlock` is request/user-side only and model responses accept only text/tool-use shapes bound to stop reason.

**Step 6: Run focused GREEN and full offline regression**

Run the three focused executables, then:

```powershell
cmake --build build/vs2022 --config Debug --parallel
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Output must be clean apart from already documented third-party configure warnings. No live smoke.

**Step 7: Self-review, commit, backup**

Confirm only the nine named tracked paths changed, outgoing/durable user tool results remain covered, the old JSONL legal-response fixture no longer places a `ToolResultBlock` in `ModelResponse`, exact MaxTokens replay remains covered, direct failure states remain unchanged, and `git diff --check` passes.

Commit:

```text
fix: reject invalid model response blocks
```

Push to `backup/feat/runtime-kernel`; verify local SHA, tracking ref, and `E:\GitBackups\AgentFramework.git` branch ref match; require tracked worktree clean. Write the required task report with RED/GREEN/full-suite evidence.

### Task 2: Report safe durable runtime progress and task identity

**Files:**
- Modify: `src/application/runtime_engine.h`
- Modify: `src/application/runtime_engine.cpp`
- Modify: `src/cli/cli_app.h`
- Modify: `src/cli/cli_app.cpp`
- Modify: `src/main.cpp`
- Modify: `tests/application/runtime_engine_test.cpp`
- Modify: `tests/cli/cli_app_test.cpp`
- Modify: `tests/integration/runtime_integration_test.cpp`
- Modify: `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`
- Modify: `README.md`
- Modify in fix round: `docs/superpowers/plans/2026-08-20-runtime-kernel-merge-review-fixes.md`

**Step 1: Write RuntimeEngine progress-order tests**

Introduce tests for the new per-run observer contract before implementation:

- a normal completed run reports one `RuntimeProgress` after each successfully appended event, with contiguous sequence, the stable task ID, fixed `EventKind`, and the post-reduce `TaskStatus` in durable order;
- an EventStore append failure reports no progress for the rejected append and never reports a state transition that is absent from the store;
- a throwing observer is called once, then disabled; the task still executes to its intended terminal state, returns its normal structured result, and persists the same complete event sequence as an empty observer;
- observer data has no payload/error/message/text/evidence/tool/provider fields by type, not merely by a redaction assertion.

**Step 2: Write CLI safe-output tests**

Change the injected `RunCommand` contract so a fake run receives the observer and can emit progress. Assert exact output for:

- durable progress lines: `task_id=<validated> sequence=<n> event=<fixed-name> status=<fixed-name>`;
- completed run: progress plus sanitized Unicode final text;
- `Failed`, `BudgetExceeded`, and `Cancelled`: task ID, fixed status, fixed error-code name, and a fixed actionable summary, never `terminal_error.message`;
- persistence/fatal failure with a durable state: validated task ID, last fixed status, fixed fatal error-code name, and a fixed summary;
- invalid task IDs in progress or final state are rejected without reflecting the unsafe bytes;
- sentinel issue, workspace, evidence, model text, tool values, correlation/timestamp, provider body, and fake credential never appear in progress/error summaries.

Do not weaken the existing UTF-8 renderer/control-byte tests or stable exit-code assertions.

**Step 3: Write a real Runtime + CLI integration test**

Update the existing integration composition so `CliApp` supplies its observer through `RunCommand` to the real `RuntimeEngine`. Assert emitted progress sequences correspond exactly to events read back from `JsonlEventStore`, the task ID is the same in output/state/log, final status is Completed, and no sentinel secret/error payload appears. This is offline and uses existing Fakes; it is not claimed as a real Windows console or provider smoke test.

**Step 4: Run RED and record evidence**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_engine_tests cli_tests runtime_integration_tests
```

The expected RED is a missing observer/progress API or failing safe-report behavior. Record the exact command and relevant compiler/test failure.

**Step 5: Implement the minimal safe projection and reporting seam**

- Add `RuntimeProgress { task_id, sequence, EventKind, TaskStatus }` and `RuntimeProgressObserver` to the Application API.
- `RuntimeEngine::run` accepts the observer per call and threads it into event append operations.
- After preview reduce succeeds, EventStore append succeeds, and state is committed, invoke the observer with the safe projection. Never invoke it before durability or with a rejected event.
- `RuntimeEngine::run` owns its callback copy. If notification throws, catch at this narrow observational boundary, clear the callback so it is not retried, and continue; do not turn reporting failure into `fatal_error`, a task event, an exit-code change, or an interrupted model/tool loop.
- Change `RunCommand` so `CliApp` supplies the observer. The observer renders only validated/fixed progress fields; it never receives `RuntimeEvent::payload`.
- Expose fixed mappings for all current `EventKind`, `TaskStatus`, and relevant `ErrorCode` values inside the CLI module; unknown enum values render a fixed `Unknown`, not integers or untrusted text.
- On terminal/fatal return, print a fixed summary that includes a validated durable task ID when state exists, fixed status, and fixed error code. Preserve the final-text renderer for successful model output; never print raw error messages.
- In `main.cpp`, forward the CLI observer to `RuntimeEngine::run`; the credential-free `verify-log` path remains isolated from env/provider/HTTP composition.
- Document the stable safe line format and the conditional/unrun live smoke status in design/README.

**Step 6: Run focused GREEN and full offline regression**

Run the three focused targets/executables, then full Debug build and CTest. Additionally run the built `agent.exe` credential-free:

- `run` must exit 2 with only the existing fixed configuration error before runtime composition;
- `verify-log` must exit 0 and report the fixture task ID/status/sequence without credentials;
- `ctest -N` must show no live provider test registered.

Do not claim the successful Unicode path is a real-console process test; that review Minor remains explicitly documented.

**Step 7: Self-review, commit, backup**

Confirm only the eleven named tracked paths changed across Task 2 and its reviewed fix round, progress is post-durable and payload-free by type, throwing observation cannot interrupt the task, `verify-log` composition is unchanged, no raw error is printed, all task IDs are validated, no `pending_failure` appears in production code, and `git diff --check` passes.

Commit:

```text
feat: report safe runtime progress
```

Push and verify all local/tracking/bare SHAs and a clean tracked worktree. Write the required task report with RED/GREEN/full-suite/process evidence.

## Final controller and review gate

After both task-scoped reviews have no Critical/Important findings:

1. Configure a new MSVC 2022 x64 build directory with `AGENT_ENABLE_LIVE_TESTS=OFF`.
2. Build Debug from scratch and run focused Reducer/Engine/Anthropic/CLI/Integration executables plus full CTest.
3. Run credential-free `run`, credential-free `verify-log`, `ctest -N`, `git diff --check main..HEAD`, production `pending_failure` scan, bounded credential-pattern scan, branch/backup SHA checks, and feature/main tracked-worktree checks.
4. Dispatch a new whole-branch reviewer from the live main merge-base to HEAD. Fix every Critical/Important finding and repeat a scoped re-review; Minor items remain explicit evidence gaps, not hidden claims.
5. Only after the final whole-branch review says ready may the SDD workspaces be removed and the local integration choices be presented. Do not merge, delete the feature branch, or contact GitHub without the user's choice.
