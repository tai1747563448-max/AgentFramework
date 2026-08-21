# Structured Build Tools Review Fixes

Date: 2026-08-21  
Branch: `feat/structured-build-tools`  
Reviewed base: `3bc5e1da4428e0f1376426ee89184d2e81f2f29d`  
Remote: local bare `backup` only; GitHub was not used.

## Scope

This fix round closes the independent review of the structured CMake build
milestone. It does not add a shell, arbitrary executable or argument tool,
package management, Git, network access, RAG, recovery, or multi-agent
behavior. The model-visible tool surface remains exactly the five workspace
file tools plus `configure_project`, `build_project`, and `run_tests`.

## Accepted findings and fixes

1. POSIX children no longer retain the write end of their own stdin pipe or
   unrelated pipe descriptors. Every pipe descriptor is created close-on-exec;
   argv and envp are prepared before `fork`; the executable is resolved from
   the allowlisted environment before `fork`; and the child performs checked
   process-group, `dup2`, close, `chdir`, and `execve` operations.
2. UTF-8 normalization now preserves valid UTF-8 segments, replaces individual
   invalid bytes, and reports truncation when replacement expansion cannot fit
   the byte budget. Tests cover invalid bytes, mixed valid/invalid output, and
   a multibyte boundary.
3. The production `agent_runtime` target now publicly declares
   `Threads::Threads`, so static-library consumers inherit the runner's thread
   dependency.
4. The real CMake integration now completes configure -> build -> pass -> edit
   to failure -> fail -> restore -> rebuild -> pass, and was run twice in the
   focused verification.
5. Gateway tests cover a linked workspace root, linked `.agent`, and linked
   `.agent/cmake-build`; the runner must remain uncalled. Windows reported an
   explicit privilege-based SKIP, while WSL executed all three link cases.
6. The process-tree fixture writes a ready marker before its delayed survival
   marker. The timeout test requires ready to exist and survival to remain
   absent, proving that a started descendant was terminated.
7. Windows exit status is interpreted through signed 32-bit semantics before
   promotion to the public 64-bit field.
8. Public target and test-name schemas describe the length, character, and
   exact-literal constraints already enforced by the gateway.

## TDD evidence

The original WSL/g++13 RED was genuine: the stdin/Unicode DirectProcessRunner
test timed out with exit `-1`, while the other three then-existing runner tests
passed. After adding the new Windows regressions but before production fixes:

- `direct_process_runner_tests` failed the invalid-output normalization and
  signed Windows exit-code tests;
- `cmake_tool_gateway_tests` failed the exact schema comparison;
- the strengthened real recovery cycle and ready/survival process-tree test
  passed immediately, so they are recorded as coverage improvements rather
  than claimed as product REDs;
- Windows could not create directory symlinks under the current token and
  printed an explicit SKIP.

After the production fixes:

- Windows DirectProcessRunner: 8/8 passed;
- WSL Ubuntu with g++13 DirectProcessRunner: 7/7 passed, including the formerly
  deadlocked stdin/Unicode case;
- Windows CMake gateway: 6/6 passed with the documented symlink SKIP;
- WSL CMake gateway: 6/6 passed with all link regressions executed;
- the real CMake recovery integration passed twice consecutively.

## Full offline verification

- MSVC Debug `--clean-first` full build: exit 0.
- Offline CTest: 15/15 passed, 0 failed.
- Credential-free `run`: exit 2 with exactly
  `AGENT_BASE_URL is required`.
- Credential-free `verify-log`: exit 0 with task
  `task-0000000000000000000000000000000b`, status `Completed`, and sequence 6.
- `ctest -N`: exactly 15 offline tests; no live test is registered.
- Production definition scan: exactly eight names, the five file tools and
  three CMake tools; no shell/program/argv public definition exists.
- `git diff --check`: passed.

## Explicit limits

WSL has g++13 but no CMake installation, so this round used direct g++ builds
for the two POSIX-focused executables and does not claim a Linux CMake configure
or full Linux production build. The production CMake target now declares its
thread dependency and the full MSVC build passes. No credentialed Provider,
live network, package installation, or GitHub operation was run.
