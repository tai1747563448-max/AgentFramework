# Runtime Kernel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a C++17 single-agent Runtime Kernel that drives an explicit task state machine, records replayable JSONL events, talks to one Anthropic-compatible model adapter, and exposes `run` and `verify-log` CLI commands.

**Architecture:** Domain types and the pure reducer sit at the center; `RuntimeEngine` depends only on Port interfaces. CLI, Anthropic HTTP, empty Tool/RAG implementations, clocks, IDs, and JSONL persistence are Adapters wired by `main.cpp`. Every durable transition follows append-then-reduce, and the first release stays single-process, single-task, single-threaded, and sequential.

**Tech Stack:** C++17; CMake 3.21 minimum; Visual Studio 2022/MSVC x64; CTest; nlohmann/json 3.12.0; cpr 1.14.2; dotenv-cpp commit `9275210b8abf1a551fb81e0ba45866286d764acd`.

## Global Constraints

- Implement only the Runtime Kernel described in `docs/superpowers/specs/2026-08-17-runtime-kernel-design.md`.
- Do not copy production files from `D:\Users\Lenovo\AGENT`; use it only as behavioral background.
- Do not implement real coding tools, real RAG, recovery, automatic retry, multi-Agent, HTTP service, TUI, MCP, or plugins.
- Keep Domain independent of cpr, dotenv-cpp, nlohmann/json, CLI, and filesystem I/O.
- Keep one task and one external call in flight at a time; execute multiple tool calls in model response order.
- Persist an event successfully before applying it to `TaskState`.
- Never put API keys, authorization headers, `.env`, runtime events, model bodies from failed requests, or local knowledge data in Git or test output.
- Use behavior-first tests: observe RED, implement the minimum behavior, then observe GREEN.
- At execution time, use `superpowers:using-git-worktrees` before creating the `feat/runtime-kernel` worktree/branch.
- After every task Commit, push the current branch to `backup` and verify the local and backup Commit IDs match.
- Configure MSVC with `cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64`.
- Build with `cmake --build build/vs2022 --config Debug --parallel` and run tests with `ctest --test-dir build/vs2022 -C Debug --output-on-failure`.

## File Map

```text
CMakeLists.txt                              C++17 build, dependency pins, targets, CTest
.env.example                               Names only; no credential values
README.md                                  Build, run, event, security, and live-smoke guide
src/main.cpp                               Composition Root only
src/domain/value.h/.cpp                    Provider-neutral recursive value type
src/domain/runtime_error.h                 ErrorCode and RuntimeError
src/domain/result.h                        C++17 Result<T> and Result<void>
src/domain/model_types.h                   Messages, tool calls/results, evidence, model request/response
src/domain/task_state.h                     Task status, budgets, usage, current/pending state
src/domain/runtime_event.h/.cpp             Typed event payloads and EventKind mapping
src/application/state_reducer.h/.cpp        Pure legal-transition and replay logic
src/application/runtime_engine.h/.cpp       Single-task orchestration and append-then-reduce
src/ports/model_client.h                    Model completion Port
src/ports/tool_gateway.h                    Tool definitions/execution Port
src/ports/knowledge_provider.h              Evidence retrieval Port
src/ports/event_store.h                     Event append/read Port
src/ports/clock.h                           UTC and monotonic time Port
src/ports/id_generator.h                    Task/correlation ID Port
src/ports/cancellation.h                    Cancellation Port
src/adapters/json/value_json.h/.cpp         Value <-> nlohmann::json mapping
src/adapters/persistence/event_json.h/.cpp  RuntimeEvent <-> JSON mapping
src/adapters/persistence/jsonl_event_store.h/.cpp  Append, flush, read, validation
src/adapters/anthropic/http_transport.h     Provider-independent HTTP request/response Port
src/adapters/anthropic/cpr_http_transport.h/.cpp  cpr transport
src/adapters/anthropic/anthropic_messages_client.h/.cpp  Messages protocol mapping
src/adapters/empty/empty_tool_gateway.h/.cpp       No production tools in this subproject
src/adapters/empty/empty_knowledge_provider.h/.cpp No production RAG in this subproject
src/adapters/system/system_clock.h/.cpp      UTC and monotonic clocks
src/adapters/system/random_id_generator.h/.cpp     Stable-format random IDs
src/adapters/system/signal_cancellation.h/.cpp     Ctrl+C state
src/config/runtime_config.h/.cpp             Environment and optional .env-file configuration
src/cli/cli_app.h/.cpp                       Argument parsing, status text, exit codes
tests/test_support.h                         Minimal assertion and registration helpers
tests/test_main.cpp                          Shared test runner
tests/domain/value_result_test.cpp           Domain primitive tests
tests/domain/runtime_event_test.cpp          Event vocabulary tests
tests/application/state_reducer_test.cpp     State/replay tests
tests/adapters/jsonl_event_store_test.cpp    Persistence tests
tests/application/runtime_engine_test.cpp    Orchestration, tools, budgets, cancellation
tests/adapters/anthropic_messages_client_test.cpp  Offline protocol tests
tests/cli/cli_app_test.cpp                   CLI/config/exit-code tests
tests/integration/runtime_integration_test.cpp     Fake end-to-end and Unicode path test
tests/live/anthropic_live_smoke.cpp           Opt-in real-provider smoke test
```

---

### Task 1: Establish the C++17 build and provider-neutral domain primitives

**Files:**
- Create: `CMakeLists.txt`
- Create: `src/domain/value.h`
- Create: `src/domain/value.cpp`
- Create: `src/domain/runtime_error.h`
- Create: `src/domain/result.h`
- Create: `tests/test_support.h`
- Create: `tests/test_main.cpp`
- Create: `tests/domain/value_result_test.cpp`

**Interfaces:**
- Consumes: no project code; uses only the C++17 standard library.
- Produces: `agent::Value`, `agent::ErrorCode`, `agent::RuntimeError`, `agent::Result<T>`, and `agent::Result<void>` for every later task.

- [ ] **Step 1: Create the build shell and the failing primitive test**

Create the pinned dependency declarations and a temporary INTERFACE `agent_kernel` so configuration succeeds while the test fails because the domain headers do not exist:

