# Safe Workspace File Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the existing single C++17 Coding Agent five bounded workspace file tools that support a real inspect-edit-verify loop without adding process execution, Git, RAG, or multi-agent behavior.

**Architecture:** `RuntimeEngine` passes durable `TaskState.workspace_utf8` through a small `ToolExecutionContext`. A concrete `WorkspaceToolGateway` validates `Value` arguments, delegates path/text/file work to focused adapter helpers, and serializes bounded JSON `ToolResult` values. The MVP uses `std::filesystem` plus a small Windows/POSIX atomic-replace helper; it rejects links and protected paths but deliberately does not build a hostile-concurrency filesystem framework.

**Tech Stack:** C++17, CMake 3.21+, MSVC 2022, `std::filesystem`, nlohmann/json 3.12.0, Win32 `GetFileAttributesW`/`MoveFileExW`, POSIX `rename`, existing custom C++ test harness and CTest.

**Spec:** `docs/superpowers/specs/2026-08-21-safe-workspace-file-tools-design.md`

## Global Constraints

- Implement only `list_files`, `read_file`, `search_text`, `replace_text`, and `write_file`.
- Do not add shell/process execution, Git operations, directory creation, delete/move tools, RAG, MCP, plugins, sub-agents, or multi-agent behavior.
- Tool paths are `/`-separated workspace-relative UTF-8 paths; reject absolute/drive/UNC/backslash/NUL/empty/`.`/`..` components.
- Reject symlink, Windows reparse point, non-regular file, and `hard_link_count != 1` at the operation boundary.
- Protect `.git`, `.worktrees`, secret env/credential leaves, `runtime_data`, and an in-workspace configured runtime root.
- Limit text files to 1 MiB, result JSON to 64 KiB, result count to 200, traversal to 2000 entries/500 files, and search bytes to 16 MiB.
- Expected tool errors return `Result<ToolResult>::success` with `is_error=true`; only an untrustworthy adapter outcome returns outer `failure`.
- Keep the existing state machine, event kinds, direct `ToolCallFailed -> Failed` behavior, and credential-free `verify-log` path unchanged.
- Every production change begins with a focused failing test, then minimal implementation, focused GREEN, full CTest, review, commit, push to local `backup`, and SHA comparison.
- Default tests stay offline. Do not call a real Provider or GitHub.

## File Structure

- `src/ports/tool_gateway.h`: provider-neutral workspace execution context.
- `src/adapters/workspace/workspace_types.h`: internal result, fault, path, listing, snapshot, search, and write records.
- `src/adapters/workspace/workspace_text.h/.cpp`: strict UTF-8, SHA-256, line paging, ASCII-insensitive literal matching.
- `src/adapters/workspace/workspace_path_policy.h/.cpp`: relative path grammar, protection rules, containment, link/reparse/hard-link checks.
- `src/adapters/workspace/workspace_file_ops.h/.cpp`: bounded list/read/search and temp-file replacement.
- `src/adapters/workspace/workspace_tool_gateway.h/.cpp`: five schemas, argument parsing, routing, and deterministic JSON envelopes.
- `tests/adapters/workspace_primitives_test.cpp`: text/hash/path policy boundaries.
- `tests/adapters/workspace_tool_gateway_test.cpp`: public tool contract and real temporary filesystem behavior.
- Existing Runtime/CLI/integration files change only where required to pass workspace context and compose the gateway.

---

### Task 1: Pass the durable workspace through ToolGateway

**Files:**
- Modify: `src/ports/tool_gateway.h:8-15`
- Modify: `src/application/runtime_engine.cpp:321`
- Modify: `src/adapters/empty/empty_tool_gateway.h:7-11`
- Modify: `src/adapters/empty/empty_tool_gateway.cpp:9-13`
- Modify: `tests/application/runtime_engine_test.cpp:55-103`
- Modify: `tests/cli/cli_app_test.cpp:925-935`

