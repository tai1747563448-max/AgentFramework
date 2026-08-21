# Recovery, Evaluation, and Real Issue Workflow Implementation Plan

> Apply brainstorming, test-driven-development, systematic-debugging,
> verification-before-completion, requesting-code-review, and
> receiving-code-review throughout this plan.

**Goal:** Add explicit durable task recovery, credential-free log evaluation,
and a real offline Issue-to-tested-fix workflow without expanding beyond one
C++17 Coding Agent with a Python RAG sidecar.

**Architecture:** Refactor the existing Runtime loop into one state-driven
continuation path used by fresh and resumed tasks. Replay retains the exact
in-flight model request and reissues in-flight external calls at least once.
Evaluation is a pure application function over validated events. The final
integration composes real persistence, RAG, file, build, and process adapters
with only the Provider replaced by a deterministic scripted model.

**Base:** `main` at `759f33aa922bea389aea78c63fd8187799e4f845`

**Branch/worktree:** `feat/recovery-evaluation-workflow` in
`.worktrees/recovery-evaluation-workflow`

## Global constraints

- Keep exactly the existing task status vocabulary and direct `Failed` path.
- Single foreground task, sequential calls, operator-triggered resume only.
- No shell, Git, network, install, scheduler, concurrency, or multi-agent tool.
- No live Provider or network in ordinary verification.
- Resume derives storage from a valid task ID; it never executes an arbitrary
  event-log path.
- An in-flight replay uses its exact durable request/call and appends no
  duplicate start intent.
- Cumulative model/tool counts persist; monotonic wall time resets per explicit
  process attempt and is documented honestly.
- Every commit is pushed only to `E:\GitBackups\AgentFramework.git`, with
  local/tracking/bare/`ls-remote` SHA equality verified.

## Task 1: Lock the design

**Files**

- Add `docs/superpowers/specs/2026-08-21-recovery-evaluation-workflow-design.md`
- Add `docs/superpowers/plans/2026-08-21-recovery-evaluation-workflow.md`

**Checks**

- Confirm the recovery matrix covers every legal nonterminal replay state.
- Confirm exact external-call semantics and the wall-time limitation.
- Confirm evaluation does not claim semantic code correctness.
- Confirm the real workflow uses production adapters but no live Provider.

**Commit**

```text
docs: design recovery and evaluation workflow
```

## Task 2: Bind replayable model requests

**Files**

- Modify `src/domain/task_state.h`
- Modify `src/application/state_reducer.cpp`
- Modify `tests/application/state_reducer_test.cpp`
- Modify `tests/adapters/jsonl_event_store_test.cpp`

### RED

Add focused reducer and wire-log tests proving replay currently accepts a
model-start request with mismatched messages or timeout, and does not retain the
exact request needed for recovery. Also reject system-prompt changes after the
first durable model round.

Run the reducer and JSONL targets. Record the named expected failures.

### GREEN

Add reducer-derived `last_model_request`, exact message/evidence/timeout
binding, and stable per-task system-prompt binding. Preserve existing event
schema version 1. Re-run focused tests and `git diff --check`.

**Commit**

```text
fix: bind durable model requests for recovery
```

## Task 3: Implement one state-driven continuation engine

**Files**

- Modify `src/application/runtime_engine.h`
- Modify `src/application/runtime_engine.cpp`
- Modify `tests/application/runtime_engine_test.cpp`

### RED

Write recovery tests first for:

- terminal idempotence with zero external calls/appends;
- `Created`, `PreparingContext`, idle `AwaitingModel`, and completed-tool
  prefixes;
- exact in-flight model reissue without another model-start;
- exact active tool reexecution without another tool-start;
- accepted terminal model response completed without another model call;
- cumulative count budgets and per-attempt wall/cancellation guards;
- invalid/empty event input rejected before external calls;
- append failure returns the last durable replay state.

Build and run `runtime_engine_tests`; capture genuine behavior/compile RED.

### GREEN

Add a bounded `ResumeRequest` and `RuntimeEngine::resume`. Refactor fresh run
and resume into one continuation loop keyed by `TaskState`. Preserve every
existing result/error/event contract. Re-run all Engine tests.

**Commit**

```text
feat: resume durable runtime tasks
```

## Task 4: Add safe CLI resume

**Files**