```cmake
cmake_minimum_required(VERSION 3.21)
project(AgentFramework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPR_BUILD_TESTS_SSL OFF CACHE BOOL "" FORCE)
set(CPR_CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.14.2
    GIT_SHALLOW TRUE)
FetchContent_Declare(
    dotenv_source
    GIT_REPOSITORY https://github.com/laserpants/dotenv-cpp.git
    GIT_TAG 9275210b8abf1a551fb81e0ba45866286d764acd
    SOURCE_SUBDIR _header_only)
FetchContent_MakeAvailable(nlohmann_json cpr dotenv_source)

add_library(dotenv_header_only INTERFACE)
target_include_directories(dotenv_header_only INTERFACE
    "${dotenv_source_SOURCE_DIR}/include/laserpants/dotenv")

add_library(agent_kernel INTERFACE)
target_include_directories(agent_kernel INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

enable_testing()
function(add_agent_test target source)
    add_executable(${target} tests/test_main.cpp ${source})
    target_link_libraries(${target} PRIVATE agent_kernel)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME ${target} COMMAND ${target})
endfunction()
add_agent_test(domain_primitives_tests tests/domain/value_result_test.cpp)
```

Create a small test registry in `test_support.h` with `TEST_CASE(name)` and `REQUIRE(expr)` macros. `test_main.cpp` must run every registered case, print one PASS/FAIL line per case, and return nonzero if any case throws.

The failing test must request recursive objects/arrays and both Result branches:

```cpp
#include "domain/result.h"
#include "domain/value.h"
#include "test_support.h"

TEST_CASE(value_preserves_recursive_structure) {
    agent::Value value = agent::Value::object({
        {"command", agent::Value("cmake --build build")},
        {"flags", agent::Value::array({agent::Value("Debug"), agent::Value(true)})}
    });
    REQUIRE(value.at("command").as_string() == "cmake --build build");
    REQUIRE(value.at("flags").as_array().at(1).as_bool());
}

TEST_CASE(result_separates_value_from_error) {
    auto ok = agent::Result<int>::success(7);
    auto bad = agent::Result<int>::failure(
        {agent::ErrorCode::InvalidInput, "missing issue", false});
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 7);
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().code == agent::ErrorCode::InvalidInput);
}
```

- [ ] **Step 2: Configure and build to verify RED**

Run:

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --target domain_primitives_tests
```

Expected: configuration succeeds and compilation fails because `domain/value.h` and `domain/result.h` are missing.

- [ ] **Step 3: Implement the minimal primitives and make `agent_kernel` concrete**

Use this public shape for `Value` so Domain never exposes nlohmann/json:

```cpp
namespace agent {
class Value {
public:
    struct ArrayNode;
    struct ObjectNode;
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                                 std::string,
                                 std::shared_ptr<const ArrayNode>,
                                 std::shared_ptr<const ObjectNode>>;

    Value();
    Value(bool value);
    Value(std::int64_t value);
    Value(double value);
    Value(std::string value);
    Value(const char* value);
    static Value array(Array values);
    static Value object(Object values);

