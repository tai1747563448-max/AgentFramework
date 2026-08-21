# Structured build and test tools design

Date: 2026-08-21

## 1. Goal and scope

This milestone lets the existing single Coding Agent run the verification half
of a coding workflow after it has inspected and edited files:

1. configure a CMake project;
2. build the project or one named target;
3. run all CTest tests or one exact named test;
4. return bounded, structured process evidence to the model so it can inspect a
   failure, edit again, and retry.

The milestone preserves the current C++17 Runtime, direct `Failed` state with a
reason, JSONL replay, safe progress reporting, and five workspace file tools.
It does not add multi-agent coordination, arbitrary shell execution, Git
mutation, package installation, deployment, or the Python RAG sidecar. RAG is
the next independently reviewed milestone.

## 2. Chosen approach

Three alternatives were considered:

1. Expose an arbitrary shell command. This is flexible but turns model text
   directly into a command language and makes quoting, policy, and auditability
   unbounded. It is rejected.
2. Expose one generic process tool with an executable and argument array. This
   avoids a shell but still lets the model execute any installed program. It is
   rejected as the public tool contract.
3. Keep a reusable internal direct-process Port, and expose only fixed
   CMake/CTest adapters with closed schemas. This is selected. The model can
   choose a build configuration, a validated target, or an exact test name; it
   cannot choose the executable or append arbitrary flags.

The internal process runner is reusable by the later Python RAG adapter, but it
is never itself included in model tool definitions.

## 3. Architecture

New boundaries:

- `ProcessRunner` is a Port carrying a fixed program, an argument vector,
  working directory, UTF-8 stdin, timeout, output budgets, and internal
  environment overrides.
- `DirectProcessRunner` is the native OS adapter. Windows uses
  `CreateProcessW`; POSIX uses `fork` plus `execvp`. Neither path invokes a
  shell.
- `CMakeToolGateway` owns the three public build/test tool contracts and maps
  them to fixed CMake/CTest requests.
- `CompositeToolGateway` combines the existing `WorkspaceToolGateway` and the
  optional `CMakeToolGateway`, rejects duplicate names, and routes by exact
  tool name.

Production composition remains:

`CLI -> RuntimeEngine -> CompositeToolGateway -> workspace/build adapters`

The Runtime still persists `ToolCallStarted` before execution and persists the
bounded `ToolResult` after execution. A command exit code or timeout is a
model-visible `ToolResult{is_error=true}` and does not become a task-level
`ToolCallFailed`. Only an internal Port/protocol failure uses the existing
direct task failure path.

## 4. Public tool contracts

All schemas are objects with `additionalProperties=false`.

### `configure_project`

Required input:

```json
{"configuration":"Debug"}
```

`configuration` is exactly `Debug` or `Release`.

Fixed execution:

```text
cmake -S <canonical-workspace> -B <workspace>/.agent/cmake-build
      -DCMAKE_BUILD_TYPE=<configuration>
```

### `build_project`

Required `configuration`, optional `target`:

```json
{"configuration":"Debug","target":"runtime_engine_tests"}
```

`target` is 1-128 ASCII characters from `[A-Za-z0-9_.+-]`. It becomes one
argument after `--target`; it is never parsed as a command line.

Fixed execution:

```text
cmake --build <workspace>/.agent/cmake-build --config <configuration>
      [--target <validated-target>]
```

### `run_tests`

Required `configuration`, optional `test_name`:

```json
{"configuration":"Debug","test_name":"runtime_engine_tests"}
```

`test_name` is strict UTF-8 text, 1-200 bytes. It is escaped as a literal CTest
regular expression and surrounded with `^...$`; the model cannot provide CTest
flags or regex operators.

Fixed execution:

```text
ctest --test-dir <workspace>/.agent/cmake-build -C <configuration>
      --output-on-failure --no-tests=error
      [-R <escaped-exact-name>]
```

## 5. Workspace and build-directory policy

The build directory is always `.agent/cmake-build` below the canonical task
workspace. It is not model-selectable.

Before process execution, the adapter:

1. validates that the supplied workspace exists, is a real directory, and is
   not reached through a symlink/junction alias;
