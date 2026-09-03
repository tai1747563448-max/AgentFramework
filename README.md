# AgentFramework Runtime Kernel

完整的中文架构、运行说明、真实崩溃恢复工作流和逐文件代码地图见
[`docs/AGENT_FRAMEWORK_GUIDE.zh-CN.md`](docs/AGENT_FRAMEWORK_GUIDE.zh-CN.md)。

This repository contains the first, deliberately bounded Runtime Kernel for a
C++17 coding agent. It runs one foreground task through an event-sourced state
machine, persists JSONL events, and can verify and replay a task log.

## Architecture and boundaries

The dependency direction points inward:

- `src/domain` owns typed values, model/tool data, task state, errors, and
  runtime events. It does not depend on CPR, dotenv-cpp, nlohmann/json, the
  CLI, or filesystem I/O.
- `src/application` owns the reducer and `RuntimeEngine`. Every candidate event
  is preview-reduced without mutating live state, appended and flushed, and
  only then committed to memory. A per-run observer is notified after that
  durable commit with only task ID, sequence, event kind, and task status. If
  observation throws, the Runtime disables its local callback copy and
  continues the same task; reporting cannot create an event, fatal result, or
  exit-code change.
- `src/ports` defines model, tool, knowledge, event-store, clock, ID, and
  cancellation interfaces.
- `src/adapters` supplies Anthropic Messages HTTP, JSONL persistence, bounded
  workspace file tools, opt-in structured CMake/CTest tools, an empty
  knowledge adapter, an opt-in Python RAG adapter, and system
  clock/ID/cancellation implementations.
- `src/main.cpp` is the composition root; `src/cli` parses `run`, `resume`,
  `verify-log`, and `evaluate-log`, uses fixed failure messages, and bounds the
  fields printed by progress, verification, and evaluation. A successful
  `run` or `resume` renders final text as
  UTF-8, visibly
  escapes terminal controls and invalid bytes, and truncates only at code-point
  boundaries after at most 8192 rendered bytes.

Tool calls are sequential and retain provider response order. A returned
`ToolResult{is_error=true}` is a completed tool call, while infrastructure
failure is a failed gateway call. Cancellation and budgets stop the runtime
before the next external call. JSONL replay validates schema, contiguous
sequence, the exact `task-` plus 32 lowercase hexadecimal ID format, strict
schema-owned nested records, and state transitions; arbitrary `Value` maps
remain open. Replay does not repair a log.

`ModelCallStarted` records the single call in flight. `tool_use` requires a
tool block; `end_turn` and `stop_sequence` require nonempty tool-free text;
`max_tokens` terminates as `BudgetExceeded`; unknown or inconsistent stops are
direct `ModelCallFailed` protocol failures.

This milestone provides versioned text-file inspection/editing, an opt-in
structured build/test loop, and an opt-in deterministic local knowledge
sidecar. It does not provide a model-selectable shell, Git mutation, embeddings,
a vector database, an automatic retry policy, parallel execution, multiple
agents, a second provider, an HTTP service, a TUI, MCP, or plugins.

## Workspace file tools

Every `run` exposes exactly five workspace file tools to the model; the
explicit build-tool opt-in adds the three tools documented below:

- `list_files` returns stable, workspace-relative directory entries.
- `read_file` returns bounded UTF-8 lines and the SHA-256 of the complete
  file.
- `search_text` performs deterministic literal, non-overlapping search;
  case-insensitive mode folds ASCII only.
- `replace_text` requires the current SHA-256 and exact non-overlapping match
  count before changing a file.
- `write_file` either creates a missing file without replacement or overwrites
  an existing file only when its SHA-256 still matches.

Tool paths are relative to the `--workspace` supplied for that durable task.
Absolute paths, `..`, links/reparse points, multiply linked files, `.git`,
`.worktrees`, secret-bearing `.env*` files (except `.env.example`), common
credential files, runtime data, `.agent`, `.rag`, and reserved `.agent-tmp-*`
names are refused or omitted. Writes use an exclusively created same-directory
temporary file, flush it, atomically install it, and verify the installed
bytes. A stale hash or match count is a retryable tool conflict and leaves the
target unchanged.

Text files are strict UTF-8 without NUL and at most 1 MiB. A tool result is at
most 64 KiB; listings and searches return at most 200 results. Recursive
search additionally stops at 2000 entries, 500 files, or 16 MiB scanned and
reports the exact truncation reason. These checks are safety and determinism
limits, not a sandbox against a hostile process concurrently racing the same
workspace.

## Structured build and test tools (opt-in)

