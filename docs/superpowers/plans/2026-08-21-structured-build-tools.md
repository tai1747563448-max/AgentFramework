# Structured build and test tools implementation plan

> Execute this plan with test-driven-development and verification-before-
> completion. The feature branch is `feat/structured-build-tools`; no GitHub or
> live Provider request is in scope.

**Goal:** Add opt-in, shell-free CMake configure/build/CTest tools to the
existing single Coding Agent, with bounded process evidence and no change to
the Runtime state machine.

**Architecture:** A reusable `ProcessRunner` Port is implemented by native OS
process code. `CMakeToolGateway` maps three closed public schemas to fixed
program/argument vectors. `CompositeToolGateway` combines it with the existing
file gateway. Build tools are exposed only when configuration explicitly
enables them.

**Tech:** C++17, Windows `CreateProcessW`/Job Objects, POSIX `fork`/`execvp`,
nlohmann JSON, CMake, CTest, the repository's test harness.

## Global bindings

- Preserve every existing event kind, payload, task status, Reducer rule,
  Runtime guard, direct task failure behavior, provider mapping, and JSONL
  schema.
- Never add a public shell/program/argv tool.
- The only public names added are `configure_project`, `build_project`, and
  `run_tests`.
- Process exit/timeout is a returned `ToolResult` with `is_error=true`; only an
  internal runner/protocol failure may return outer `Result` failure.
- `.agent/cmake-build` is fixed and protected from file tools.
- Every ordinary test is offline. Do not run `anthropic_live_smoke`.
- Commit and push explainable milestones to local remote `backup`; do not
  access GitHub.

## Task 1: Define and prove the direct process Port

**Files:**

- Create: `src/ports/process_runner.h`
- Create: `src/adapters/process/direct_process_runner.h`
- Create: `src/adapters/process/direct_process_runner.cpp`
- Create: `tests/process/process_fixture.cpp`
- Create: `tests/adapters/direct_process_runner_test.cpp`
- Modify: `CMakeLists.txt`

### Step 1: Write the process fixture and failing contract tests

Add a small fixture executable with explicit modes:

- emit received argv, cwd, and stdin as JSON;
- emit independent stdout/stderr byte counts;
- report whether named environment variables exist without printing values;
- spawn a child marker process and wait, so timeout can prove whole-tree kill;
- return a requested nonzero exit code.

Tests must directly require:

- `ProcessRunner`, `ProcessRequest`, `ProcessOutput`, and
  `DirectProcessRunner`;
- literal `&|><"` arguments arrive unchanged and do not create sentinel files;
- Unicode cwd/argv/stdin round-trip;
- stdout and stderr are captured independently while both pipes are active;
- configured head/tail budgets are never exceeded and truncation is true;
- `AGENT_API_KEY`, `AGENT_AUTH_TOKEN`, and a generic `*_SECRET` variable are
  absent in the child while PATH/temp/system essentials remain usable;
- timeout marks `timed_out`, kills the descendant, and returns promptly;
- a missing executable returns a fixed outer `ProcessFailure` without raw OS
  diagnostics.

Register `process_fixture` and `direct_process_runner_tests`.

### Step 2: Run the focused build and record genuine RED

```powershell
cmake --build build/baseline-vs2022 --config Debug `
  --target direct_process_runner_tests
```

Expected RED: compilation fails because `ports/process_runner.h` or the direct
adapter does not exist. A fixture syntax/configuration failure is not the
behavioral RED and must be corrected first.

### Step 3: Implement the minimal cross-platform runner

Implement bounded collectors, UTF-8 normalization, a child environment
allowlist, Windows CRT quoting, Windows pipes/job/process handling, POSIX
pipes/process group/exec handling, concurrent drains, stdin writing, timeout,
exit-code capture, and fixed sanitized failures.

The Port accepts only trusted adapter-owned program/argv values. Do not expose
it through `ToolGateway`.

### Step 4: Run focused GREEN

Build and execute `direct_process_runner_tests`. All named behaviors must pass;
no child sentinel may survive the timeout test.

### Step 5: Self-review and commit

Run `git diff --check`, inspect handle/fd closure on every early return, and
commit:

```powershell
git add CMakeLists.txt src/ports/process_runner.h `
  src/adapters/process/direct_process_runner.h `
  src/adapters/process/direct_process_runner.cpp `
  tests/process/process_fixture.cpp `
  tests/adapters/direct_process_runner_test.cpp