2. creates `.agent` and `.agent/cmake-build` one component at a time when
   absent;
3. rejects an existing non-directory, symlink, reparse point, or canonical path
   outside the workspace;
4. rechecks the final canonical directory before use.

The file-tool path policy adds `.agent` as a protected component, so model file
operations cannot inspect or mutate build artifacts or adapter control data.
The threat model still excludes a hostile concurrent filesystem actor racing
between the final checks and the child process.

## 6. Direct process contract

`ProcessRequest` contains:

- program and ordered arguments supplied only by a trusted adapter;
- canonical working directory;
- optional UTF-8 stdin;
- positive timeout in milliseconds;
- independent stdout/stderr byte budgets;
- trusted environment overrides.

`ProcessOutput` contains:

- signed exit code;
- `timed_out`;
- duration in milliseconds;
- UTF-8 stdout and stderr;
- independent truncation flags.

The runner drains stdout and stderr concurrently, writes stdin without a shell,
and terminates the whole child process group/job on timeout. Output collectors
retain a small prefix and the most recent tail while never retaining more than
their configured budget. Invalid native output encoding is converted to valid
UTF-8 without throwing into the Runtime.

On Windows, arguments use the documented CRT quoting rules and are passed to
`CreateProcessW` as one generated command line. On POSIX, each argument is an
independent `execvp` array element.

## 7. Environment boundary

Child processes do not inherit the complete agent environment. The runner
constructs a small platform allowlist needed for executable lookup, temporary
files, locale, user paths, and native build tools, then applies adapter-owned
overrides such as English build diagnostics. Provider credentials
(`AGENT_API_KEY`, `AGENT_AUTH_TOKEN`) and generic secret/token/password
variables are never copied.

This is credential hygiene, not a sandbox. CMake configure, compilers, tests,
and project scripts execute code from the selected workspace and can access
files available to the current OS user. The tools are therefore opt-in and
must only be enabled for a trusted local workspace.

## 8. Result and error semantics

Successful or failed child execution returns bounded JSON containing:

```json
{
  "operation":"build",
  "exit_code":1,
  "timed_out":false,
  "duration_ms":1234,
  "stdout":"...",
  "stderr":"...",
  "stdout_truncated":false,
  "stderr_truncated":true
}
```

The complete serialized tool content is at most 64 KiB. If JSON escaping would
cross the limit, the adapter removes complete UTF-8 units from the retained
output prefixes/tails and marks the corresponding truncation flag. The error
flag is true when the process times out or exits nonzero.

Invalid arguments, unsafe workspace/build paths, unavailable process programs,
and output/timeout problems use fixed bounded error codes and messages. Raw OS
error text, environment values, and credentials are not returned.

## 9. Configuration and production exposure

New configuration:

- `AGENT_ENABLE_BUILD_TOOLS=0|1`, default `0`;
- `AGENT_BUILD_TIMEOUT_SECONDS`, positive and at most 600, default `300`.

When disabled, the model receives exactly the existing five file tools. When
enabled, the composite exposes those five followed by the three build tools.
`verify-log` remains credential/config/process independent and never constructs
the runner or either tool gateway.

## 10. Verification strategy

TDD coverage includes:

- real process fixture proving literal shell metacharacters, Unicode
  args/cwd/stdin, concurrent stdout/stderr capture, output budgets, secret-env
  removal, missing-program handling, and whole-tree timeout termination;
- composite routing, deterministic definition order, duplicate rejection, and
  unknown-tool behavior;
- exact three CMake schemas and exact fake-runner requests;
- invalid configuration/target/test name and safe build-directory cases;
- real offline configure/build/CTest of a tiny temporary CMake project through
  `CMakeToolGateway` and `DirectProcessRunner`;
- Runtime integration proving a failed test is persisted as a model-visible
  tool result, followed by a second model round and successful retry;
- full MSVC Debug build, all offline CTest, credential-free `run`,
  credential-free `verify-log`, no live test, no arbitrary-shell definition,
  independent review, and local bare-backup SHA verification.

Live Provider traffic, package installation, GitHub, and arbitrary project
commands are not part of this acceptance.