    bool is_array() const noexcept;
    bool is_object() const noexcept;
    bool is_integer() const noexcept;
    bool is_double() const noexcept;
    const Array& as_array() const;
    const Object& as_object() const;
    const std::string& as_string() const;
    bool as_bool() const;
    std::int64_t as_integer() const;
    double as_double() const;
    const Value& at(const std::string& key) const;
    const Storage& storage() const noexcept;
    friend bool operator==(const Value& left, const Value& right);
private:
    Storage storage_;
};
struct Value::ArrayNode { Array values; };
struct Value::ObjectNode { Object values; };
}
```

Define every `ErrorCode` from the spec and this error record:

```cpp
enum class ErrorCode {
    InvalidInput, InvalidConfiguration, PersistenceFailure,
    TransportFailure, RequestTimeout, HttpFailure, ProtocolFailure,
    DependencyUnavailable, InvalidTransition, BudgetExceeded, Cancelled
};
struct RuntimeError {
    ErrorCode code;
    std::string message;
    bool retryable;
};
```

Implement `Result<T>` with `std::variant<T, RuntimeError>`, factories `success(T)` and `failure(RuntimeError)`, and checked `value()`/`error()` accessors. Implement the `void` specialization with a success flag plus optional error.

Replace the INTERFACE library with:

```cmake
add_library(agent_kernel STATIC src/domain/value.cpp)
target_include_directories(agent_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(agent_kernel PRIVATE /W4 /permissive- /utf-8)
endif()
```

- [ ] **Step 4: Run GREEN and the complete current test set**

Run:

```powershell
cmake -S . -B build/vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/vs2022 --config Debug --target domain_primitives_tests
ctest --test-dir build/vs2022 -C Debug -R domain_primitives_tests --output-on-failure
```

Expected: `1/1` test passes.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/domain tests/test_support.h tests/test_main.cpp tests/domain/value_result_test.cpp
git commit -m "feat: establish runtime domain primitives"
$branch = git branch --show-current
git push -u backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 2: Define the typed model, task, and event vocabulary

**Files:**
- Create: `src/domain/model_types.h`
- Create: `src/domain/task_state.h`
- Create: `src/domain/runtime_event.h`
- Create: `src/domain/runtime_event.cpp`
- Create: `tests/domain/runtime_event_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Value`, `RuntimeError`, and `Result<T>` from Task 1.
- Produces: `Role`, `ContentBlock`, `Message`, `ToolDefinition`, `ToolCall`, `ToolResult`, `EvidencePack`, `ModelRequest`, `ModelResponse`, `RuntimeBudgets`, `TaskState`, `EventKind`, `EventPayload`, `RuntimeEvent`, and `event_kind(const EventPayload&)`.

- [ ] **Step 1: Add a failing event-vocabulary test**

Register `runtime_event_tests` in CMake and create tests that require exact event-kind mapping and terminal status detection:

```cpp
TEST_CASE(event_kind_is_derived_from_typed_payload) {
    agent::EventPayload payload = agent::ModelCallFailedPayload{
        {agent::ErrorCode::RequestTimeout, "model timeout", true}};
    REQUIRE(agent::event_kind(payload) == agent::EventKind::ModelCallFailed);
}

TEST_CASE(only_four_task_statuses_are_terminal) {
    REQUIRE(agent::is_terminal(agent::TaskStatus::Completed));
    REQUIRE(agent::is_terminal(agent::TaskStatus::Failed));
    REQUIRE(agent::is_terminal(agent::TaskStatus::BudgetExceeded));
    REQUIRE(agent::is_terminal(agent::TaskStatus::Cancelled));
    REQUIRE(!agent::is_terminal(agent::TaskStatus::AwaitingModel));
}
```

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target runtime_event_tests`.

Expected: compilation fails because `domain/runtime_event.h` does not exist.

- [ ] **Step 3: Implement the complete typed vocabulary**

Use `std::variant<TextBlock, ToolUseBlock, ToolResultBlock>` for `ContentBlock`. `ToolCall.arguments`, `ToolDefinition.input_schema`, and Evidence metadata use `Value`. `ModelResponse` contains ordered content blocks, `StopReason`, usage counts, and provider request ID.

Use these task structures:

```cpp
enum class TaskStatus {
    Created, PreparingContext, AwaitingModel, AwaitingTool,
    Completed, Failed, BudgetExceeded, Cancelled
};
struct RuntimeBudgets {
    std::size_t max_model_rounds{16};
    std::size_t max_tool_calls{64};
    std::int64_t max_task_time_ms{1'800'000};
    std::int64_t model_timeout_ms{120'000};
};
struct RuntimeUsage {
    std::size_t model_rounds{0};
    std::size_t tool_calls{0};
};
struct TaskState {
    std::string task_id;
    TaskStatus status{TaskStatus::Created};
    std::string issue;
    std::string workspace_utf8;
    RuntimeBudgets budgets;
    RuntimeUsage usage;
    std::uint64_t last_sequence{0};
    std::vector<Message> messages;
    EvidencePack evidence;
    std::vector<ToolCall> pending_tool_calls;
    std::size_t next_tool_index{0};
    std::optional<std::string> active_tool_call_id;
    std::vector<ToolResult> pending_tool_results;
    std::optional<std::string> final_text;
    std::optional<RuntimeError> terminal_error;
};
bool is_terminal(TaskStatus status) noexcept;
```

Define one payload struct for every event in the spec, including `ContextPreparationFailedPayload`. The fields are fixed as follows:

| Payload | Fields |
|---|---|
| `TaskStartedPayload` | `issue`, `workspace_utf8`, `RuntimeBudgets budgets` |
| `ContextPreparationStartedPayload` | no fields |
| `ContextPreparedPayload` | `EvidencePack evidence` |
| `ContextPreparationFailedPayload` | `RuntimeError error` |
| `ModelCallStartedPayload` | `ModelRequest request` |
| `ModelCallSucceededPayload` | `ModelResponse response` |
| `ModelCallFailedPayload` | `RuntimeError error` |
| `ToolCallStartedPayload` | `ToolCall call` |
| `ToolCallSucceededPayload` | `ToolResult result` |
| `ToolCallFailedPayload` | `std::string tool_call_id`, `RuntimeError error` |
| `TaskCompletedPayload` | `std::string final_text` |
| `TaskFailedPayload` | `RuntimeError error` |
| `TaskBudgetExceededPayload` | `std::string budget_name`, `RuntimeError error` |
| `TaskCancelledPayload` | `std::string reason`, `RuntimeError error` |

`ModelResponse` contains `std::vector<ContentBlock> content`, `StopReason stop_reason`, `std::string raw_stop_reason`, input/output token counts, and `provider_request_id`. All Domain records used in replay tests implement value equality. Use this event envelope:

```cpp
using EventPayload = std::variant<
    TaskStartedPayload,
    ContextPreparationStartedPayload, ContextPreparedPayload,
    ContextPreparationFailedPayload,
    ModelCallStartedPayload, ModelCallSucceededPayload, ModelCallFailedPayload,
    ToolCallStartedPayload, ToolCallSucceededPayload, ToolCallFailedPayload,
    TaskCompletedPayload, TaskFailedPayload,
    TaskBudgetExceededPayload, TaskCancelledPayload>;

struct RuntimeEvent {
    std::uint32_t schema_version{1};
    std::uint64_t sequence{0};
    std::string task_id;
    std::string timestamp_utc;
    std::string correlation_id;
    EventPayload payload;
};
```

Implement `event_kind` with an exhaustive `std::visit`; do not store a second mutable kind field in `RuntimeEvent`.

- [ ] **Step 4: Run GREEN**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_event_tests
ctest --test-dir build/vs2022 -C Debug -R runtime_event_tests --output-on-failure
```

Expected: `runtime_event_tests` passes and existing primitive tests still pass under full CTest.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/domain tests/domain/runtime_event_test.cpp
git commit -m "feat: define typed runtime events"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 3: Implement legal state reduction and deterministic replay

**Files:**
- Create: `src/application/state_reducer.h`
- Create: `src/application/state_reducer.cpp`
- Create: `tests/application/state_reducer_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `TaskState` and `RuntimeEvent` from Task 2.
- Produces: `Result<TaskState> reduce_event(const std::optional<TaskState>&, const RuntimeEvent&)` and `Result<TaskState> replay_events(const std::vector<RuntimeEvent>&)`.

- [ ] **Step 1: Write failing transition and replay tests**

Cover a text-completion trace, a two-tool trace, invalid sequence, invalid task ID, incomplete tools, and terminal-state rejection. The core assertions are:

```cpp
TEST_CASE(replay_text_completion_is_deterministic) {
    auto events = fixtures::completed_text_trace("task-1", "fix warning", "done");
    auto first = agent::replay_events(events);
    auto second = agent::replay_events(events);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value() == second.value());
    REQUIRE(first.value().status == agent::TaskStatus::Completed);
    REQUIRE(first.value().final_text == std::optional<std::string>{"done"});
}

TEST_CASE(reducer_rejects_event_after_terminal_state) {
    auto state = agent::replay_events(
        fixtures::completed_text_trace("task-1", "fix warning", "done"));
    auto event = fixtures::context_started("task-1", state.value().last_sequence + 1);
    auto result = agent::reduce_event(state.value(), event);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::InvalidTransition);
}
```

Each test source file owns its own `fixtures` namespace. These helpers return fully constructed typed Domain records; they are test-only builders, not undeclared production interfaces and not shared references to temporary objects.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target state_reducer_tests`.

Expected: compilation fails because `application/state_reducer.h` is absent.

- [ ] **Step 3: Implement append-independent pure reduction**

Enforce this exact transition table:

| Payload | Required state | New state / mutation |
|---|---|---|
| `TaskStarted` | no state, sequence 1 | create `Created`, add initial user text message |
| `ContextPreparationStarted` | `Created` or fully processed `AwaitingTool` | flush all pending tool results into one user message, clear pending fields, set `PreparingContext` |
| `ContextPrepared` | `PreparingContext` | replace EvidencePack, set `AwaitingModel` |
| `ContextPreparationFailed` | `PreparingContext` | retain state until `TaskFailed` |
| `ModelCallStarted` | `AwaitingModel` | increment model rounds |
| `ModelCallSucceeded` with tools | `AwaitingModel` | append assistant message, copy ordered calls, set `AwaitingTool` |
| `ModelCallSucceeded` without tools | `AwaitingModel` | append assistant message, retain `AwaitingModel` |
| `ModelCallFailed` | `AwaitingModel` | retain state until `TaskFailed` |
| `ToolCallStarted` | `AwaitingTool`, call equals next pending call, no active call | set `active_tool_call_id`, increment tool calls |
| `ToolCallSucceeded` | ID matches `active_tool_call_id` | append result, clear active ID, advance `next_tool_index` |
| `ToolCallFailed` | ID matches `active_tool_call_id` | clear active ID, retain state until `TaskFailed` |
| `TaskCompleted` | `AwaitingModel`, no tool calls in last response | set final text and `Completed` |
| `TaskFailed` | any nonterminal state | store error and set `Failed` |
| `TaskBudgetExceeded` | any nonterminal state | store error and set `BudgetExceeded` |
| `TaskCancelled` | any nonterminal state | store error and set `Cancelled` |

Every event after the first must have the same task ID, `schema_version == 1`, and `sequence == last_sequence + 1`. Apply `last_sequence` only after all validation succeeds. `replay_events` must stop at the first error and return it.

- [ ] **Step 4: Run reducer tests and the full suite**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target state_reducer_tests
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: all registered tests pass.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/application tests/application/state_reducer_test.cpp
git commit -m "feat: add deterministic runtime reducer"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 4: Add Port interfaces and durable JSONL event persistence

**Files:**
- Create: `src/ports/model_client.h`
- Create: `src/ports/tool_gateway.h`
- Create: `src/ports/knowledge_provider.h`
- Create: `src/ports/event_store.h`
- Create: `src/ports/clock.h`
- Create: `src/ports/id_generator.h`
- Create: `src/ports/cancellation.h`
- Create: `src/adapters/json/value_json.h`
- Create: `src/adapters/json/value_json.cpp`
- Create: `src/adapters/persistence/event_json.h`
- Create: `src/adapters/persistence/event_json.cpp`
- Create: `src/adapters/persistence/jsonl_event_store.h`
- Create: `src/adapters/persistence/jsonl_event_store.cpp`
- Create: `tests/adapters/jsonl_event_store_test.cpp`
- Modify: `tests/test_support.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: all Domain types and `replay_events`.
- Produces: abstract Ports, `value_to_json`, `value_from_json`, `event_to_json`, `event_from_json`, and `JsonlEventStore`.

- [ ] **Step 1: Write failing persistence tests**

Use a unique directory under the test process temporary directory and verify append/read/replay plus corruption rejection:

```cpp
TEST_CASE(jsonl_round_trip_preserves_replay_state) {
    test::ScopedTempDir temp("jsonl-round-trip");
    agent::JsonlEventStore store(temp.path());
    auto events = fixtures::completed_text_trace("task-unicode", "修复警告", "完成");
    for (const auto& event : events) {
        REQUIRE(store.append(event).has_value());
    }
    auto loaded = store.read_file(store.event_path("task-unicode"));
    REQUIRE(loaded.has_value());
    auto state = agent::replay_events(loaded.value());
    REQUIRE(state.has_value());
    REQUIRE(state.value().final_text == std::optional<std::string>{"完成"});
}

TEST_CASE(jsonl_rejects_duplicate_sequence) {
    test::ScopedTempDir temp("jsonl-duplicate");
    auto file = temp.write_text("events.jsonl", fixtures::duplicate_sequence_jsonl());
    agent::JsonlEventStore store(temp.path());
    auto loaded = store.read_file(file);
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::PersistenceFailure);
}
```

Also test invalid JSON, missing required keys, unknown schema, a missing sequence, Windows Unicode path names, and a sentinel secret string absent from the file.

Add `test::ScopedTempDir` to `test_support.h`. It creates one unique child under `std::filesystem::temp_directory_path()`, exposes `path()` and `write_text(relative, content)`, and removes only that exact child in its destructor.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target jsonl_event_store_tests`.

Expected: compilation fails because the persistence adapter is absent.

- [ ] **Step 3: Define the Ports exactly**

```cpp
class ModelClient {
public:
    virtual ~ModelClient() = default;
    virtual Result<ModelResponse> complete(const ModelRequest& request) = 0;
};
class ToolGateway {
public:
    virtual ~ToolGateway() = default;
    virtual std::vector<ToolDefinition> definitions() const = 0;
    virtual Result<ToolResult> execute(const ToolCall& call) = 0;
};
class KnowledgeProvider {
public:
    virtual ~KnowledgeProvider() = default;
    virtual Result<EvidencePack> retrieve(const TaskState& state) = 0;
};
class EventStore {
public:
    virtual ~EventStore() = default;
    virtual Result<void> append(const RuntimeEvent& event) = 0;
    virtual Result<std::vector<RuntimeEvent>> read_file(
        const std::filesystem::path& path) const = 0;
};
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::string now_utc() const = 0;
    virtual std::int64_t monotonic_ms() const = 0;
};
class IdGenerator {
public:
    virtual ~IdGenerator() = default;
    virtual std::string next_task_id() = 0;
    virtual std::string next_correlation_id() = 0;
};
class Cancellation {
public:
    virtual ~Cancellation() = default;
    virtual bool requested() const noexcept = 0;
};
```

`JsonlEventStore` additionally exposes `std::filesystem::path event_path(const std::string& task_id) const` for tests and CLI reporting; this helper is not added to the abstract EventStore Port.

- [ ] **Step 4: Implement explicit JSON codecs and append/flush behavior**

Map `Value` recursively without exposing nlohmann types to Domain. Encode every event with keys `schema_version`, `sequence`, `task_id`, `timestamp`, `event_type`, `correlation_id`, and `payload`. Use lower snake-case event names such as `task_started` and `context_preparation_failed`.

Each payload must have explicit keys; for example:

```json
{
  "schema_version": 1,
  "sequence": 4,
  "task_id": "task-1",
  "timestamp": "2026-08-17T12:00:00.000Z",
  "event_type": "model_call_started",
  "correlation_id": "corr-2",
  "payload": {"request": {"system_prompt": "You are a coding agent runtime.", "messages": [], "tools": []}}
}
```

`JsonlEventStore::append` must create `runtime_root/tasks/<task_id>/`, open `events.jsonl` in binary append mode, write `json.dump()` plus `\n`, call `flush()`, check stream state after write and flush, and return `PersistenceFailure` without mutating external state on failure. `read_file` reads every nonempty line, decodes all events, verifies contiguous sequence and one task ID, and calls `replay_events` once to reject illegal transitions; it does not repair the file.

- [ ] **Step 5: Run persistence tests and full CTest**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target jsonl_event_store_tests
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: JSONL tests and all earlier tests pass.

- [ ] **Step 6: Commit and back up**

```powershell
git add CMakeLists.txt src/ports src/adapters/json src/adapters/persistence tests/adapters/jsonl_event_store_test.cpp tests/test_support.h
git commit -m "feat: persist replayable runtime events"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 5: Drive the final-text and dependency-failure paths in RuntimeEngine

