# MaxTokens Replay Integrity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make replay accept a `max_tokens` budget terminal only when it is the exact causal successor of a legal `MaxTokens` model response, without breaking the three existing generic runtime budget guards.

**Architecture:** Keep `TaskBudgetExceededPayload` as the existing generic terminal event. Add reducer-side causal validation for the `max_tokens` variant using `TaskState::accepted_model_stop_reason`, the accepted assistant response, and the exact fixed payload; reject any budget terminal after an accepted `EndTurn` or `StopSequence`. Preserve the legal `max_task_time_ms`, `max_model_rounds`, and `max_tool_calls` paths.

**Tech Stack:** C++17, MSVC/Visual Studio 2022, existing minimal test runner, CMake/CTest, Git local bare `backup` remote.

**Spec:** `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`

## Global Constraints

- Implement only this replay-integrity repair in the Runtime Kernel.
- Do not add real tools, real RAG, recovery, retry, parallel calls, multi-Agent, a second Provider, HTTP service, TUI, MCP, or plugins.
- Keep Domain independent of cpr, dotenv-cpp, nlohmann/json, CLI, and filesystem I/O.
- Preserve the user's failure-state ruling: specialized context/model/tool failures directly set `TaskStatus::Failed` and `terminal_error`; do not add `pending_failure` or another failure status.
- Preserve event legality preview before persistence, then append, then commit live state.
- Preserve `TaskBudgetExceededPayload` for the existing `max_task_time_ms`, `max_model_rounds`, and `max_tool_calls` guards.
- Use behavior-first TDD: observe a focused RED before changing production code, then observe focused GREEN.
- Do not run a live provider request; `AGENT_ENABLE_LIVE_TESTS` remains OFF and credentials are unavailable.
- Commit the milestone to `feat/runtime-kernel`, push to `backup`, and verify exact local/backup SHA equality.

---

### Task 1: Bind MaxTokens budget replay to its exact causal response

**Files:**
- Modify: `tests/application/state_reducer_test.cpp`
- Modify: `src/application/state_reducer.cpp`
- Modify: `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`

**Interfaces:**
- Consumes: `TaskState::accepted_model_stop_reason`, `ModelCallSucceededPayload`, `TaskBudgetExceededPayload`, `reduce_event`, and `replay_events`.
- Produces: strict reducer/replay acceptance for the existing `TaskBudgetExceededPayload` wire event; no new event kind or task status.

- [ ] **Step 1: Add focused forged/tampered replay tests**

Add behavior tests with hand-authored literal payloads. The production mutation each test catches is removal or weakening of the causal/payload validation.

```cpp
TEST_CASE(max_tokens_budget_terminal_requires_exact_prior_response_and_payload) {
    // Start from a legal trace through ModelCallSucceeded(MaxTokens).
    // The exact payload below must replay successfully.
    const agent::TaskBudgetExceededPayload exact{
        "max_tokens",
        {agent::ErrorCode::BudgetExceeded,
         "model output token budget exceeded", false}};

    // Require failure for each independently forged/tampered form:
    // - max_tokens terminal without a preceding ModelCallSucceeded(MaxTokens)
    // - after EndTurn
    // - after StopSequence
    // - after ToolUse
    // - budget_name changed from max_tokens
    // - error code changed
    // - error message changed
    // - retryable changed to true
    // Each failure must be ErrorCode::InvalidTransition.
}

TEST_CASE(non_max_token_budget_guards_remain_replayable) {
    // Hand-build one legal replay trace for each existing guard:
    // max_task_time_ms, max_model_rounds, max_tool_calls.
    // Require TaskStatus::BudgetExceeded and the exact terminal error.
}
```

Do not compute expected payloads with production helpers. Do not assert on a mock; replay real `RuntimeEvent` sequences through the real reducer.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target state_reducer_tests
& .\build\vs2022\Debug\state_reducer_tests.exe
```

Expected RED: at least the forged/tampered `TaskBudgetExceededPayload` cases replay successfully under the current unconditional reducer branch, causing the new negative assertions to fail. Existing tests must still compile.

- [ ] **Step 3: Implement the smallest reducer validation**

In `src/application/state_reducer.cpp`, validate `TaskBudgetExceededPayload` before mutating terminal state:

```cpp
// Binding contract, expressed as reducer behavior:
// 1. If budget_name == "max_tokens", require:
//    - status == AwaitingModel
//    - model_call_in_flight == false
//    - accepted_model_stop_reason == StopReason::MaxTokens
//    - the latest message is an Assistant response with no ToolUseBlock
//    - error == {BudgetExceeded,
//                "model output token budget exceeded", false}
// 2. If accepted_model_stop_reason == MaxTokens, reject every other budget_name.
// 3. If accepted_model_stop_reason is EndTurn or StopSequence, reject every
//    TaskBudgetExceededPayload; TaskCompleted is the only valid terminal successor.
// 4. Preserve existing legal non-max-token budget events. Do not require all
//    TaskBudgetExceeded events to follow MaxTokens.
```

Return `InvalidTransition` with a fixed non-secret diagnostic on any mismatch. Only after validation succeeds assign `terminal_error` and `TaskStatus::BudgetExceeded`. Do not add fields, statuses, event kinds, or provider-specific dependencies.

- [ ] **Step 4: Update the binding design text**

In `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`, add a concise replay-invariant statement next to stop/budget semantics:

```text
A max_tokens TaskBudgetExceeded event is replayable only immediately after an
accepted MaxTokens ModelCallSucceeded and only with the exact fixed max_tokens
payload. EndTurn and StopSequence accept TaskCompleted, not a budget terminal.
The generic time/model-round/tool-call budget events retain their existing
guard-specific legal states.
```

- [ ] **Step 5: Run focused and full offline verification**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target state_reducer_tests runtime_engine_tests
& .\build\vs2022\Debug\state_reducer_tests.exe
& .\build\vs2022\Debug\runtime_engine_tests.exe
cmake --build build/vs2022 --config Debug --parallel
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: focused reducer and engine tests pass; every offline CTest passes; no live test is registered or run.

- [ ] **Step 6: Self-review, commit, and verify local backup**

Confirm the diff touches only the three planned tracked files, preserves direct specialized failures and generic budget guards, and adds no `pending_failure`.

```powershell
git diff --check
git add tests/application/state_reducer_test.cpp `
        src/application/state_reducer.cpp `
        docs/superpowers/specs/2026-08-17-runtime-kernel-design.md `
        docs/superpowers/plans/2026-08-20-max-tokens-replay-integrity.md
git commit -m "fix: bind max tokens budget replay"
$branch = git branch --show-current
git push backup $branch
$local = git rev-parse HEAD
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ($local -ne $remote) { throw "backup commit mismatch" }
git status --short --branch
```

Expected: local and backup SHA match; tracked worktree is clean.

---

## Plan Self-Review

- Spec coverage: the single remaining load-bearing replay invariant is covered by Task 1; no unrelated Runtime Kernel area is changed.
- Placeholder scan: no TBD/TODO or unspecified implementation/test step remains.
- Type consistency: all named types and fields already exist at `c8f27b7`; the plan adds no interface.
- Regression protection: the plan explicitly preserves and replays all three non-MaxTokens budget guards.