git commit -m "feat: add bounded direct process runner"
git push -u backup feat/structured-build-tools
```

Verify local/tracking/bare SHAs match.

## Task 2: Add deterministic composite tool routing

**Files:**

- Create: `src/adapters/tools/composite_tool_gateway.h`
- Create: `src/adapters/tools/composite_tool_gateway.cpp`
- Create: `tests/adapters/composite_tool_gateway_test.cpp`
- Modify: `CMakeLists.txt`

### Step 1: Write failing routing tests

Use small fake gateways to require:

- child definition order is stable;
- calls route only to the owner of the exact name;
- the original execution context is forwarded unchanged;
- duplicate definition names are rejected at construction;
- an unknown name returns a bounded model-visible `invalid_arguments` result
  with the original tool-call ID;
- no child is called for an unknown name.

### Step 2: Capture RED

Build `composite_tool_gateway_tests`. Expected RED is the missing composite
adapter API.

### Step 3: Implement and reach GREEN

Store non-owning gateway references whose lifetimes are owned by production
composition. Build an exact name-to-child table once; concatenate definitions
in supplied order. Do not catch and hide duplicate programmer errors.

Run the focused executable, `git diff --check`, and commit:

```powershell
git commit -m "feat: compose closed tool gateways"
git push backup feat/structured-build-tools
```

Verify backup SHA equality.

## Task 3: Add the closed CMake/CTest gateway

**Files:**

- Create: `src/adapters/build/cmake_tool_gateway.h`
- Create: `src/adapters/build/cmake_tool_gateway.cpp`
- Create: `tests/adapters/cmake_tool_gateway_test.cpp`
- Modify: `src/adapters/workspace/workspace_path_policy.cpp`
- Modify: `tests/adapters/workspace_primitives_test.cpp`
- Modify: `CMakeLists.txt`

### Step 1: Write exact-schema and fake-process tests

With a fake `ProcessRunner`, require full equality for the three ordered
definitions and every field/bound/enum.

Require exact requests for:

- Debug and Release configure;
- build all and one validated target;
- all tests and one exact regex-escaped test name;
- canonical workspace cwd and fixed `.agent/cmake-build` path;
- fixed program names and argument ordering;
- timeout and output budgets;
- adapter-owned diagnostic environment overrides.

Require bounded JSON for success, nonzero exit, timeout, huge escaped output,
and fixed process-unavailable errors. Invalid/extra fields, invalid targets,
invalid UTF-8/NUL test names, unsafe workspace aliases, and unsafe existing
`.agent` objects must never call the runner.

Extend workspace primitives so any model file path containing `.agent` is
`AccessDenied` while similarly named ordinary components remain legal.

### Step 2: Capture focused RED

Build `cmake_tool_gateway_tests` and `workspace_primitives_tests`. Expected RED
is the missing gateway and the current `.agent` allowance.

### Step 3: Implement safe fixed build-directory preparation

Validate the workspace with the existing physical-root policy. Create the two
fixed private directory components one at a time, inspect symlink/reparse/type,
and compare canonical containment before returning the build directory.

Do not generalize this into a model-selectable directory API.

### Step 4: Implement argument decoding and result mapping

Use closed exact-key checks and strict types. Escape exact test names for CTest
regex matching. Map runner output into a serialized result no larger than
64 KiB, preserving UTF-8 and the most useful tail. Nonzero/timed-out outputs are
tool errors with process evidence, not outer Runtime errors.

### Step 5: Focused GREEN and commit

Run both focused executables plus `workspace_tool_gateway_tests`, then commit:

```powershell
git commit -m "feat: add structured cmake tools"
git push backup feat/structured-build-tools
```

Verify backup SHA equality.

## Task 4: Prove a real configure-build-test cycle

**Files:**

- Create: `tests/integration/build_tool_integration_test.cpp`
- Modify: `CMakeLists.txt`

### Step 1: Write a real offline integration test

Create a temporary Unicode workspace containing a minimal CMake project and one
CTest test. Through real `CMakeToolGateway` plus `DirectProcessRunner`:

1. configure Debug;
2. build the test target;
3. run the exact named test and require exit 0;
4. replace its source with an intentional assertion failure, rebuild, and
   require `run_tests` returns `is_error=true` with nonzero exit and bounded
   failure evidence;
5. prove `.agent` cannot be listed or read through `WorkspaceToolGateway`.

Skip is not allowed merely because the compiler is slow. If CMake/compiler is
genuinely unavailable, the test configuration must fail clearly rather than
claim pass.

### Step 2: Capture behavioral RED

The initial integration test must fail before the necessary composition/path
behavior is complete. Record the exact failing assertion.

### Step 3: Make only necessary fixes and reach GREEN

Do not add project-specific flags or install dependencies. Run the integration
test twice to detect stale build/output behavior.

Commit:

```powershell
git commit -m "test: prove real structured build workflow"
git push backup feat/structured-build-tools
```

## Task 5: Add opt-in configuration and production composition

**Files:**

- Modify: `src/config/runtime_config.h`
- Modify: `src/config/runtime_config.cpp`
- Modify: `tests/cli/cli_app_test.cpp`
- Modify: `src/main.cpp`
- Modify: `.env.example`
- Modify: `README.md`
- Modify: `tests/integration/runtime_integration_test.cpp`

### Step 1: Write configuration and composition tests first

Require:

- default disabled and default 300-second timeout;
- exact `0`/`1` flag parsing;
- rejection of empty/malformed flag, zero/negative/overflow timeout, and timeout
  above 600 seconds;
- when disabled, production-equivalent composition exposes exactly five file
  tools;
- when enabled, it exposes exactly eight ordered tools;
- a scripted Runtime workflow persists a failing structured test result,
  returns it to the next model request, then persists a successful retry and
  completes without `ToolCallFailed`.

### Step 2: Capture RED

Build focused config/CLI and Runtime integration targets. Expected RED is
missing config fields/composite production behavior.

### Step 3: Implement minimal production wiring

Keep `verify-log` dispatch before environment/config/process construction.
For `run`, own the direct runner, file gateway, build gateway, and composite in
an order that keeps every reference alive. Add the build gateway to the
composite only when enabled.

Update the default system prompt only enough to require inspect/edit/verify and
to use only definitions actually supplied.

Document the trust boundary and exact opt-in. Do not claim RAG is implemented;
name it as the next milestone.

### Step 4: Focused and full GREEN

Run all new focused executables, then:

```powershell
cmake --build build/baseline-vs2022 --config Debug --clean-first
ctest --test-dir build/baseline-vs2022 -C Debug --output-on-failure
```

Also run credential-free `run` and `verify-log`, list CTest registrations, and
scan production definitions to prove there is no shell/program/argv tool.

Commit:

```powershell
git commit -m "feat: wire opt-in build verification tools"
git push backup feat/structured-build-tools
```

## Task 6: Independent review and final acceptance

**Files:**

- Create: `docs/superpowers/reports/2026-08-21-structured-build-tools-review.md`
- Modify only files required by accepted review findings.

### Step 1: Self-review exact scope

Confirm:

- no state/event/JSONL changes;
- no public shell, executable, argv, cwd, environment, package, Git, or network
  input;
- fixed build directory and `.agent` protection;
- secret environment variables absent from child tests;
- process timeout kills descendants;
- all outputs and errors are bounded valid UTF-8;
- default production tool count is five and explicit opt-in count is eight;
- no live/provider test was registered or run;
- `git diff --check main..HEAD` is clean.

### Step 2: Request whole-branch read-only review

Review `main..feat/structured-build-tools` against the design and plan. Every
Critical/Important finding is TDD-fixed and re-reviewed. Minor evidence gaps are
fixed when cheap or documented without inflating claims.

### Step 3: Fresh final verification

Use a fresh MSVC build directory with live tests OFF. Run full build, full
CTest, the real build integration twice, credential-free CLI checks, forbidden
tool/config scans, and local/bare SHA checks.

### Step 4: Finish locally

After `Ready: Yes`, use the finishing-development-branch workflow. The user's
standing local-first choice selects local merge to `main`, not a PR. Re-run the
full suite on merged `main`, push `backup/main`, verify SHAs, then clean only the
owned worktree/local feature branch. Keep the bare feature branch as recoverable
history unless explicitly asked to delete it.

## Acceptance result

This milestone is complete only when a trusted local workspace can perform a
real configure/build/test/fail/fix/test cycle through the exact structured
tools, the persisted Runtime workflow can iterate on a test failure, all
offline tests pass on merged `main`, and independent review reports no
Critical/Important findings. It does not yet complete the overall Agent goal;
Python RAG, resume/evaluation, a real coding scenario, and the Chinese manual
remain required.