Build tools are disabled by default. Set `AGENT_ENABLE_BUILD_TOOLS=1` only for
a trusted local workspace. This adds exactly three closed-schema tools after
the five file tools:

- `configure_project` configures `Debug` or `Release` with CMake.
- `build_project` builds all targets or one strictly validated target.
- `run_tests` runs all CTest tests or one exact, regex-escaped test name.

The adapter always uses `<workspace>/.agent/cmake-build`, fixed `cmake` or
`ctest` programs, and adapter-owned argument vectors. The model cannot select
a program, shell, working directory, environment variable, package command,
Git command, or network command. `AGENT_BUILD_TIMEOUT_SECONDS` defaults to 300
and must be an integer from 1 through 600. Process stdout/stderr and the final
JSON tool result are bounded; a nonzero exit or timeout is returned as
`ToolResult{is_error=true}` so the model can inspect the failure, edit, and
retry without changing the task state machine.

No shell is involved, but this is not an OS sandbox. CMake configuration,
compilers, and project tests execute code from the workspace with the current
user's filesystem permissions. The child environment is reduced and provider
credentials are excluded, yet enabling these tools still authorizes trusted
workspace code execution. Keep them disabled for unknown repositories.

## Local Python RAG sidecar (opt-in)

RAG is disabled by default. In V1 it is a standard-library-only Python 3.10+
SQLite index with deterministic BM25 ranking. The tokenizer handles ASCII
identifiers, camelCase, snake_case, and Chinese characters/bigrams. It does not
download a model, call a network service, install a package, generate an
embedding, or expose a long-running HTTP daemon.

Build or replace an index manually from a trusted source tree. For example,
from the repository root in PowerShell:

```powershell
$python = (Get-Command python).Source
$script = (Resolve-Path .\rag\agent_rag_cli.py).Path
& $python -E -s -X utf8 $script build `
  --source .\docs `
  --index .\.rag\knowledge.sqlite3
$index = (Resolve-Path .\.rag\knowledge.sqlite3).Path
```

The builder accepts a bounded set of text/code extensions, skips protected
directories and conservatively named secret files, rejects links and multiply
linked files, chunks in stable path/line order, and atomically replaces the
SQLite file. The filename filter rejects stem tokens `secret`, `secrets`,
`credential`, `credentials`, `password`, `passwords`, `passwd`, `token`, and
`tokens`, separated by `.`, `-`, or `_`. This is not a content secret scanner:
use a trusted, curated source tree (the example uses `docs`) and review it
before building. Rebuilding removes stale chunks. Its result and failures
contain only bounded summaries; source text and local absolute paths are not
printed as diagnostics.

Enable retrieval only after the index exists. `AGENT_RAG_SCRIPT` and
`AGENT_RAG_INDEX` must name absolute canonical ordinary files without link
components; `AGENT_RAG_PYTHON` may be an absolute interpreter path or a trusted
program name on `PATH`:

```powershell
$env:AGENT_ENABLE_RAG = "1"
$env:AGENT_RAG_PYTHON = $python
$env:AGENT_RAG_SCRIPT = $script
$env:AGENT_RAG_INDEX = $index
$env:AGENT_RAG_TOP_K = "5"
$env:AGENT_RAG_TIMEOUT_SECONDS = "10"
```

For each Runtime context round, C++ sends only the durable Issue through stdin
to one fixed command:

```text
<python> -E -s -X utf8 <script> query --index <index>
```

The response is parsed as an exact versioned JSON object and converted to a
structured `EvidencePack`. At most 20 items and 32 KiB of source content may
become durable. The Python adapter requires each source ID to match its
canonical relative `path#Lx-Ly[-Pn]` citation, requires an exact path/line/hash/
score metadata schema, and recomputes the content SHA-256. Runtime, Reducer,
and JSONL replay enforce the provider-neutral EvidencePack bounds. Reducer also
requires each durable `ModelCallStarted.request.evidence` to exactly equal the
immediately prepared context, so replay cannot substitute what the model saw.

Evidence is labeled as untrusted reference data in the Provider request. It is
not a system instruction and RAG is not a model-callable tool: enabling it does
not change the five file tools or optional eight file/build tools. A timeout,
broken index, invalid JSON, or invalid evidence becomes the existing direct
`ContextPreparationFailed -> Failed` path with a fixed explanation; raw Python
stderr is never persisted or sent to the model.

