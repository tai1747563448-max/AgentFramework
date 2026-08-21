# Recovery and evaluation workflow implementation report

Date: 2026-08-21

## Scope and boundary

This milestone adds explicit single-Agent crash recovery and offline durable-log
evaluation. It does not add automatic retry policy, arbitrary shell execution,
Git mutation, networking beyond the existing opt-in Provider, parallel tasks,
or multi-Agent coordination. The Runtime remains C++17 and the knowledge
sidecar remains Python-only.

## Delivered slices

1. `ModelCallStarted` is bound to the exact durable messages, evidence, timeout,
   and stable system prompt. `TaskState` retains the last durable model request.
2. `RuntimeEngine::resume` strictly replays the supplied prefix and continues
   through the same control loop as a fresh task. It handles every durable
   phase, reissues exact in-flight calls, preserves cumulative counts, and is
   idempotent for terminal states.
3. `agent resume --task-id <id>` loads only the task-ID-derived JSONL path and
   rejects a directory/log task-ID mismatch before continuing.
4. `agent evaluate-log --events <path>` strictly replays without credentials or
   Provider/RAG construction and returns a fixed-field verdict and metrics.
5. A production-component offline integration test proves an autonomous coding
   workflow, post-effect persistence interruption, process restart, at-least-once
   tool reconciliation, successful CMake/CTest verification, exact replay, and
   offline evaluation.

## TDD evidence

- Request-binding tests first failed because forged messages and evidence were
  accepted; the reducer and JSONL replay now reject them.
- Resume tests first failed to compile because the API did not exist, then
  exercised every recovered phase before the shared continuation loop passed.
- The CLI resume process test first returned “command unavailable”; a second RED
  proved a requested task ID could be paired with a different log ID before the
  composition-root binding was added.
- Evaluator tests first failed to compile because no evaluator existed. The
  credential-free process test then failed with `AGENT_BASE_URL is required`
  until evaluation dispatch was moved before Provider/config construction.
- The real workflow test first observed the tiny project's genuine failing
  CTest. Its first full run exposed a Unicode-path MSBuild header-dependency
  refresh variance, so the fixture was corrected to edit a directly compiled
  `.cpp` source while keeping the Unicode workspace. The completed workflow has
  since passed twice consecutively.

## Real workflow proof

The test creates a tiny calculator repository and a real SQLite/BM25 knowledge
index. It proves the broken project test fails, then uses the production file
tools, structured build tools, process runner, Python RAG adapter, JSONL store,
reducer, Runtime, replay, and evaluator. Only the model is a deterministic
offline script.

The first process durably records 12 events and changes `calculator.cpp`, then
the event-store seam fails append 13 before `ToolCallSucceeded` can be stored.
A new Runtime replays the active tool call. The repeated versioned replacement
returns a model-visible SHA conflict, after which the model rereads the file,
configures, builds, runs the exact `calculator.correct` CTest, and completes.
The resulting 42-event stream is contiguous and contains exactly one task
start, seven model starts, and six tool starts. Replay equals the live terminal
state. Evaluation passes with seven evidence rounds and one expected tool-error
result. `.git`, runtime sentinel, and an outside sentinel remain unchanged.

## Honest limitations

- Recovery is at-least-once for external calls. Exactly-once effects are not
  claimed; safe tools use version/hash preconditions so duplicates can be
  reconciled.
- Durable model/tool counts measure logical call intents, not physical recovery
  attempts. Repeated manual resumes after pre-outcome crashes are not globally
  bounded by those counters.
- Model/tool count budgets are durable. The monotonic wall timer restarts for a
  new explicit process attempt.
- The real workflow uses a scripted model and makes no Provider/network call.
  It proves orchestration and adapter composition, not live Provider quality.

## Independent review fixes

The first independent recovery review reported no Critical finding and three
Important findings:

1. a recovered in-flight wall-time terminal was rejected by the Reducer;
2. the design overstated durable logical counts as a physical-attempt bound;
3. resume used the general link-following log reader before external calls.

It also found that the resume process fixture unset two obsolete environment
names. Focused RED proved the wall guard returned fatal `InvalidTransition`,
`JsonlEventStore` had no safe task-read API, and a hard-linked valid task log
made the real `agent resume` process return success. The fixes permit an exact
time-budget terminal from recovered in-flight model/tool states, document the
logical-count semantics, add a task-ID-bound no-follow read on Windows and
POSIX, route the composition root through it, and correct the environment
names. Focused MSVC tests pass; WSL warning-as-error tests additionally execute
the directory-link rejection that the current Windows token cannot create.

Final disconnected full verification remains pending after re-review.