**Interfaces:**
- Produces: `agent::ToolExecutionContext { std::string workspace_utf8; }`.
- Produces: `ToolGateway::execute(const ToolCall&, const ToolExecutionContext&)`.
- Preserves: `definitions()` and all Runtime event/state behavior.

- [ ] **Step 1: Write the failing Runtime test**

Extend `FakeTools` to record contexts and add this assertion to a successful tool-loop test:

```cpp
agent::Result<agent::ToolResult> execute(
    const agent::ToolCall& call,
    const agent::ToolExecutionContext& context) override {
    executed_calls.push_back(call);
    executed_contexts.push_back(context);
    return results_.at(next_result_++);
}

REQUIRE(tools.executed_contexts.size() == 1);
REQUIRE(tools.executed_contexts.front().workspace_utf8 ==
        u8"E:/工作区/项目");
```

Update the request in that test to use the exact Unicode workspace above.

- [ ] **Step 2: Run the focused RED**

Run:

```powershell
cmake --build build/baseline-vs2022 --config Debug --target runtime_engine_tests
```

Expected: compile failure because `ToolExecutionContext` and the two-argument `execute` do not exist.

- [ ] **Step 3: Add the minimal Port and propagation**

```cpp
struct ToolExecutionContext {
    std::string workspace_utf8;
};

virtual Result<ToolResult> execute(
    const ToolCall& call,
    const ToolExecutionContext& context) = 0;
```

Change the Runtime call to:

```cpp
auto tool_result = tools_.execute(
    call, ToolExecutionContext{state->workspace_utf8});
```

Update EmptyToolGateway and its direct CLI test to pass `{u8"E:/工作区"}`. Do not change any event payload.