When `AGENT_ENABLE_RAG=0`, all other `AGENT_RAG_*` values are ignored, no RAG
path is inspected, and no Python process is started. `AGENT_RAG_TOP_K` must be
1 through 20 and `AGENT_RAG_TIMEOUT_SECONDS` must be 1 through 60 when enabled.
Keeping an index inside workspace `.rag` is supported operationally, but the
agent's workspace tools cannot list, read, edit, or replace that directory.

## Build and offline tests on Visual Studio 2022

From a Visual Studio-capable PowerShell in the repository root:

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --parallel
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

CMake configuration may obtain pinned third-party dependencies through
`FetchContent` when they are not already cached. If Python 3.10+ is found,
CTest also registers the Python unit suite and the real local C++/Python RAG
integration. It also runs a real, Unicode-path coding workflow against a tiny
CMake project: the initial CTest fails, the Runtime edits source, persistence is
failed after the filesystem effect, a fresh Runtime resumes the durable task,
reconciles the repeated edit through its SHA conflict, rebuilds, passes the
exact CTest, replays the final log, and evaluates it. The model is scripted and
offline; the production Runtime, JSONL store, workspace/build tools, process
runner, and Python RAG adapter are used. The default CTest suite uses only
deterministic fakes, local processes, and local files; it does not send network
requests.

## Configuration and `.env`

Copy `.env.example` to `.env`, fill `AGENT_BASE_URL`, `AGENT_MODEL`, and
exactly one of `AGENT_API_KEY` or `AGENT_AUTH_TOKEN`, then keep `.env` local.
The example contains empty credential fields and safe defaults only. `.env`
and runtime data are ignored by Git; never put credentials on the command line
or commit them.

The executable loads an env file only when it is named explicitly. It does not
search parent directories:

```powershell
& .\build\vs2022\Debug\agent.exe --env-file .env run `
  --workspace . --issue "Fix the current warning"
```

Without valid provider configuration, `run` exits with code 2 before any
provider request. After each successful event flush and state commit, `run`
prints exactly one safe progress line:

```text
task_id=<validated> sequence=<n> event=<fixed-name> status=<fixed-name>
```

The observer and renderer never receive an event payload. Terminal and durable
fatal failures print only a validated task ID when available, fixed status and
error-code names, and a fixed actionable summary; raw runtime error messages
are not printed. Unknown enum values render as `Unknown`. Successful tasks then
print final model text through the existing bounded UTF-8 renderer.

## Resume a durable task

`resume` loads exactly
`<AGENT_RUNTIME_ROOT>/tasks/<task-id>/events.jsonl`, validates that every event
belongs to the requested ID, replays it, and continues from the durable phase:

```powershell
& .\build\vs2022\Debug\agent.exe --env-file .env resume `
  --task-id task-0123456789abcdef0123456789abcdef
```

A terminal task is idempotent: it returns the durable final result without an
external call or another event. An in-flight model call is reissued from the
exact durable `ModelRequest`; an active tool call is reexecuted from the exact
durable `ToolCall`. This is at-least-once recovery. The file tools' expected
SHA-256 and exact match count make an already-applied edit return a model-visible
conflict, after which the model can reread and reconcile. The Runtime never
claims exactly-once external effects.

Durable cumulative model/tool counts continue across attempts. The wall-clock
limit is monotonic only within one process attempt and restarts for an explicit
resume; this is a documented V1 limitation, not a durable elapsed-time budget.
Those counters measure persisted logical call intents. Reexecuting the same
in-flight intent does not increment them, so repeated operator resumes after
pre-outcome crashes are not a globally bounded count of physical attempts.
Nonterminal resume needs the same local Provider/tool/RAG configuration as a
fresh run. A terminal resume makes no Provider request.

Resume storage is more restrictive than arbitrary `verify-log` input: it opens
the runtime root, `tasks`, task directory, and `events.jsonl` without following
link/reparse components, requires a regular single-link leaf, and binds the
decoded task ID to the requested ID before any Provider, RAG, or tool call.

## Verify an event log

Each task is stored under
`<AGENT_RUNTIME_ROOT>/tasks/<task-id>/events.jsonl`. Verification is a local,
credential-independent path: it does not load an env file, provider
configuration, HTTP transport, or model client. Use the exact binding:

```powershell
& .\build\vs2022\Debug\agent.exe verify-log `
  --events .\runtime_data\tasks\<task-id>\events.jsonl
```

Verification prints only the validated task ID, current replayed status, and
last sequence. It rejects malformed JSON, unknown schema, noncontiguous or mixed
task sequences, invalid transitions, and unsafe task IDs without modifying the
file. Combining `verify-log` with `--env-file` is invalid input.

## Evaluate an event log

Evaluation is also credential-independent and offline:

```powershell
& .\build\vs2022\Debug\agent.exe evaluate-log `
  --events .\runtime_data\tasks\<task-id>\events.jsonl
```

