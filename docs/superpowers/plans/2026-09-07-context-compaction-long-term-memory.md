# Context Compaction and Long-Term Memory Implementation Plan

Date: 2026-09-07
Design: `docs/superpowers/specs/2026-09-07-context-compaction-long-term-memory-design.md`

This plan is executed inline because it extends the already uncommitted interactive-Session work in the current checkout. Existing user-owned changes are preserved and no automatic commit is created.

## Task 1: Lock the session compaction projection with reducer tests

Files:

- Modify `tests/application/session_reducer_test.cpp`
- Modify `tests/adapters/session_event_json_test.cpp`
- Modify `src/domain/session_state.h`
- Modify `src/domain/session_event.h`
- Modify `src/domain/session_event.cpp`
- Modify `src/application/session_reducer.cpp`
- Modify `src/adapters/persistence/session_event_json.cpp`

Steps:

1. Add failing tests for retained turn records, valid prefix compaction, cumulative compaction, invalid coverage, pending-turn rejection, summary bounds, and JSON round-trip/shape rejection.
2. Run only `session_reducer_tests` and `session_event_json_tests` and confirm the expected compile/test failures.
3. Add `CommittedSessionTurn`, compaction state, `SessionCompactedPayload`, reducer invariants, and strict JSON codec support.
4. Re-run the focused tests until green.

## Task 2: Add a strict append-only memory domain and secure store

Files:

- Add `src/domain/memory_event.h`
- Add `src/domain/memory_event.cpp`
- Add `src/domain/memory_state.h`
- Add `src/application/memory_reducer.h`
- Add `src/application/memory_reducer.cpp`
- Add `src/ports/memory_store.h`
- Add `src/adapters/persistence/memory_event_json.h`
- Add `src/adapters/persistence/memory_event_json.cpp`
- Add `src/adapters/persistence/jsonl_memory_store.h`
- Add `src/adapters/persistence/jsonl_memory_store.cpp`
- Add `tests/application/memory_reducer_test.cpp`
- Add `tests/adapters/memory_event_json_test.cpp`
- Add `tests/adapters/jsonl_memory_store_test.cpp`
- Modify `CMakeLists.txt`

Steps:

1. Write failing tests for upsert, tombstone, consolidation checkpoints, sequence continuity, id/category/scope/provenance validation, duplicate keys, extra/wrong shapes, corrupt lines, symlink roots/leaves, and hard links.
2. Run the three new focused test targets and observe RED.
3. Implement the smallest domain, reducer, codec, and store that satisfy the invariants, reusing the hardened Session-store native file boundary.
4. Re-run until green.

## Task 3: Implement memory safety, retrieval, and opt-out

Files:

- Add `src/application/memory_policy.h`
- Add `src/application/memory_policy.cpp`
- Add `src/application/memory_retriever.h`
- Add `src/application/memory_retriever.cpp`
- Add `tests/application/memory_policy_test.cpp`
- Add `tests/application/memory_retriever_test.cpp`
- Modify `CMakeLists.txt`

Steps:

1. Add failing tests for MiniMax/OpenAI-style key material, bearer/password/private-key patterns, valid ordinary text, UTF-8/size limits, workspace/global scope, Chinese and ASCII matches, stable ranking, top-K/byte limits, forgotten entries, and opt-out synonyms.
2. Run focused tests and confirm RED.
3. Implement bounded validators, deterministic Unicode-aware tokenization/ranking, and turn-local opt-out.
4. Re-run until green.

## Task 4: Implement model-backed compaction and consolidation services

Files:

- Add `src/ports/context_compactor.h`
- Add `src/ports/memory_consolidator.h`
- Add `src/application/model_context_compactor.h`
- Add `src/application/model_context_compactor.cpp`
- Add `src/application/model_memory_consolidator.h`
- Add `src/application/model_memory_consolidator.cpp`
- Add `tests/application/model_context_compactor_test.cpp`
- Add `tests/application/model_memory_consolidator_test.cpp`
- Modify `CMakeLists.txt`

