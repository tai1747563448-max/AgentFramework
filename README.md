# AgentFramework Runtime Kernel

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
- `src/main.cpp` is the composition root; `src/cli` parses `run` and
  `verify-log`, uses fixed failure messages, and bounds the fields printed by
  progress and log verification. A successful `run` renders final text as
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
a vector database, crash resumption, automatic retry policy, parallel
execution, multiple agents, a second provider, an HTTP service, a TUI, MCP, or
plugins.

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
  --source . `
  --index .\.rag\knowledge.sqlite3
$index = (Resolve-Path .\.rag\knowledge.sqlite3).Path
```

The builder accepts a bounded set of text/code extensions, skips secrets and
protected directories, rejects links and multiply linked files, chunks in
stable path/line order, and atomically replaces the SQLite file. Rebuilding
removes stale chunks. Its result and failures contain only bounded summaries;
source text and local absolute paths are not printed as diagnostics.

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
become durable. Source IDs remain relative `path#Lx-Ly` citations; metadata
retains relative path, line range, content SHA-256, and score. The Runtime,
Reducer, and JSONL replay all enforce the same bounds, so a faulty sidecar or a
forged event cannot bypass the invariant.

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
integration. The default CTest suite uses only deterministic fakes, local
processes, and local files; it does not send network requests.

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

Event files are local audit records, not public logs. They can contain the
issue, workspace path, prompts, evidence, tool inputs/results, model text, and
sanitized error messages. Credentials and authentication headers are excluded,
but user or model content is not a general-purpose secret scrubber. Protect
the runtime directory with appropriate local access controls and do not add it
to Git.

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