- [ ] **Step 4: Run focused and full GREEN**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target runtime_engine_tests cli_tests runtime_integration_tests
& .\build\baseline-vs2022\Debug\runtime_engine_tests.exe
& .\build\baseline-vs2022\Debug\cli_tests.exe
& .\build\baseline-vs2022\Debug\runtime_integration_tests.exe
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
```

Expected: all focused executables and CTest pass; existing event counts remain unchanged.

- [ ] **Step 5: Review, commit, and back up**

```powershell
git diff --check
git add src/ports/tool_gateway.h src/application/runtime_engine.cpp src/adapters/empty/empty_tool_gateway.h src/adapters/empty/empty_tool_gateway.cpp tests/application/runtime_engine_test.cpp tests/cli/cli_app_test.cpp
git commit -m "feat: pass workspace context to tools"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
if ($local -ne $backup) { throw 'backup SHA mismatch' }
```

### Task 2: Add bounded text, hash, and path-policy primitives

**Files:**
- Create: `src/adapters/workspace/workspace_types.h`
- Create: `src/adapters/workspace/workspace_text.h`
- Create: `src/adapters/workspace/workspace_text.cpp`
- Create: `src/adapters/workspace/workspace_path_policy.h`
- Create: `src/adapters/workspace/workspace_path_policy.cpp`
- Create: `tests/adapters/workspace_primitives_test.cpp`
- Modify: `CMakeLists.txt:53-104`

**Interfaces:**
- Produces: `workspace::FaultCode`, `workspace::Fault`, and `workspace::Outcome<T>`.
- Produces: `workspace::RelativePath` and `WorkspacePathPolicy::parse/resolve_existing/resolve_parent`.
- Produces: `is_strict_utf8_text`, `sha256_hex`, `line_page`, and `literal_matches`.

- [ ] **Step 1: Write primitive tests before headers exist**

Create tests with exact known vectors and rejected forms:

```cpp
TEST_CASE(workspace_sha256_and_utf8_are_deterministic) {
    REQUIRE(agent::workspace::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");
    REQUIRE(agent::workspace::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(agent::workspace::is_strict_utf8_text(u8"中文🙂\n"));
    REQUIRE(!agent::workspace::is_strict_utf8_text(std::string("a\0b", 3)));
    REQUIRE(!agent::workspace::is_strict_utf8_text("\xC0\xAF"));
}

TEST_CASE(workspace_relative_paths_reject_escape_and_windows_aliases) {
    agent::workspace::WorkspacePathPolicy policy("runtime_data");
    for (const auto& path : {"", "../x", "a/../x", "/x", "C:/x",
                             "a\\b", "a//b", "NUL.txt", "a:stream"}) {
        REQUIRE(std::holds_alternative<agent::workspace::Fault>(
            policy.parse(path)));
    }
    const auto valid = policy.parse(u8"源/文件.cpp");
    REQUIRE(std::holds_alternative<agent::workspace::RelativePath>(valid));
    REQUIRE(std::get<agent::workspace::RelativePath>(valid).generic ==
            u8"源/文件.cpp");
}
```

Add tests for `.git`, `.env`, `.env.local`, `.worktrees`, `runtime_data`, configured in-workspace runtime root, `.env.example`, line paging, and ASCII-insensitive non-overlapping matches.

- [ ] **Step 2: Run the focused RED**

Add only the CMake test target, then run:

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_primitives_tests
```

Expected: compile failure on missing workspace headers/functions.

- [ ] **Step 3: Implement the internal types and pure helpers**

Use these exact core declarations:

```cpp
namespace agent::workspace {

enum class FaultCode {
    InvalidArguments, AccessDenied, NotFound, Conflict,
    UnsupportedFile, LimitExceeded, IoError
};

struct Fault {
    FaultCode code;
    std::string message;
    bool retryable{false};
};

template <typename T>
using Outcome = std::variant<T, Fault>;

struct RelativePath {
    std::string generic;
    std::vector<std::string> components;
};

struct LinePage {
    std::size_t start_line;
    std::size_t end_line;
    std::size_t total_lines;
    std::string content;
    std::optional<std::size_t> next_start_line;
};

bool is_strict_utf8_text(std::string_view text) noexcept;
std::string sha256_hex(std::string_view bytes);
Outcome<LinePage> line_page(std::string_view text,
                            std::size_t start_line,
                            std::size_t max_lines,
                            std::size_t max_content_bytes);
std::vector<std::size_t> literal_matches(std::string_view line,
                                         std::string_view query,
                                         bool case_sensitive);
}
```

`WorkspacePathPolicy` stores only `runtime_root_`, validates syntax without I/O in `parse`, and exposes:

```cpp
Outcome<std::filesystem::path> resolve_existing(
    const std::filesystem::path& workspace,
    const RelativePath& relative,
    bool require_directory) const;
Outcome<std::filesystem::path> resolve_parent(
    const std::filesystem::path& workspace,
    const RelativePath& relative) const;
```

The implementation must use fixed `Fault` messages, `weakly_canonical`/`absolute` containment, every-component `symlink_status`, Windows `GetFileAttributesW` reparse rejection, and `hard_link_count == 1` for file leaves. SHA-256 is a private portable implementation in `workspace_text.cpp`, tested only through `sha256_hex`.

- [ ] **Step 4: Run focused GREEN and boundary review**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_primitives_tests
& .\build\baseline-vs2022\Debug\workspace_primitives_tests.exe
git diff --check
```

Expected: all primitive tests pass; no absolute temp path appears in returned `Fault.message` assertions.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/adapters/workspace/workspace_types.h src/adapters/workspace/workspace_text.h src/adapters/workspace/workspace_text.cpp src/adapters/workspace/workspace_path_policy.h src/adapters/workspace/workspace_path_policy.cpp tests/adapters/workspace_primitives_test.cpp
git commit -m "feat: add workspace path and text guards"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
if ($local -ne $backup) { throw 'backup SHA mismatch' }
```

### Task 3: Implement tool definitions, list_files, and read_file

**Files:**
- Create: `src/adapters/workspace/workspace_file_ops.h`
- Create: `src/adapters/workspace/workspace_file_ops.cpp`
- Create: `src/adapters/workspace/workspace_tool_gateway.h`
- Create: `src/adapters/workspace/workspace_tool_gateway.cpp`
- Create: `tests/adapters/workspace_tool_gateway_test.cpp`
- Modify: `CMakeLists.txt:53-104`

**Interfaces:**
- Consumes: Task 2 path/text primitives.
- Produces: `WorkspaceFileOps::list/read` and `WorkspaceToolGateway`.
- Produces: five static `ToolDefinition` values even though only list/read behavior becomes GREEN in this task.

- [ ] **Step 1: Write definition and read-only behavior tests**

Add exact test-call builders so every fixture has a stable unique ID:

```cpp
agent::ToolCall list_call(std::string id,
                          std::string path,
                          bool recursive = false,
                          std::int64_t max_results = 100) {
    return {std::move(id), "list_files",
            agent::Value::object({
                {"path", std::move(path)},
                {"recursive", recursive},
                {"max_results", max_results}})};
}

agent::ToolCall read_call(std::string id,
                          std::string path,
                          std::int64_t start_line = 1,
                          std::int64_t max_lines = 200) {
    return {std::move(id), "read_file",
            agent::Value::object({
                {"path", std::move(path)},
                {"start_line", start_line},
                {"max_lines", max_lines}})};
}
```

```cpp
TEST_CASE(workspace_gateway_exposes_exact_five_definitions) {
    agent::WorkspaceToolGateway gateway("runtime_data");
    const auto definitions = gateway.definitions();
    REQUIRE(definitions.size() == 5);
    REQUIRE(definitions.at(0).name == "list_files");
    REQUIRE(definitions.at(1).name == "read_file");
    REQUIRE(definitions.at(2).name == "search_text");
    REQUIRE(definitions.at(3).name == "replace_text");
    REQUIRE(definitions.at(4).name == "write_file");
    REQUIRE(definitions.at(0).input_schema.at("additionalProperties") ==
            agent::Value(false));
}

TEST_CASE(workspace_list_and_read_return_relative_bounded_json) {
    test::ScopedTempDir temp(u8"workspace-tools");
    temp.write_text(u8"源/a.cpp", u8"一\n二\n三\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto listed = gateway.execute(list_call("call-list", "."), context);
    const auto read = gateway.execute(
        read_call("call-read", u8"源/a.cpp", 2, 1), context);
    REQUIRE(listed.has_value() && !listed.value().is_error);
    REQUIRE(read.has_value() && !read.value().is_error);
    REQUIRE(read.value().content.find(u8"二\n") != std::string::npos);
    REQUIRE(read.value().content.find(temp.path().generic_u8string()) ==
            std::string::npos);
}
```

Add exact tests for unknown/missing/extra/wrong-type keys, stable sort, protected omission, empty file, CRLF, paging, 1 MiB and 64 KiB boundaries, direct symlink/reparse and hard-link refusal. A Windows symlink creation privilege failure prints a named SKIP line and does not mark the attack path PASS.

- [ ] **Step 2: Run the focused RED**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests
```

Expected: compile failure because `WorkspaceToolGateway` does not exist.

- [ ] **Step 3: Implement read-only file operations and gateway routing**

Use these records and methods:

```cpp
struct WorkspaceEntry {
    std::string path;
    bool directory;
    std::uintmax_t size_bytes;
};
struct ListOutput {
    std::vector<WorkspaceEntry> entries;
    bool truncated{false};
    std::size_t omitted_entries{0};
};
struct ReadOutput {
    std::string path;
    std::string sha256;
    LinePage page;
};

class WorkspaceFileOps {
public:
    explicit WorkspaceFileOps(std::filesystem::path runtime_root);
    Outcome<ListOutput> list(const std::filesystem::path& workspace,
                             const RelativePath& path,
                             bool recursive,
                             std::size_t max_results) const;
    Outcome<ReadOutput> read(const std::filesystem::path& workspace,
                             const RelativePath& path,
                             std::size_t start_line,
                             std::size_t max_lines) const;
};
```

`WorkspaceToolGateway::execute` validates arguments, dispatches exact tool names, maps `Fault` to `{"error":...}` with `is_error=true`, and catches only unknown internal exceptions as outer `PersistenceFailure`. Serialize with `nlohmann::json::dump()` and refuse any content above 65536 bytes.

- [ ] **Step 4: Run focused and existing-suite GREEN**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests runtime_engine_tests
& .\build\baseline-vs2022\Debug\workspace_tool_gateway_tests.exe
& .\build\baseline-vs2022\Debug\runtime_engine_tests.exe
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
```

Expected: tool tests and the full existing suite pass without network.

- [ ] **Step 5: Commit and back up**

```powershell
git add CMakeLists.txt src/adapters/workspace/workspace_file_ops.h src/adapters/workspace/workspace_file_ops.cpp src/adapters/workspace/workspace_tool_gateway.h src/adapters/workspace/workspace_tool_gateway.cpp tests/adapters/workspace_tool_gateway_test.cpp
git commit -m "feat: add bounded workspace read tools"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
if ($local -ne $backup) { throw 'backup SHA mismatch' }
```

### Task 4: Implement deterministic literal search_text

**Files:**
- Modify: `src/adapters/workspace/workspace_types.h`
- Modify: `src/adapters/workspace/workspace_file_ops.h`
- Modify: `src/adapters/workspace/workspace_file_ops.cpp`
- Modify: `src/adapters/workspace/workspace_tool_gateway.cpp`
- Modify: `tests/adapters/workspace_tool_gateway_test.cpp`

**Interfaces:**
- Consumes: `literal_matches`, protected traversal, strict UTF-8 reader.
- Produces: `WorkspaceFileOps::search(...) -> Outcome<SearchOutput>`.

- [ ] **Step 1: Add failing literal-search tests**

Add this builder beside the read-only helpers:

```cpp
agent::ToolCall search_call(std::string id,
                            std::string path,
                            std::string query,
                            bool case_sensitive = true,
                            std::int64_t max_results = 100) {
    return {std::move(id), "search_text",
            agent::Value::object({
                {"path", std::move(path)},
                {"query", std::move(query)},
                {"case_sensitive", case_sensitive},
                {"max_results", max_results}})};
}
```

```cpp
TEST_CASE(workspace_search_is_literal_stable_and_explicitly_truncated) {
    test::ScopedTempDir temp("workspace-search");
    temp.write_text("b.cpp", "ToolGateway ToolGateway\n");
    temp.write_text("a.cpp", "toolgateway [x]+\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto result = gateway.execute(
        search_call("call-search", ".", "ToolGateway", true, 2), context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(result.value().content.find("a.cpp") == std::string::npos);
    REQUIRE(result.value().content.find("b.cpp") != std::string::npos);
    REQUIRE(result.value().content.find("\"truncated\":false") !=
            std::string::npos);
}
```

Add cases proving `[x]+` is literal, ASCII-insensitive mode, 1-based Unicode column, non-overlapping matches, protected/invalid file omission, direct invalid leaf error, stable path/line/column order, max-results truncation, 2000-entry/500-file/16-MiB scan bounds, and 64-KiB serialized result bound.

- [ ] **Step 2: Run RED**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests
& .\build\baseline-vs2022\Debug\workspace_tool_gateway_tests.exe
```

Expected: the new search tests fail because `search_text` is not routed.

- [ ] **Step 3: Implement the minimal search contract**

```cpp
struct SearchMatch {
    std::string path;
    std::size_t line;
    std::size_t column;
    std::string text;
    bool line_truncated{false};
};
struct SearchOutput {
    std::vector<SearchMatch> matches;
    bool truncated{false};
    std::string truncation_reason;
    std::size_t scanned_files{0};
    std::size_t omitted_entries{0};
};
```

Sort candidate relative paths before reading; count bytes before each file; stop before crossing a scan/result/JSON limit and set an exact truncation reason (`max_results`, `entry_budget`, `file_budget`, `byte_budget`, or `output_bytes`). Do not use `std::regex` or locale-sensitive case conversion.

- [ ] **Step 4: Run GREEN and review limits**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests
& .\build\baseline-vs2022\Debug\workspace_tool_gateway_tests.exe
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
git diff --check
```

- [ ] **Step 5: Commit and back up**

```powershell
git add src/adapters/workspace/workspace_types.h src/adapters/workspace/workspace_file_ops.h src/adapters/workspace/workspace_file_ops.cpp src/adapters/workspace/workspace_tool_gateway.cpp tests/adapters/workspace_tool_gateway_test.cpp
git commit -m "feat: add bounded workspace text search"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
if ($local -ne $backup) { throw 'backup SHA mismatch' }
```

### Task 5: Implement versioned replace_text and write_file

**Files:**
- Modify: `src/adapters/workspace/workspace_types.h`
- Modify: `src/adapters/workspace/workspace_file_ops.h`
- Modify: `src/adapters/workspace/workspace_file_ops.cpp`
- Modify: `src/adapters/workspace/workspace_tool_gateway.cpp`
- Modify: `tests/adapters/workspace_tool_gateway_test.cpp`

**Interfaces:**
- Produces: `WorkspaceFileOps::replace_text` and `WorkspaceFileOps::write_file`.
- Produces: same-parent temp-write + platform replace helper used by both methods.

- [ ] **Step 1: Write mutation tests that first prove zero modification**

Add exact builders for both mutation tools:

```cpp
agent::ToolCall replace_call(std::string id,
                             std::string path,
                             std::string old_text,
                             std::string new_text,
                             std::int64_t occurrences,
                             std::string hash) {
    return {std::move(id), "replace_text",
            agent::Value::object({
                {"path", std::move(path)},
                {"old_text", std::move(old_text)},
                {"new_text", std::move(new_text)},
                {"expected_occurrences", occurrences},
                {"expected_sha256", std::move(hash)}})};
}

agent::ToolCall write_call(std::string id,
                           std::string path,
                           std::string content,
                           std::string mode,
                           std::optional<std::string> hash = std::nullopt) {
    agent::Value::Object args{{"path", std::move(path)},
                              {"content", std::move(content)},
                              {"mode", std::move(mode)}};
    if (hash.has_value()) {
        args.emplace("expected_sha256", std::move(*hash));
    }
    return {std::move(id), "write_file",
            agent::Value::object(std::move(args))};
}
```

```cpp
TEST_CASE(workspace_replace_requires_matching_hash_and_occurrence_count) {
    test::ScopedTempDir temp("workspace-replace");
    const auto file = temp.write_text("a.cpp", "old old\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto stale = gateway.execute(
        replace_call("call-replace", "a.cpp", "old", "new", 2,
                     std::string(64, '0')),
        context);
    REQUIRE(stale.has_value() && stale.value().is_error);
    REQUIRE(read_bytes(file) == "old old\n");
}
```

Add success tests for replace one/many, write create, overwrite, empty content, Unicode, and returned new SHA. Add failures for create-existing, overwrite-missing, missing parent, stale hash, wrong occurrence count, overlap (`aaa`/`aa`), invalid UTF-8/NUL, >1 MiB output, protected path, symlink/reparse/hard-link sentinel, and temp artifact omission.

- [ ] **Step 2: Run RED**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests
& .\build\baseline-vs2022\Debug\workspace_tool_gateway_tests.exe
```

Expected: new mutation tests fail because those routes return tool errors or are absent.

- [ ] **Step 3: Implement common atomic installation**

```cpp
struct WriteOutput {
    std::string path;
    bool created{false};
    std::string old_sha256;
    std::string new_sha256;
    std::size_t replacements{0};
    std::size_t bytes_written{0};
};

Outcome<WriteOutput> replace_text(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t expected_occurrences,
    std::string_view expected_sha256) const;

Outcome<WriteOutput> write_file(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::string_view content,
    bool create,
    std::optional<std::string_view> expected_sha256) const;
```

The private installer creates `.agent-tmp-<random>` beside the target with exclusive creation, writes/flushed binary bytes, verifies its hash, installs with `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows or `std::filesystem::rename` on POSIX, then re-reads and verifies. A create operation must never pass replace-existing. Failure cleanup is limited to the temp path created by that call.

- [ ] **Step 4: Run mutation GREEN, full tests, and sentinel scan**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target workspace_tool_gateway_tests
& .\build\baseline-vs2022\Debug\workspace_tool_gateway_tests.exe
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
rg -n "CreateProcess|system\(|ShellExecute|cmd\.exe|powershell" src/adapters/workspace tests/adapters/workspace_tool_gateway_test.cpp
git diff --check
```

Expected: tests pass; the forbidden process scan returns no matches (rg exit 1 is expected).

- [ ] **Step 5: Commit and back up**

```powershell
git add src/adapters/workspace/workspace_types.h src/adapters/workspace/workspace_file_ops.h src/adapters/workspace/workspace_file_ops.cpp src/adapters/workspace/workspace_tool_gateway.cpp tests/adapters/workspace_tool_gateway_test.cpp
git commit -m "feat: add versioned workspace edits"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
if ($local -ne $backup) { throw 'backup SHA mismatch' }
```

### Task 6: Compose the production gateway and prove the real file workflow

**Files:**
- Modify: `src/main.cpp:1-82`
- Modify: `tests/integration/runtime_integration_test.cpp:1-150`
- Modify: `tests/cli/cli_app_test.cpp:925-935`
- Modify: `CMakeLists.txt:53-104`
- Modify: `README.md`

**Interfaces:**
- Consumes: all five tool routes and `RuntimeConfig.runtime_root`.
- Produces: production `run` composition using `WorkspaceToolGateway`.
- Preserves: `verify-log` local-only construction and credential/config behavior.

- [ ] **Step 1: Add the failing real-gateway integration workflow**

Build a deterministic Fake Model script with four `ToolUse` responses followed by terminal text:

```cpp
agent::ToolCall list_call(std::string id, std::string path) {
    return {std::move(id), "list_files",
            agent::Value::object({
                {"path", std::move(path)},
                {"recursive", false},
                {"max_results", std::int64_t{100}}})};
}

agent::ToolCall read_call(std::string id,
                          std::string path,
                          std::int64_t start_line,
                          std::int64_t max_lines) {
    return {std::move(id), "read_file",
            agent::Value::object({
                {"path", std::move(path)},
                {"start_line", start_line},
                {"max_lines", max_lines}})};
}

agent::ToolCall replace_call(std::string id,
                             std::string path,
                             std::string old_text,
                             std::string new_text,
                             std::int64_t occurrences,
                             std::string hash) {
    return {std::move(id), "replace_text",
            agent::Value::object({
                {"path", std::move(path)},
                {"old_text", std::move(old_text)},
                {"new_text", std::move(new_text)},
                {"expected_occurrences", occurrences},
                {"expected_sha256", std::move(hash)}})};
}

agent::ModelResponse tool_response(agent::ToolCall call) {
    return {{agent::ToolUseBlock{std::move(call)}},
            agent::StopReason::ToolUse, "tool_use", 5, 3,
            "provider-request-tool"};
}

const auto original_hash = agent::workspace::sha256_hex("answer=41\n");
test::FakeModel model({
    tool_response(list_call("call-list", ".")),
    tool_response(read_call("call-read-before", "answer.txt", 1, 20)),
    tool_response(replace_call("call-replace", "answer.txt", "41", "42",
                               1, original_hash)),
    tool_response(read_call("call-read-after", "answer.txt", 1, 20)),
    fixtures::text_response(u8"已把答案改为 42，并重新读取验证。")});
```

The test must use real `WorkspaceToolGateway`, `JsonlEventStore`, and `RuntimeEngine`; assert final file `answer=42\n`, exact tool order, `Completed`, replay equality, and unchanged sentinels outside workspace, under `.git`, and under runtime root.

- [ ] **Step 2: Run RED**

```powershell
cmake --build build/baseline-vs2022 --config Debug --target runtime_integration_tests agent
& .\build\baseline-vs2022\Debug\runtime_integration_tests.exe
```

Expected: integration compile or behavior failure because the fixture and production main still use `EmptyToolGateway`.

- [ ] **Step 3: Wire production and document the exact milestone**

Change only the `run` composition:

```cpp
#include "adapters/workspace/workspace_tool_gateway.h"

agent::WorkspaceToolGateway tools(config.value().runtime_root);
```

Keep `verify-log` before runtime config and gateway construction. Update README with the five tool names, workspace/protected-path rules, limits, example `agent run`, and explicit statement that build/test execution and RAG are the next milestones rather than current capabilities.

- [ ] **Step 4: Run final file-tool acceptance**

```powershell
cmake --build build/baseline-vs2022 --config Debug --clean-first
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
$env:AGENT_BASE_URL=''
& .\build\baseline-vs2022\Debug\agent.exe run --workspace . --issue probe
if ($LASTEXITCODE -ne 2) { throw 'credential-free run contract changed' }
& .\build\baseline-vs2022\Debug\agent.exe verify-log --events tests/fixtures/valid-completed-events.jsonl
if ($LASTEXITCODE -ne 0) { throw 'verify-log failed' }
ctest --test-dir build/baseline-vs2022 -N
rg -n "CreateProcess|system\(|ShellExecute|cmd\.exe|powershell|git commit|embedding|vector database" src/adapters/workspace
git diff --check
```

Expected: clean build and all offline tests pass; run returns the fixed missing-config path; verify-log succeeds; CTest contains no live test; forbidden-scope scan has no production matches.

- [ ] **Step 5: Review scope, commit, and back up**

```powershell
git status --short
git diff --stat
git add CMakeLists.txt README.md src/main.cpp tests/integration/runtime_integration_test.cpp tests/cli/cli_app_test.cpp
git commit -m "feat: enable workspace file workflow"
git push backup feat/safe-workspace-tools
$local = git rev-parse HEAD
$tracking = git rev-parse refs/remotes/backup/feat/safe-workspace-tools
$backup = git --git-dir='E:\GitBackups\AgentFramework.git' rev-parse refs/heads/feat/safe-workspace-tools
$remote = (git ls-remote backup refs/heads/feat/safe-workspace-tools).Split("`t")[0]
if (@($local, $tracking, $backup, $remote) | Where-Object { $_ -ne $local }) {
    throw 'backup SHA mismatch'
}
if (git status --porcelain) { throw 'tracked worktree is not clean' }
```

## Completion Gate

Before calling the milestone complete:

1. Re-read the spec and map every acceptance bullet to a named test.
2. Confirm the only production tool names are the fixed five.
3. Confirm no state/event schema change occurred.
4. Confirm real Provider/live tests were not run and GitHub was not touched.
5. Use `superpowers:requesting-code-review` for a focused review, address findings with RED/GREEN evidence, then rerun the final acceptance commands.
6. Use `superpowers:finishing-a-development-branch` to merge locally into `main`, push `backup/main`, verify SHAs, and remove the worktree/feature branch only after merge verification.
7. Start a new architectural spec for structured build/test process tools; do not continue adding file features.
