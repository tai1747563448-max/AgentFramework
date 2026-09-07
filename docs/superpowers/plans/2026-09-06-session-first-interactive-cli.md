# Session-first Interactive CLI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a no-argument, resumable, multi-turn `AgentFramework.exe` whose durable Session transcript supplies exact prior messages to one Runtime Task per turn.

**Architecture:** Add a Session domain/event log and SessionEngine above the existing RuntimeEngine. Extend Runtime task-start events with optional, strictly validated initial messages and session linkage while preserving legacy one-shot event JSON when those fields are absent.

**Tech Stack:** C++17, nlohmann/json, CMake 3.21+, existing custom C++ test harness, CTest, Windows/MSVC with portable POSIX-compatible filesystem code where practical.

**Spec:** `docs/superpowers/specs/2026-09-06-session-first-interactive-cli-design.md`

## Global Constraints

- Preserve `run`, `resume`, `verify-log`, and `evaluate-log` behavior and legacy schema-v1 fixture compatibility.
- Never persist the API key, bearer token, or base URL in Session/Task events.
- Store Session events under `<AGENT_RUNTIME_ROOT>/sessions/<session-id>/events.jsonl` and Task events under the existing tasks path.
- Store complete successful-turn `Message` blocks; failed turns retain their user input and linked Task trace but do not enter active context.
- Do not implement or claim automatic compact, fork, streaming tokens, full-screen TUI, or shell mode in this phase.
- Do not restore the user-deleted historical documentation.
- Do not commit automatically in the current dirty checkout; `CMakeLists.txt` contains a pre-existing pytest migration.

---

### Task 1: Durable Runtime conversation prefix

**Files:**
- Modify: `src/application/runtime_engine.h`
- Modify: `src/application/runtime_engine.cpp`
- Modify: `src/domain/runtime_event.h`
- Modify: `src/domain/task_state.h`
- Modify: `src/application/state_reducer.cpp`
- Modify: `src/adapters/persistence/event_json.cpp`
- Test: `tests/application/state_reducer_test.cpp`
- Test: `tests/application/runtime_engine_test.cpp`
- Test: `tests/domain/runtime_event_test.cpp`
- Test: `tests/adapters/jsonl_event_store_test.cpp`

**Interfaces:**
- Produces: `SessionTaskLink`, `RunRequest::initial_messages`, `RunRequest::requested_task_id`, `RunRequest::session_link`.
- Produces: `bool conversation_history_is_valid(const std::vector<Message>&)`.
- Preserves: old aggregate initialization by appending fields with defaults.

- [x] **Step 1: Write failing reducer and engine tests**

Add literal prior messages containing assistant tool use and the matching user tool result. Assert the first generated `ModelRequest.messages` equals:

```cpp
std::vector<agent::Message>{
    {agent::Role::User, {agent::TextBlock{"first"}}},
    {agent::Role::Assistant, {agent::ToolUseBlock{call}}},
    {agent::Role::User, {agent::ToolResultBlock{result}}},
    {agent::Role::Assistant, {agent::TextBlock{"first answer"}}},
    {agent::Role::User, {agent::TextBlock{"second"}}},
};
```

Also assert an explicitly requested valid task ID is used and invalid/unbalanced histories fail before model, knowledge, or tool calls.

- [x] **Step 2: Run focused tests and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_engine_tests state_reducer_tests runtime_event_tests jsonl_event_store_tests
ctest --test-dir build/vs2022 -C Debug -R "runtime_engine_tests|state_reducer_tests|runtime_event_tests|jsonl_event_store_tests" --output-on-failure
```

Expected: compile or assertion failure because the new fields and behavior do not exist.

- [x] **Step 3: Implement minimal Runtime support**

Define:

```cpp
struct SessionTaskLink {
    std::string session_id;
    std::uint64_t turn_index{0};
};

