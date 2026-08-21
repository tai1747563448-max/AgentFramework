# Recovery, Evaluation, and Real Issue Workflow Design

Date: 2026-08-21

Status: approved under the user's standing authorization

## 1. Goal

Complete the first production-shaped version of the local single Coding Agent
by adding three tightly related capabilities:

1. explicitly resume one durable, nonterminal task after a process crash;
2. evaluate a durable event log without credentials or external calls; and
3. prove the composed Runtime, local RAG, safe file tools, structured CMake
   tools, JSONL persistence, and recovery path with one real offline coding
   workflow.

The C++17 Runtime remains the only orchestrator. Python remains a local RAG
sidecar. The existing task states remain unchanged, including direct `Failed`
with a durable error reason.

## 2. Non-goals

This milestone does not add:

- multiple agents, concurrent tasks, a scheduler, a daemon, or background
  retries;
- a shell, Git, network, package-install, or arbitrary-process tool;
- an automatic retry policy for Provider or tool failures;
- a new task status, recovery status, or intermediate failure status;
- exactly-once guarantees for external calls;
- wall-clock accounting that pretends a process monotonic clock survives a
  restart;
- a claim that a scripted offline model proves real Provider intelligence;
- task-specific semantic judging inside the generic log evaluator.

The operator explicitly chooses `resume`. Concurrent `run`/`resume` processes
for the same task are outside V1 and must not be started.

## 3. Chosen recovery semantics

### 3.1 Command and trust boundary

The command is:

```text
agent resume --task-id task-<32 lowercase hex characters>
```

The CLI does not accept an event-log path, workspace, issue, budgets, or prompt
for resume. The composition root validates the task ID and derives exactly:

```text
<AGENT_RUNTIME_ROOT>/tasks/<task-id>/events.jsonl
```

through `JsonlEventStore::event_path`. It reads and strictly replays the entire
log before constructing any new external call. A malformed, forged, missing,
or terminal-inconsistent log cannot be resumed.

### 3.2 At-least-once, append-after-effect recovery

Runtime events describe intent before an external call and outcome after it:

```text
ModelCallStarted -> Provider call -> ModelCallSucceeded/Failed
ToolCallStarted  -> tool effect   -> ToolCallSucceeded/Failed
```

A crash can occur after the external call succeeded but before its outcome was
durably appended. V1 therefore uses explicit at-least-once recovery:

- an in-flight model call is reissued with the exact durable `ModelRequest`;
- an active tool call is reissued with the exact durable `ToolCall`;
- the corresponding `*Started` event is not appended a second time;
- only the newly observed result is appended at the next sequence.

This may duplicate Provider cost. A mutating file call may already have taken
effect. Existing `expected_sha256`, exact-occurrence, and create-only rules make
such reexecution fail safely as a model-visible conflict rather than silently
overwrite newer content. Read/build/test calls can be rerun safely. These are
honest at-least-once semantics, not an exactly-once claim.

### 3.3 Durable request binding

`TaskState` gains a reducer-derived `last_model_request`. It is not a new wire
event or status. On every `ModelCallStarted`, replay requires:

- request messages equal the durable state messages;
- request evidence equal the just-prepared durable evidence;
- request timeout equal the task's durable model timeout;
- after the first model round, the system prompt equals the first durable
  system prompt.

The full request is retained in derived state. If `model_call_in_flight` is
true, resume reissues that exact request. New rounds preserve its system prompt
but obtain current trusted tool definitions. If the task crashed before its
first model call, the configured system prompt is used.

### 3.4 Recovery matrix

| Replayed state | Durable boundary | Resume action |
|---|---|---|
| terminal | completed, failed, budget exceeded, or cancelled | return the existing state; no append and no external call |
| `Created` | `TaskStarted` | append context-start and continue |
| `PreparingContext` | context-start persisted | rerun read-only RAG retrieval, append its result, and continue |
| `AwaitingModel`, idle | context prepared | apply guards, construct and persist the next request, call model |
| `AwaitingModel`, in flight | model-start persisted | apply cancellation/wall guard, reissue exact durable request, append outcome |
| `AwaitingModel`, accepted terminal stop | model-success persisted | append the matching completed or max-token terminal event without another model call |
| `AwaitingTool`, idle pending call | tool work remains | apply guards, persist next tool-start, execute it |
| `AwaitingTool`, active call | tool-start persisted | apply cancellation/wall guard, reexecute exact pending call, append outcome |
| `AwaitingTool`, all calls complete | final tool result persisted | begin the next context round |