**Files:**
- Create: `src/application/runtime_engine.h`
- Create: `src/application/runtime_engine.cpp`
- Create: `tests/application/runtime_engine_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: all Ports, `reduce_event`, and typed Domain events.
- Produces: `RunRequest`, `RuntimeResult`, and `RuntimeEngine::run(const RunRequest&)`.

- [ ] **Step 1: Write failing engine tests with deterministic Fakes**

Define Fakes inside the test file. The first cases must assert exact event order:

```cpp
TEST_CASE(engine_completes_single_model_turn) {
    test::FakeModel model({fixtures::text_response("done")});
    test::FakeKnowledge knowledge(agent::EvidencePack{});
    test::FakeTools tools;
    test::MemoryEventStore events;
    test::FakeClock clock;
    test::FakeIds ids;
    test::FakeCancellation cancel(false);
    agent::RuntimeEngine engine(model, tools, knowledge, events, clock, ids, cancel);

    auto result = engine.run(fixtures::run_request("fix warning"));
    REQUIRE(result.state.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(events.kinds() == std::vector<agent::EventKind>{
        agent::EventKind::TaskStarted,
        agent::EventKind::ContextPreparationStarted,
        agent::EventKind::ContextPrepared,
        agent::EventKind::ModelCallStarted,
        agent::EventKind::ModelCallSucceeded,
        agent::EventKind::TaskCompleted});
}

TEST_CASE(event_store_failure_does_not_apply_event) {
    test::FailingEventStore events(4);
    auto result = fixtures::engine_with(events).run(fixtures::run_request("fix warning"));
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::PersistenceFailure);
    REQUIRE(result.state->last_sequence == 3);
}
```

Add KnowledgeProvider and ModelClient failure tests that require `ContextPreparationFailed -> TaskFailed` and `ModelCallFailed -> TaskFailed`.

The test file owns an `EngineFixture` containing Fake Model, Tool, Knowledge, EventStore, Clock, IDs, Cancellation, and RuntimeEngine in declaration order. `fixtures::engine_with` returns that owning fixture; it must not return an engine holding references to destroyed temporaries.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target runtime_engine_tests`.