Steps:

1. Add recording/failing model fakes and failing tests for tool-free requests, prior-summary inclusion, exact compacted prefix, strict JSON candidate parsing, duplicate/extra/wrong shapes, secret filtering, output bounds, and sanitized provider failures.
2. Run focused tests and confirm RED.
3. Implement both services on `ModelClient` without exposing internal calls to runtime tools.
4. Re-run until green.

## Task 5: Orchestrate compaction and long-term memory in the Session engine

Files:

- Modify `src/application/session_engine.h`
- Modify `src/application/session_engine.cpp`
- Add `src/application/memory_engine.h`
- Add `src/application/memory_engine.cpp`
- Modify `tests/application/session_engine_test.cpp`
- Add `tests/application/memory_engine_test.cpp`
- Modify `CMakeLists.txt`

Steps:

1. Add failing tests for threshold-triggered compaction, exact recent-turn retention, summary/memory prompt sections, turn-local opt-out, soft compaction failure fallback, hard-limit rejection before `turn_started`, checkpointed consolidation, retry after failure, explicit remember, list, and forget.
2. Run focused tests and confirm RED.
3. Implement configuration objects, prompt section assembly, non-destructive compaction, memory engine operations, and boundary consolidation.
4. Re-run until green, including pending-turn recovery tests.

## Task 6: Wire strict configuration and interactive controls

Files:

- Modify `src/config/runtime_config.h`
- Modify `src/config/runtime_config.cpp`
- Modify `src/cli/interactive_cli.h`
- Modify `src/cli/interactive_cli.cpp`
- Modify `src/main.cpp`
- Modify `tests/cli/interactive_cli_test.cpp`
- Modify configuration tests under `tests/cli/cli_app_test.cpp` or add `tests/config/runtime_config_test.cpp`
- Modify `CMakeLists.txt`

Steps:

1. Add failing tests for defaults, invalid flags/ranges/relationships, every memory command, status output, consolidation at new/clear/exit/EOF, and failure messages that do not block exit.
2. Run focused tests and confirm RED.
3. Wire the concrete store, services, engines, progress messages, and CLI commands.
4. Re-run until green.

## Task 7: Prove restart, cross-Session injection, compaction, and package behavior

Files:

- Extend `tests/cli/interactive_process_test.cpp`
- Add or extend CMake process scripts under `tests/cli/`
- Modify package configuration/documentation files discovered in the current checkout
- Modify the ignored `.env` and staged Ready `.env` without printing credentials
- Modify `CMakeLists.txt`

Steps:

1. Extend the scripted local HTTP provider to identify normal, compaction, and consolidation requests without recording secrets.
2. Add a failing process scenario that creates a memory, starts a new Session, restarts the executable, observes relevant memory injection, triggers compaction at a tiny test threshold, forgets the memory, and verifies it is absent later.
3. Add staged-package parity and runtime-debris checks.
4. Implement packaging/config changes and run the process tests until green.

## Task 8: Documentation, hardening closure, and final verification

Files:

- Add `README.md` only if the user-owned deletion is not intentional; otherwise add a focused guide under `docs/`
- Add `docs/interactive-memory.md`
- Update `.assistant-hardening/` using the provided scripts

Steps:

1. Document double-click startup, Session versus Task versus memory, all commands, data locations, configuration, privacy, opt-out, forgetting, and recovery.
2. Run Debug configure/build and the complete Debug CTest suite.
3. Run Release configure/build and the complete Release CTest suite.
4. Run deterministic interactive/process tests from the staged Ready package.
5. Rebuild the Ready package last, inspect its file list, scan outside `.env` for credential material, verify binary parity, and confirm no `runtime_data` or caches are shipped.
6. Run one minimal real MiniMax-M3 multi-turn smoke, checking request roles and response completion without printing credentials or provider bodies.
7. Record verification probes, mark all five hardening findings fixed with exact regression tests/fixes, summarize the round, and run strict hardening validation.