The same state-driven continuation loop serves both a fresh `run` and a
`resume`; there are not two drifting Runtime implementations.

### 3.5 Budgets and cancellation

Durable model/tool usage counters continue across restarts. An in-flight call
already consumed its logical-call count when its `*Started` event was reduced,
so a recovery reexecution does not increment the counter again. A new call
still observes the normal count guard.

Cancellation and wall-time guards run before every recovered external call.
The monotonic wall-time budget starts at the beginning of each explicit `run`
or `resume` process attempt. V1 cannot compare monotonic readings across
processes, so it deliberately does not claim a lifetime wall-clock budget.
The durable call-count budgets limit logical call intents, not physical
at-least-once attempts. An operator can repeatedly invoke `resume` after a
process dies before persisting the same call's outcome; V1 has no durable
attempt counter and does not claim that such manual attempts are globally
bounded.

## 4. Credential-free evaluation

The command is:

```text
agent evaluate-log --events <path>
```

Like `verify-log`, it is dispatched before env-file loading, Provider config,
HTTP transport, Python, or tool composition. `--env-file` is rejected.

Evaluation first performs the existing strict JSONL decode and replay. It then
produces deterministic runtime metrics:

- task ID and replayed status;
- pass/fail verdict;
- model rounds and tool calls;
- prepared evidence rounds and total evidence items;
- model requests carrying nonempty evidence;
- model-visible tool error results;
- last durable sequence.

`pass` means the durable Runtime outcome is `Completed` with a nonempty final
text and no in-flight/pending/terminal error state. It does not mean the code
change is semantically correct. A malformed log uses `InvalidEventLog`; a
well-formed nonpassing task uses the ordinary `TaskFailed` process exit.
Task-specific correctness belongs to the real workflow or a future pluggable
judge, not to generic event replay.

## 5. Real offline coding workflow

One integration test constructs a temporary, real CMake project containing a
bug and a failing CTest. It also creates a curated documentation corpus and
builds a real SQLite/BM25 index through the tracked Python CLI.

The production components are composed directly:

```text
scripted offline ModelClient
        |
RuntimeEngine
  |-- PythonRagKnowledgeProvider -> DirectProcessRunner -> Python/SQLite BM25
  |-- CompositeToolGateway
  |     |-- WorkspaceToolGateway
  |     `-- CMakeToolGateway -> DirectProcessRunner -> cmake/ctest
  `-- JsonlEventStore
```

The scripted model performs a realistic sequence:

1. read the buggy source;
2. replace the faulty expression using the returned SHA-256;
3. encounter a simulated persistence crash after the file mutation but before
   `ToolCallSucceeded` is appended;
4. resume from a new Runtime instance;
5. reexecute the durable replace and receive a model-visible hash conflict;
6. reread the file and recognize that the intended change already exists;
7. configure the real CMake project;
8. build the real target;
9. run the real CTest;
10. return a concise verified conclusion.

The test asserts:

- contiguous, strictly replayable durable events;
- no duplicate `TaskStarted`, `ModelCallStarted`, or recovered
  `ToolCallStarted` intent;
- the expected recovery conflict reaches the next model request;
- real RAG evidence reaches every model round with valid citations;
- the source is fixed and the real test passes;
- `.git`, runtime storage, and an external sentinel remain unchanged;
- generic evaluation reports pass;
- the final state is `Completed`, not a recovery-specific state.

This proves the deterministic framework plumbing and recovery contract. It is
explicitly not a live Provider smoke test. Credentialed Provider validation is
run only when credentials and separate authorization are available.

## 6. CLI output and privacy

`run` and `resume` share the existing bounded progress and terminal renderer.
Progress is emitted only after a durable append. Recovery therefore prints only
new sequences. It never prints issue text, workspace paths, evidence, raw tool
results, Provider metadata, or raw error messages.

Evaluation output contains only fixed field names, enum names, booleans, and
counts. Invalid paths/logs produce fixed messages.

## 7. Documentation outcome

After this milestone is independently reviewed and merged, the repository will
receive a Chinese manual built around the same real workflow. It will explain
every production area in call-flow order: configuration and composition,
Domain, reducer/events, Runtime, RAG protocol/index/query, safe workspace tools,
build tools/process isolation, persistence/replay/recovery, CLI/evaluation, and
tests/known limits. File maps and code links will make the manual usable as a
guided source tour rather than a marketing summary.