Expected: compilation fails because `application/runtime_engine.h` is absent.

- [ ] **Step 3: Implement append-then-reduce and the text path**

Use this public API:

```cpp
struct RunRequest {
    std::string issue;
    std::string workspace_utf8;
    std::string system_prompt;
    RuntimeBudgets budgets;
};
struct RuntimeResult {
    std::optional<TaskState> state;
    std::optional<RuntimeError> fatal_error;
};
class RuntimeEngine {
public:
    RuntimeEngine(ModelClient&, ToolGateway&, KnowledgeProvider&, EventStore&,
                  Clock&, IdGenerator&, Cancellation&);
    RuntimeResult run(const RunRequest& request);
};
```

Implement one private operation that constructs the next sequence, appends the event, and only then calls `reduce_event`. On append failure, return the last durable state plus fatal `PersistenceFailure`; do not attempt to record another event.

For `ModelClient` or `KnowledgeProvider` failures, append the specific failure event and then `TaskFailed`. For a response with no tool calls, concatenate TextBlocks in order; an empty final response becomes `ProtocolFailure`, not a successful empty answer.

- [ ] **Step 4: Run engine and full tests**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_engine_tests
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: engine event-order tests and all earlier tests pass.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/application/runtime_engine.* tests/application/runtime_engine_test.cpp
git commit -m "feat: orchestrate durable model turns"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 6: Add ordered tool turns, budgets, cancellation, and terminal guards

**Files:**
- Modify: `src/application/runtime_engine.cpp`
- Modify: `src/application/runtime_engine.h`
- Modify: `tests/application/runtime_engine_test.cpp`

**Interfaces:**
- Consumes: `ToolGateway::definitions/execute`, Clock monotonic time, Cancellation, and TaskState pending tool fields.
- Produces: ordered tool execution, one collected tool-result user message per model response, and deterministic budget/cancellation termination.

- [ ] **Step 1: Add failing tests for tool and stop behavior**

Add exact cases for two tools, a returned error result, ToolGateway infrastructure failure, model-round limit, tool-call limit, wall-time limit, cancellation, and no external call after terminal state:

