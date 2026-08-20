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
  durable commit with only task ID, sequence, event kind, and task status.
- `src/ports` defines model, tool, knowledge, event-store, clock, ID, and
  cancellation interfaces.
- `src/adapters` supplies Anthropic Messages HTTP, JSONL persistence, empty
  tool/knowledge adapters, and system clock/ID/cancellation implementations.
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

This milestone does not provide real coding tools, real RAG, crash recovery,
automatic retry, parallel execution, multiple agents, a second provider, an
HTTP service, a TUI, MCP, or plugins.

## Build and offline tests on Visual Studio 2022

From a Visual Studio-capable PowerShell in the repository root:

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --parallel
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

CMake configuration may obtain pinned third-party dependencies through
`FetchContent` when they are not already cached. The default CTest suite uses
only deterministic fakes or local files and does not send network requests.

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

Verification prints only the validated task ID, terminal status, and last
sequence. It rejects malformed JSON, unknown schema, noncontiguous or mixed
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