struct RunRequest {
    std::string issue;
    std::string workspace_utf8;
    std::string system_prompt;
    RuntimeBudgets budgets;
    std::vector<Message> initial_messages;
    std::optional<std::string> requested_task_id;
    std::optional<SessionTaskLink> session_link;
};
```

Load initial messages in the first reducer transition, append the current user message, and strictly validate role/tool pairing. Serialize the two optional TaskStarted fields only when present.

- [x] **Step 4: Run focused tests and verify GREEN**

Run the focused build and CTest command from Step 2. Expected: all four targets pass.

### Task 2: Session domain reducer and strict JSON codec

**Files:**
- Create: `src/domain/session_event.h`
- Create: `src/domain/session_event.cpp`
- Create: `src/domain/session_state.h`
- Create: `src/application/session_reducer.h`
- Create: `src/application/session_reducer.cpp`
- Create: `src/adapters/persistence/session_event_json.h`
- Create: `src/adapters/persistence/session_event_json.cpp`
- Create: `tests/application/session_reducer_test.cpp`
- Create: `tests/adapters/session_event_json_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Message`, `RuntimeError`, `TaskStatus`, and task/session ID validators.
- Produces: `SessionEvent`, `SessionState`, `reduce_session_event`, `replay_session_events`, `session_event_to_json`, `session_event_from_json`.

- [x] **Step 1: Write failing reducer and codec tests**

Cover a two-turn happy path and literal invalid traces: first sequence not one, mismatched session ID, second pending Turn, wrong task ID on commit, noncontiguous sequence, extra JSON key, malformed message role/block pairing, and post-commit message mismatch.

- [x] **Step 2: Run focused tests and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target session_reducer_tests session_event_json_tests
```

Expected: target/source compilation failure because the Session domain does not exist.

- [x] **Step 3: Implement the minimal reducer and codec**

Use these event payloads:

```cpp
struct SessionStartedPayload { std::string workspace_utf8; std::string model; };
struct SessionTurnStartedPayload {
    std::uint64_t turn_index;
    std::string task_id;
    std::string user_text;
};
struct SessionTurnCommittedPayload {
    std::uint64_t turn_index;
    std::string task_id;
    std::vector<Message> messages;
};
struct SessionTurnFailedPayload {
    std::uint64_t turn_index;
    std::string task_id;
    TaskStatus status;
    std::string summary;
};
```

The reducer owns sequencing and pending-Turn legality. The codec uses exact-key validation and rejects duplicate JSON keys via the existing parsing boundary.

- [x] **Step 4: Run focused tests and verify GREEN**

Run the build targets and CTest regex for the two new tests. Expected: both pass.

### Task 3: Filesystem SessionStore

**Files:**
- Create: `src/ports/session_store.h`
- Create: `src/adapters/persistence/jsonl_session_store.h`
- Create: `src/adapters/persistence/jsonl_session_store.cpp`
- Create: `tests/adapters/jsonl_session_store_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Session event codec and `is_valid_session_id`.
- Produces: `append`, `read_session`, `list_sessions`, and `session_event_path`.

- [x] **Step 1: Write failing real-filesystem tests**

Use `ScopedTempDir`; append and reload a two-event Session, list two Sessions in descending last-timestamp order, and assert invalid ID, truncated JSON, extra keys, and link/symlink escape fail closed.

- [x] **Step 2: Run focused test and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target jsonl_session_store_tests
```

Expected: target/source compilation failure.

- [x] **Step 3: Implement minimal append/read/list behavior**

Resolve only validated IDs under:

```cpp
runtime_root / "sessions" / session_id / "events.jsonl"
```

Create directories without following an existing symlink/reparse-point component, append exactly one JSON line through native no-follow/single-link handles, durably flush with `FlushFileBuffers`/`fsync`, then replay the entire file on read. Listing ignores non-ID directory names and returns an error for a valid-looking but corrupted Session rather than silently selecting it.

- [x] **Step 4: Run focused test and verify GREEN**

Run the target and its CTest entry. Expected: pass, with platform symlink case skipped only when symlink creation is unavailable.

### Task 4: SessionEngine orchestration and crash recovery

**Files:**
- Create: `src/application/session_engine.h`
- Create: `src/application/session_engine.cpp`
- Create: `tests/application/session_engine_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `RuntimeEngine`, `SessionStore`, `EventStore`, `Clock`, and injected session/task ID factories.
- Produces: `create_session`, `submit_turn`, `recover_pending_turn`, `load_session`, and `list_sessions`.

- [x] **Step 1: Write failing orchestration tests**

Assert:

```text
turn_started append -> Runtime run/resume -> turn_committed append
```

Verify the second Turn receives the first Turn's exact committed messages. Simulate a pending Turn with no Task log, a nonterminal Task log, and a completed Task log; the completed case must make zero model/tool calls while committing the Session.

- [x] **Step 2: Run focused test and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target session_engine_tests
```

Expected: target/source compilation failure.

- [x] **Step 3: Implement minimal orchestration**