```cpp
TEST_CASE(engine_executes_multiple_tools_in_response_order) {
    test::FakeModel model({
        fixtures::tool_response({fixtures::call("call-1", "read"),
                                 fixtures::call("call-2", "search")}),
        fixtures::text_response("done")});
    test::FakeTools tools({
        agent::ToolResult{"call-1", "contents", false},
        agent::ToolResult{"call-2", "matches", false}});
    auto fixture = fixtures::engine_with(model, tools);
    auto result = fixture.engine.run(fixtures::run_request("inspect code"));
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(tools.executed_ids() == std::vector<std::string>{"call-1", "call-2"});
    REQUIRE(model.requests().at(1).messages.back().content.size() == 2);
}

TEST_CASE(tool_error_result_is_not_gateway_failure) {
    test::FakeTools tools({agent::ToolResult{"call-1", "compiler failed", true}});
    auto fixture = fixtures::two_turn_engine(tools);
    auto result = fixture.engine.run(fixtures::run_request("build"));
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallFailed) == 0);
    REQUIRE(fixture.events.count(agent::EventKind::ToolCallSucceeded) == 1);
}
```

- [ ] **Step 2: Run tests to verify RED**

Run `cmake --build build/vs2022 --config Debug --target runtime_engine_tests` followed by the test executable.

Expected: ordered tool and budget cases fail against the text-only engine.

- [ ] **Step 3: Implement the exact guard order and tool loop**

Before each external call check in this order: cancellation, wall time, then the call-specific count. When a guard fires, append exactly one terminal event and return without making the call.

For tool responses:

1. preserve `ToolUseBlock` order;
2. before each call append `ToolCallStarted` and increment usage through the reducer;
3. on `Result<ToolResult>::success`, append `ToolCallSucceeded` even when `is_error == true`;
4. on gateway `Result` failure, append `ToolCallFailed`, append `TaskFailed`, and stop;
5. after all results, append `ContextPreparationStarted`; the reducer flushes all pending results into one user message;
6. retrieve context and continue to the next model round.

Do not add threads, futures, retries, sleeps, background queues, or implicit recursion.

- [ ] **Step 4: Run all engine cases and full CTest**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_engine_tests
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: tool order, error result, infrastructure failure, all budgets, cancellation, and terminal guard cases pass.

- [ ] **Step 5: Commit and back up**

```powershell
git add src/application/runtime_engine.* tests/application/runtime_engine_test.cpp
git commit -m "feat: enforce runtime tool and stop budgets"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 7: Implement the offline-tested Anthropic-compatible Messages Adapter

**Files:**
- Create: `src/adapters/anthropic/http_transport.h`
- Create: `src/adapters/anthropic/cpr_http_transport.h`
- Create: `src/adapters/anthropic/cpr_http_transport.cpp`
- Create: `src/adapters/anthropic/anthropic_messages_client.h`
- Create: `src/adapters/anthropic/anthropic_messages_client.cpp`
- Create: `tests/adapters/anthropic_messages_client_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ModelClient`, model Domain types, and Value JSON codec.
- Produces: `HttpTransport`, `CprHttpTransport`, `AnthropicConfig`, and `AnthropicMessagesClient`.

- [ ] **Step 1: Write failing offline adapter tests**

Use a Fake transport that captures `HttpRequest` and returns scripted `HttpResponse` values. Verify exact endpoint, one authentication scheme, request body, ordered content blocks, and errors:

```cpp
TEST_CASE(anthropic_adapter_maps_tool_request_and_response) {
    test::FakeHttpTransport http(fixtures::anthropic_tool_response());
    agent::AnthropicConfig config{
        "https://provider.example", "model-id",
        agent::CredentialKind::ApiKey, "TEST_SECRET", "2023-06-01", 4096};
    agent::AnthropicMessagesClient client(config, http);
    auto result = client.complete(fixtures::model_request_with_tool());
    REQUIRE(result.has_value());
    REQUIRE(http.last_request().url == "https://provider.example/v1/messages");
    REQUIRE(http.last_request().headers.at("x-api-key") == "TEST_SECRET");
    REQUIRE(http.last_request().headers.count("authorization") == 0);
    REQUIRE(result.value().stop_reason == agent::StopReason::ToolUse);
    REQUIRE(std::get<agent::ToolUseBlock>(result.value().content.at(1)).call.id == "call-1");
}

TEST_CASE(anthropic_error_never_contains_secret_or_unbounded_body) {
    test::FakeHttpTransport http({500, "REQUEST_SECRET very large body", {}});
    auto result = fixtures::anthropic_client("REQUEST_SECRET", http)
        .complete(fixtures::simple_model_request());
    REQUIRE(!result.has_value());
    REQUIRE(result.error().message.find("REQUEST_SECRET") == std::string::npos);
    REQUIRE(result.error().message.find("very large body") == std::string::npos);
}
```

Also cover Bearer authentication, text plus tool block order, outgoing `tool_result`, max-token stop, timeout, non-2xx, empty body, invalid JSON, missing content, and unknown block type.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target anthropic_adapter_tests`.

Expected: compilation fails because the adapter headers are absent.

- [ ] **Step 3: Implement the HTTP seam and protocol mapping**

Use these transport records:

```cpp
struct HttpRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::int64_t timeout_ms;
};
struct HttpResponse {
    int status;
    std::string body;
    std::map<std::string, std::string> headers;
};
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual Result<HttpResponse> post(const HttpRequest& request) = 0;
};
```

`AnthropicConfig` must require nonempty base URL/model/credential, exactly one `CredentialKind`, API version `2023-06-01`, and positive max tokens. Strip one trailing slash from base URL before adding `/v1/messages`.

Map internal TextBlock, ToolUseBlock, and ToolResultBlock explicitly. Reject unknown response block types and responses with no usable content. Map `end_turn`, `tool_use`, `max_tokens`, and `stop_sequence`; preserve unknown stop text as `StopReason::Unknown` plus its raw name in a non-secret field.

`CprHttpTransport` maps cpr network errors to `TransportFailure`, timeout to `RequestTimeout`, and successful transport to `HttpResponse`. No adapter error may include request headers, credential values, or raw response body.