- Modify `src/cli/cli_app.h`
- Modify `src/cli/cli_app.cpp`
- Modify `src/main.cpp`
- Modify `tests/cli/cli_app_test.cpp`
- Modify `tests/integration/runtime_integration_test.cpp`

### RED

Add direct CLI tests for the accepted task ID, invalid/missing/duplicate/extra
arguments, shared bounded result rendering, progress containing only new
durable sequences, missing/invalid log handling, and `--env-file` behavior.
Add composition-level proof that the path is derived from runtime root and task
ID rather than accepted from input.

### GREEN

Add `ResumeCommand`, parse `resume --task-id`, compose it after configuration,
load through `JsonlEventStore::event_path/read_file`, and call Engine resume.
Refactor run/resume result rendering only as needed to avoid drift.

**Commit**

```text
feat: expose explicit task resume
```

## Task 5: Add credential-free log evaluation

**Files**

- Add `src/application/task_evaluator.h`
- Add `src/application/task_evaluator.cpp`
- Add `tests/application/task_evaluator_test.cpp`
- Modify `src/cli/cli_app.h`
- Modify `src/cli/cli_app.cpp`
- Modify `src/main.cpp`
- Modify `tests/cli/cli_app_test.cpp`
- Modify `CMakeLists.txt`

### RED

Add pure evaluation tests for completed, failed, budget, cancelled, and legal
nonterminal logs; exact metric counts; tool-error results; empty evidence; and
overflow-safe counting. Add CLI tests for pass/fail, malformed logs, fixed
output, fixed errors, and rejection of `--env-file`.

### GREEN

Implement `TaskEvaluation` over already decoded events. Dispatch
`evaluate-log` through the same credential-free startup path as `verify-log`.
Return success only for the documented pass verdict and ordinary task-failed
for a valid nonpass verdict.

**Commit**

```text
feat: evaluate durable task logs offline
```

## Task 6: Prove the real crash-and-recover coding workflow

**Files**

- Add `tests/integration/autonomous_issue_workflow_test.cpp`
- Modify `CMakeLists.txt`
- Modify `README.md`

### RED

First add the process/filesystem fixture and scenario assertions against the
pre-recovery API. Capture the missing resume/evaluation workflow failure. Do
not weaken assertions to accommodate platform differences except an existing,
explicit environment capability skip.

### GREEN

Build a temporary real CMake project and trusted RAG index. Compose production
RAG/file/build/process/persistence adapters, inject one append-after-mutation
failure, construct a fresh Runtime, resume, reconcile the safe hash conflict,
run real configure/build/CTest, and complete. Assert replay, metrics, evidence,
tool order, sentinels, source contents, and real test output.

Document commands and the precise scripted-model/live-provider boundary.

**Commit**

```text
test: prove recoverable autonomous issue workflow
```

## Task 7: Review, verify, back up, and merge

**Checks**

- independent diff/spec review with Critical/Important/Minor findings;
- focused reducer, JSONL, Engine, evaluator, CLI, and workflow executables;
- fresh disconnected MSVC Debug configure/build and full offline CTest;
- Windows Python RAG suite and WSL Python/symlink suite;
- WSL g++17 warning-as-error builds for changed portable C++ targets;
- credential-free `run`, `verify-log`, and `evaluate-log` process checks;
- `ctest -N`, live-test-off, credential/static boundary scans;
- `git diff --check`, tracked scope, clean worktree;
- local/upstream/remote-tracking/bare/`ls-remote` SHA equality.

Address accepted review findings with a genuine focused RED before each fix.
Merge locally with `--no-ff`, verify merged `main`, and push only to the E-drive
bare backup.

## Task 8: Write and verify the Chinese source manual

**Files**

- Add `docs/AGENT_FRAMEWORK_GUIDE.zh-CN.md`
- Modify `README.md` to link the guide

Use the real workflow as the narrative spine. Explain every production source
area and its responsibility, then trace exact events, state transitions, RAG
evidence, tool calls, crash boundary, resume behavior, build/test evidence, CLI
outputs, configuration, extension seams, security boundaries, and known V1
limits. Include a complete file map and reproducible offline commands.

Independently review the guide against the final merged source, render/check
Markdown links and commands, run final full verification, commit, push backup,
and only then mark the user goal complete.