Preallocate the task ID and durably append `turn_started` before invoking Runtime. On recovery, load one durable Task event snapshot, replay and validate that snapshot, then pass those exact events to Runtime without rereading. On success, verify that `TaskState.messages` begins byte-for-byte with the Session's prior messages and current user message, then commit only the suffix. On terminal failure append `turn_failed` with a fixed safe summary; on persistence failure return a fatal persistence error without pretending the Session committed.

- [x] **Step 4: Run focused test and verify GREEN**

Run the target and its CTest entry. Expected: pass.

### Task 5: Interactive terminal and no-argument startup

**Files:**
- Create: `src/cli/interactive_cli.h`
- Create: `src/cli/interactive_cli.cpp`
- Create: `tests/cli/interactive_cli_test.cpp`
- Modify: `src/config/runtime_config.h`
- Modify: `src/config/runtime_config.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: SessionEngine and RuntimeConfig model/runtime root.
- Produces: `InteractiveCli::run()`, `discover_interactive_env_file(executable_path, cwd)`, and a Windows executable whose output name is `AgentFramework`.

- [x] **Step 1: Write failing CLI tests**

Drive `InteractiveCli` with `std::istringstream` and assert observable output/state for first workspace prompt, normal two-turn input, `/status`, `/new`, `/clear`, `/resume <id>`, unknown slash command, `/exit`, and EOF. Assert empty/whitespace-only chat lines do not create Turns.

Add config-discovery tests with real temporary directories: executable-adjacent `.env` wins over cwd `.env`; missing files return no result; explicit CLI env remains authoritative.

- [x] **Step 2: Run focused tests and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target interactive_cli_tests cli_tests
```

Expected: the new target fails to build; existing CLI tests remain green.

- [x] **Step 3: Implement the terminal loop and main wiring**

Route `args.size() == 1` to the interactive path. Route all argument-bearing calls to existing `CliApp`. Auto-load only for interactive mode and never for credential-free commands. Display:

```text
AgentFramework
Model: <model>
Workspace: <workspace>
Session: <session-id>

>
```

Reuse the existing safe terminal rendering function by moving it to a small CLI utility if necessary; do not duplicate weaker sanitization.

- [x] **Step 4: Run focused tests and verify GREEN**

Run the target and CTest entries. Expected: pass and no change in old CLI assertions.

### Task 6: Process-level two-turn acceptance and packaging

**Files:**
- Create: `tests/cli/interactive_process_test.cpp`
- Modify: `CMakeLists.txt`
- Create locally (ignored): `out/AgentFramework/.env`

**Interfaces:**
- Consumes: built `AgentFramework.exe` and a deterministic local scripted provider fixture.
- Produces: a double-clickable portable directory under `out/AgentFramework`.

- [x] **Step 1: Write the failing process test**

Feed a workspace path, two prompts, `/status`, and `/exit` over stdin. The scripted provider must assert the second request contains the first user/assistant exchange and return two literal answers. Verify Session and both linked Task logs exist.

- [x] **Step 2: Run the process test and verify RED**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target AgentFramework
ctest --test-dir build/vs2022 -C Debug -R interactive_process_tests --output-on-failure
```

Expected: fail before process test registration/implementation is complete.

- [x] **Step 3: Complete wiring and portable output**

Set the executable output name to `AgentFramework`, copy required runtime DLLs as already done for `agent`, and populate ignored `out/AgentFramework-Ready` from the verified Release build. Copy the existing ignored `.env` without printing it; rewrite only `AGENT_RUNTIME_ROOT` in the copied configuration to an absolute portable data path for stable double-click behavior.

- [x] **Step 4: Run acceptance and full verification**

Run:

```powershell
cmake --build build/vs2022 --config Debug
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: all offline tests pass, including the two-turn process acceptance test.

- [x] **Step 5: Optional real MiniMax smoke**

Only when the ignored configuration is present, run `AgentFramework.exe`, submit two harmless context-dependent prompts, verify the second answer uses the first Turn, then `/exit`. Record only status, Session ID, Task IDs, and redacted outcome; never print the credential.

## Self-review

- Spec coverage: Runtime prefix, strict Session log, crash recovery, interactive commands, env discovery, compatibility, process acceptance, and portable output each map to one task.
- Deferred features are explicitly excluded rather than represented by placeholders.
- Interface names are consistent across tasks: `SessionTaskLink`, `SessionEvent`, `SessionState`, `SessionStore`, `SessionEngine`, and `InteractiveCli`.
- Existing user changes remain outside automatic commits.