It first performs the same strict replay, then prints only fixed fields: the
verdict, terminal status, model/tool counts, evidence rounds/items, model
requests carrying evidence, tool-error results, and last sequence. `pass`
means the durable state is `Completed`, has nonempty final text, and has no
pending model/tool/error state. A valid log that does not meet that contract
returns the task-failed exit code; a malformed or invalid log returns the
invalid-event-log exit code. `evaluate-log` never loads an env file, creates a
Provider client, or starts RAG. Combining it with `--env-file` is invalid input.

Event files are local audit records, not public logs. They can contain the
issue, workspace path, prompts, evidence, tool inputs/results, model text, and
sanitized error messages. Credentials and authentication headers are excluded,
but user or model content is not a general-purpose secret scrubber. Protect
the runtime directory with appropriate local access controls and do not add it
to Git.

## Benchmark the controlled Runtime paths

`agent_benchmark` is a credential-free Release target for repeatable software
benchmarking. It uses an in-process scripted Provider and an in-memory event
store so the result measures the Agent Runtime paths rather than network or
model-inference time:

```powershell
cmake --build build/vs2022 --config Release --target agent_benchmark

& .\build\vs2022\Release\agent_benchmark.exe `
  --warmup 10 `
  --iterations 100 `
  --batch-size 1000 `
  --output benchmarks/results/local-baseline.json

& .\build\vs2022\Release\agent_benchmark.exe `
  --warmup 10 `
  --iterations 100 `
  --batch-size 1000 `
  --baseline benchmarks/results/local-baseline.json `
  --max-regression-percent 15 `
  --output benchmarks/results/local-comparison.json
```

The three fixed scenarios cover one-round text completion, a two-round tool
call, and offline event-log evaluation. Each timed sample averages a batch of
operations to reduce timer and scheduler noise. Each schema-v2 report records
the batch-sample count, operation/success/error counts, actual measured wall
time, per-operation mean/P50/P95/P99/max latency, throughput, and success rate.
The benchmark target refreshes source commit and benchmark-source dirty state at
build time. A baseline is accepted only when its schema, workload parameters,
method, platform/compiler/build configuration, scenario set, and metric
relationships are compatible, including summary counts that match the declared
workload. Baseline and output must be distinct filesystem targets so comparison
cannot overwrite its evidence; the comparison report records the baseline path
and SHA-256. Comparison fails when P95 latency or throughput regresses beyond
the chosen percentage, or when success rate is below 100%. The checked-in
Windows run uses a 15% policy after repeated local noise calibration; another
machine should establish its own baseline and threshold under idle, comparable
conditions.

This is a controlled **software/Runtime benchmark**. It does not measure GPU,
model inference, Provider-network latency, or production concurrency. See
[`benchmarks/README.md`](benchmarks/README.md) for methodology, interpretation,
and the checked-in sample reports.

## Opt-in live provider smoke

The normal build does not create or register a live test. A credentialed smoke
target exists only when explicitly configured with
`AGENT_ENABLE_LIVE_TESTS=ON`:

```powershell
cmake -S . -B build/live-vs2022 -G "Visual Studio 17 2022" -A x64 `
  -DAGENT_ENABLE_LIVE_TESTS=ON
cmake --build build/live-vs2022 --config Debug --target anthropic_live_smoke
ctest --test-dir build/live-vs2022 -C Debug -R anthropic_live_smoke `
  --output-on-failure
```

The live process reads provider settings from its environment, sends one
no-tool prompt, requires nonempty text, and prints only a status and a bounded
request ID. Enabling the target authorizes neither a provider charge nor a
network call by itself; run it only with valid test credentials and explicit
authorization. Offline CTest success does not establish that this live smoke
passed.

The default offline verification for this milestone keeps the live target
disabled and does not make a Provider network request. The real Provider smoke
was not run. The Unicode renderer and the real Runtime/CLI/JSONL composition
are covered offline, but a successful Unicode `run` through a real Windows
console process remains an explicit Minor evidence gap rather than a claimed
fix.

## Local backup remote

This checkout uses a Git remote named `backup` that points to a local bare
repository. Mirror the current branch and compare commit IDs with:

```powershell
$branch = git branch --show-current
git push backup $branch
$local = git rev-parse HEAD
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ($local -ne $remote) { throw "backup commit mismatch" }
```

The `backup` remote is a machine-local Git copy. It is not GitHub, an off-site
backup, or a backup of ignored `.env` files and runtime event data.