- [ ] **Step 4: Run adapter and full offline tests**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target anthropic_adapter_tests
ctest --test-dir build/vs2022 -C Debug --output-on-failure
```

Expected: all tests pass without network access.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/adapters/anthropic tests/adapters/anthropic_messages_client_test.cpp
git commit -m "feat: add anthropic messages adapter"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 8: Add configuration, empty adapters, system adapters, and CLI composition

**Files:**
- Create: `src/adapters/empty/empty_tool_gateway.h`
- Create: `src/adapters/empty/empty_tool_gateway.cpp`
- Create: `src/adapters/empty/empty_knowledge_provider.h`
- Create: `src/adapters/empty/empty_knowledge_provider.cpp`
- Create: `src/adapters/system/system_clock.h`
- Create: `src/adapters/system/system_clock.cpp`
- Create: `src/adapters/system/random_id_generator.h`
- Create: `src/adapters/system/random_id_generator.cpp`
- Create: `src/adapters/system/signal_cancellation.h`
- Create: `src/adapters/system/signal_cancellation.cpp`
- Create: `src/config/runtime_config.h`
- Create: `src/config/runtime_config.cpp`
- Create: `src/cli/cli_app.h`
- Create: `src/cli/cli_app.cpp`
- Create: `src/main.cpp`
- Create: `tests/cli/cli_app_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `RuntimeEngine`, Anthropic/JSONL adapters, dotenv header, and system Ports.
- Produces: the `agent` executable with `run` and `verify-log`, stable exit codes, and safe configuration loading.

- [ ] **Step 1: Write failing CLI and configuration tests**

Test argument validation, command dispatch, injected Fake runner/verifier, config numeric bounds, exactly one auth mode, and exit codes:

```cpp
TEST_CASE(cli_maps_completed_task_to_zero) {
    test::FakeRunCommand run(fixtures::completed_result("done"));
    test::FakeVerifyCommand verify;
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(run, verify, out, err);
    int code = app.execute({"agent", "run", "--workspace", "E:/repo",
                            "--issue", "fix warning"});
    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(out.str().find("done") != std::string::npos);
}

TEST_CASE(config_rejects_two_authentication_modes) {
    test::MapEnvironment env{{"AGENT_BASE_URL", "https://provider.example"},
                             {"AGENT_MODEL", "model-id"},
                             {"AGENT_API_KEY", "key"},
                             {"AGENT_AUTH_TOKEN", "token"}};
    auto config = agent::load_runtime_config(env);
    REQUIRE(!config.has_value());
    REQUIRE(config.error().code == agent::ErrorCode::InvalidConfiguration);
}
```

Fix these exit values in tests: `0 Success`, `2 InvalidInputOrConfig`, `3 TaskFailed`, `4 BudgetExceeded`, `5 Cancelled`, `6 PersistenceFailure`, `7 InvalidEventLog`.

- [ ] **Step 2: Build to verify RED**

Run `cmake --build build/vs2022 --config Debug --target cli_tests`.

Expected: compilation fails because CLI/config headers are absent.

- [ ] **Step 3: Implement configuration and system adapters**

Read these names through an injected Environment interface: `AGENT_BASE_URL`, `AGENT_MODEL`, exactly one of `AGENT_API_KEY`/`AGENT_AUTH_TOKEN`, `AGENT_MAX_TOKENS`, `AGENT_MAX_MODEL_ROUNDS`, `AGENT_MAX_TOOL_CALLS`, `AGENT_MAX_TASK_SECONDS`, `AGENT_MODEL_TIMEOUT_SECONDS`, `AGENT_RUNTIME_ROOT`, and `AGENT_SYSTEM_PROMPT`.

Defaults are `4096`, `16`, `64`, `1800`, `120`, `runtime_data`, and `You are a coding agent runtime. Use only tools explicitly provided.` Reject zero, negative, overflow, malformed integers, missing URL/model/auth, and simultaneous auth values.

Allow `--env-file <absolute-or-relative-path>`; parse arguments before loading it, resolve the supplied path explicitly, and never search arbitrary workspace parents for `.env`. The CLI accepts the path, not a secret.

`SystemClock` returns UTC with millisecond precision and monotonic milliseconds. `RandomIdGenerator` returns `task-` or `corr-` plus 32 lowercase hexadecimal characters. `SignalCancellation` uses a process-wide `std::atomic_bool` set by SIGINT.

- [ ] **Step 4: Implement CLI injection and Composition Root**

Use callable seams so CLI tests do not access the network:

```cpp
using RunCommand = std::function<RuntimeResult(const RunRequest&)>;
using VerifyCommand = std::function<Result<TaskState>(const std::filesystem::path&)>;
enum ExitCode : int {
    Success = 0, InvalidInputOrConfig = 2, TaskFailed = 3,
    BudgetExceeded = 4, Cancelled = 5,
    PersistenceFailure = 6, InvalidEventLog = 7
};
class CliApp {
public:
    CliApp(RunCommand, VerifyCommand, std::ostream&, std::ostream&);
    int execute(const std::vector<std::string>& args);
};
```

`main.cpp` parses the optional env file, loads config, creates concrete adapters, creates RuntimeEngine, then calls CliApp. It must contain no reducer, protocol, or JSONL logic. `verify-log` calls `EventStore::read_file` and `replay_events`, prints task ID/status/last sequence, and returns 7 on validation failure.

- [ ] **Step 5: Run CLI tests and an offline executable check**

Run:

```powershell
cmake --build build/vs2022 --config Debug --target cli_tests agent
ctest --test-dir build/vs2022 -C Debug --output-on-failure
& .\build\vs2022\Debug\agent.exe run --workspace . --issue "test"
```

Expected: all tests pass; the final command exits `2` with a clear missing-configuration message when credentials are absent, without printing any credential values.

- [ ] **Step 6: Commit and back up**

```powershell
git add CMakeLists.txt src/adapters/empty src/adapters/system src/config src/cli src/main.cpp tests/cli/cli_app_test.cpp
git commit -m "feat: compose runtime kernel cli"
$branch = git branch --show-current
git push backup $branch
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ((git rev-parse HEAD) -ne $remote) { throw "backup commit mismatch" }
```

---

### Task 9: Close integration, Unicode, live-smoke, documentation, and security acceptance

**Files:**
- Create: `.env.example`
- Create: `README.md`
- Create: `tests/integration/runtime_integration_test.cpp`
- Create: `tests/live/anthropic_live_smoke.cpp`
- Modify: `CMakeLists.txt`
- Modify: files under `src/` only when a failing acceptance test proves a defect

**Interfaces:**
- Consumes: the complete Runtime Kernel.
- Produces: clean-environment build evidence, Fake end-to-end replay, optional real-model smoke test, user documentation, and final security checks.

- [ ] **Step 1: Write the failing Fake end-to-end acceptance test**

Use real `RuntimeEngine`, `JsonlEventStore`, Empty adapters except a scripted Fake Model, and a Unicode runtime root:

```cpp
TEST_CASE(fake_end_to_end_writes_and_replays_unicode_task) {
    test::ScopedTempDir temp("运行时-回放");
    agent::JsonlEventStore store(temp.path());
    test::FakeModel model({fixtures::text_response("已完成")});
    auto runtime = fixtures::real_runtime_with(model, store);
    auto result = runtime.run(fixtures::run_request("修复 C++ 警告", temp.path()));
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    auto loaded = store.read_file(store.event_path(result.state->task_id));
    REQUIRE(loaded.has_value());
    auto replayed = agent::replay_events(loaded.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == *result.state);
}
```

Add a secret sentinel to Fake HTTP and assert it is absent from captured CLI output, event files, and error messages.

The integration test file owns `real_runtime_with(ModelClient&, JsonlEventStore&)`; the returned fixture owns EmptyToolGateway, EmptyKnowledgeProvider, deterministic Fakes for time/IDs/cancellation, and RuntimeEngine so no Port reference outlives its object.

- [ ] **Step 2: Run acceptance tests to verify RED**

Run `cmake --build build/vs2022 --config Debug --target runtime_integration_tests`.

Expected: build or assertions fail until all composition and Unicode path details are connected.

- [ ] **Step 3: Add the opt-in live test and documentation**

Add `option(AGENT_ENABLE_LIVE_TESTS "Enable credentialed provider smoke test" OFF)`. Only when ON, build/register `anthropic_live_smoke`; the test must load environment configuration, send one no-tool prompt, require a nonempty text response, and print only status/request ID.

`.env.example` contains empty values and safe defaults only:

```dotenv
AGENT_BASE_URL=
AGENT_MODEL=
AGENT_API_KEY=
AGENT_AUTH_TOKEN=
AGENT_MAX_TOKENS=4096
AGENT_MAX_MODEL_ROUNDS=16
AGENT_MAX_TOOL_CALLS=64
AGENT_MAX_TASK_SECONDS=1800
AGENT_MODEL_TIMEOUT_SECONDS=120
AGENT_RUNTIME_ROOT=runtime_data
```

README must document architecture boundaries, non-goals, Visual Studio build commands, `run`, `verify-log`, event privacy, `.env` handling, the local `backup` remote, and the difference between offline CTest and the opt-in live smoke.

- [ ] **Step 4: Make the acceptance test GREEN without expanding scope**

Fix only defects observed by Task 9 tests. Do not add real tools, RAG, recovery, retry, parallelism, or a second Provider.

Run:

```powershell
cmake --build build/vs2022 --config Debug --target runtime_integration_tests
ctest --test-dir build/vs2022 -C Debug -R runtime_integration_tests --output-on-failure
```

Expected: Unicode end-to-end and secret-sentinel cases pass.

- [ ] **Step 5: Perform a fresh full build and offline verification**

Run from a new build directory:

```powershell
cmake -S . -B build/final-vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build/final-vs2022 --config Debug --parallel
ctest --test-dir build/final-vs2022 -C Debug --output-on-failure
& .\build\final-vs2022\Debug\agent.exe run --workspace . --issue "test"
git status --short
```

Expected: configure/build succeed; every offline CTest passes; credential-free CLI exits 2 with a safe config message; Git shows only Task 9 intended files before Commit.

- [ ] **Step 6: Run or accurately skip the real-provider smoke test**

When valid environment credentials are available:

```powershell
cmake -S . -B build/live-vs2022 -G "Visual Studio 17 2022" -A x64 -DAGENT_ENABLE_LIVE_TESTS=ON
cmake --build build/live-vs2022 --config Debug --target anthropic_live_smoke
ctest --test-dir build/live-vs2022 -C Debug -R anthropic_live_smoke --output-on-failure
```

Expected with credentials: one live smoke passes. Without credentials: do not run these commands; report `live smoke not run: credentials unavailable`, never `pass`.

- [ ] **Step 7: Commit, back up, and compare Commit IDs**

```powershell
git add .env.example README.md CMakeLists.txt src tests
git commit -m "test: verify runtime kernel end to end"
$branch = git branch --show-current
git push backup $branch
$local = git rev-parse HEAD
$remote = ((git ls-remote backup "refs/heads/$branch") -split "\s+")[0]
if ($local -ne $remote) { throw "backup commit mismatch" }
git status --short --branch
```

Expected: local and backup IDs match and the implementation worktree is clean.

## Spec Coverage Matrix

| Spec section | Implemented and verified by |
|---|---|
| Scope, decisions, goals, non-goals | Global Constraints; Tasks 1-9 scope guards |
| Domain and dependency direction | Tasks 1-3 |
| Port and Adapter boundaries | Tasks 4, 7, 8 |
| Explicit state machine | Tasks 2-3 |
| Typed event model and append-then-reduce | Tasks 2-5 |
| JSONL storage and replay validation | Task 4; Task 9 integration replay |
| Model/context/tool runtime flow | Tasks 5-6 |
| Ordered multiple tools | Task 6 |
| Budgets, cancellation, and terminal guards | Task 6 |
| Configuration and credential boundaries | Tasks 7-9 |
| Structured error mapping | Tasks 1, 4-8 |
| CLI `run` and `verify-log` | Task 8; Task 9 integration |
| Offline adapter, domain, engine, persistence, and CLI tests | Tasks 1-8 |
| Unicode, live smoke, documentation, final acceptance | Task 9 |

## Final Review Checklist

- [ ] Every spec section maps to at least one task above.
- [ ] Domain headers contain no cpr, dotenv, nlohmann/json, CLI, or file-stream includes.
- [ ] All external calls have started/succeeded/failed event coverage, including `ContextPreparationFailed`.
- [ ] Every state change uses append-then-reduce.
- [ ] JSONL replay rejects malformed sequence/schema/state without repairing files.
- [ ] Multiple tools are sequential and preserve response order.
- [ ] `ToolResult{is_error=true}` remains a successful gateway return.
- [ ] All budgets and cancellation stop before another external call.
- [ ] Offline tests never access the network.
- [ ] Live smoke is explicitly enabled and never faked as passed.
- [ ] Secrets are absent from tracked files, events, logs, errors, and replies.
- [ ] Full MSVC build and CTest evidence is fresh.
- [ ] Each task Commit exists on the feature branch and is mirrored to `backup`.
