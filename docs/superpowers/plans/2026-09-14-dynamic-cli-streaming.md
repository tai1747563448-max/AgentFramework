# Dynamic CLI and Streaming Implementation Plan

> For agentic workers: use superpowers:subagent-driven-development or executing-plans. User approved implementation with “做吧”. Continue through implementation, review, and verification.

**Goal:** Animated in-place status and real incremental model text in the existing Windows interactive CLI.
**Architecture:** Keep synchronous Session/Runtime work on a joinable worker while one presenter owns terminal output. Stream bytes through a bounded decoder and return a complete ModelResponse for existing validation and persistence. Transient display callbacks never enter the event log.
**Tech Stack:** C++17, existing CPR 1.14.2, nlohmann_json, Threads, Windows VT.
**Spec:** ../specs/2026-09-14-dynamic-cli-streaming-design.md

## Global constraints

- No new third-party dependency. Preserve noninteractive command and persisted log contracts.
- No full TUI, fixed input box or command completion in this change.
- RAG guarded responses only display after successful Session commit. Ordinary preview is explicitly provisional.
- No automatic retry; no tool execution from partial JSON; preserve cancellation classifications.
- Parent owns CMakeLists.txt and shared build invocation. Workers have exclusive file ownership below and report tests before implementation. No shared build races; request parent build slots.

## Shared interfaces

Model layer owns `src/domain/model_stream_event.h`:

```cpp
enum class ModelStreamEventKind { TextDelta, TextBlockEnd };
struct ModelStreamEvent {
    ModelStreamEventKind kind{ModelStreamEventKind::TextDelta};
    std::size_t block_index{0};
    std::string text;
};
using ModelStreamObserver = std::function<void(const ModelStreamEvent&)>;
struct ModelCallOptions {
    bool stream{false};
    const Cancellation* cancellation{nullptr};
    ModelStreamObserver observer;
};
// ModelClient retains its old pure virtual complete(request), adds:
virtual Result<ModelResponse> complete(
    const ModelRequest& request, const ModelCallOptions& options);
```

The new overload defaults to old complete(request), with cancellation checks. Anthropic overrides it. The callback is optional and exceptions disable preview rather than fail the model.

Runtime layer owns additions in runtime_engine.h:

```cpp
struct RuntimeTextUpdate {
    std::string task_id;
    std::size_t model_round{0};
    ModelStreamEvent event;
};
struct RuntimePresentationOptions {
    bool stream{false};
    std::function<void(const RuntimeTextUpdate&)> text_observer;
    std::function<void(const std::string&)> phase_observer;
};
```

Append `RuntimePresentationOptions presentation;` to RunRequest and ResumeRequest. Keep existing RuntimeProgressObserver/SessionRunTask delegate signatures. Append `std::string tool_name;` to RuntimeProgress. Session submit_turn/recover_pending_turn accept an optional final presentation argument after use_memory. Existing call sites still compile. Keep defaults off outside interactive CLI.

CLI adds optional `submit_presented` and `recover_presented` fields to InteractiveSessionCommands with existing arguments plus `const RuntimePresentationOptions&`. Existing submit/recover remain fallback for existing tests/clients. main wires the new delegates. The presenter creates transient text/phase callbacks per active turn.

## Task 1: Streaming model transport (worker model)

Files: src/domain/model_stream_event.h; src/ports/model_client.h; src/adapters/anthropic/*; tests/adapters/anthropic_messages_client_test.cpp and new streaming tests as needed.

- [x] Add a failing test: call the new overload against fragmented SSE and assert delta arrives before completion; test failed/unfinished stream produces no successful ModelResponse.
- [x] Record the failing build/test; provide new filenames to parent for CMake registration.
- [x] Implement chunked transport using existing CPR callbacks, cancellation and existing timeout/redirect/error sanitation contracts.
- [x] Implement bounded SSE framing and response assembly with text/tool blocks, strict lifecycle, complete-message validation, unknown auxiliary event tolerance and unsupported content rejection.
- [x] Cover UTF-8/CRLF/multiline data, tool fragments, 200+error, EOF, oversize, callback throw and cancellation including no-byte wait. Preserve buffered complete(request).
- [x] Parent runs focused adapter tests; fix failures and report evidence.

## Task 2: Runtime/session streaming and cancellation (worker runtime)

Files: src/application/runtime_engine.*; session_engine.*; src/adapters/system/signal_cancellation.*; tests/application/runtime_engine_test.cpp; session_engine_test.cpp; optional signal tests.

- [x] Write failing tests with a model fake that emits text, returns tool/full/failure/cancel results. Assert RAG suppresses previews, partial tool never runs, failed turn never commits, and Cancelled differs from Failed.
- [x] Add the shared presentation structures and optional Session parameters above. Propagate options without changing persistent events.
- [x] Retain tool and response checks; check cancellation after model returns and before acceptance. Pass cancellation to model call options even when preview disabled.
- [x] Emit tool names from durable events and phase callbacks for internal compaction/memory. Isolate callback failures.
- [x] Implement explicit signal cancellation begin/reset/end lifecycle usable by main for each interactive turn, preserving noninteractive behavior and idle Ctrl+C exit.
- [x] Parent runs runtime/session/signal tests, including accepted response/commit failure and next-turn cancellation reset.

## Task 3: Terminal presenter and interactive wiring (worker cli)

Files: src/cli/*; src/main.cpp; tests/cli/*; new terminal presenter tests.

- [x] Add failing tests for frame variation/in-place clearing, incremental safe UTF-8, completion without duplicate answer, plain fallback and second-turn operation.
- [x] Add capability detection and RAII restoration, stateful incremental sanitizer (8192 rendered bytes/message), and single-writer presenter.
- [x] Run synchronous turns on one joinable worker; main thread refreshes at about 10 fps and consumes bounded/coalesced events; independent completion channel covers callback/persistence failures. Close queue before joining on UI failure.
- [x] Add optional presented delegates and wire them in main; default interactive stream on, noninteractive unchanged. Add --ui auto|plain and --stream auto|off startup options.
- [x] Clear animated line before appended text; stop animation during partial lines; show tool/phase/elapsed states; provisional failure marker; final answer exactly once.
- [x] Route existing compaction/RAG diagnostics through presenter during active turn. Keep internal model responses out of transcript.
- [x] Parent runs CLI/process/presenter tests and Windows terminal smoke; fix failures.

## Task 4: Integration, review and delivery (parent)

- [x] Baseline: `ctest --test-dir build/vs2022 -C Release --output-on-failure -j 4` on original checkout. Observed 47/48; preexisting reproc_jsonl_process startup assertion fails.
- [x] Configure isolated feature build using already present pinned dependency sources; register new source/test files in CMake. No global installs.
- [x] Build Release and run focused tests after each component, then all CTest entries. Record all failures, distinguishing baseline.
- [x] Independent review of streaming/protocol and terminal/runtime boundaries; fix and rerun relevant tests.
- [x] Exercise a loopback delayed provider through the executable with two turns; verify first bytes before server completion, cancellation, no duplicate text and plain output.
- [x] Build a runnable Release result, preserve local configuration, verify Ready package contract, and integrate the reviewed feature into the user's checkout without overwriting new unrelated edits.
- [x] Report exact executable, use, tests and unverified live-provider limitations. Keep plan and evidence locally.

## Progress ledger

- Baseline 2026-09-14: original worktree 00a9af4; 47/48 existing tests pass; reproc_jsonl_process_preserves_unicode_and_reuses_one_child fails at started.has_value(). No source changes at baseline.
- Ruling: run independent model/runtime/CLI file ownership in parallel and centralize build/CMake coordination. This follows active parallel delegation and avoids overlapping writes; interface changes require coordination.
- Ruling: use existing pinned dependency sources for isolated build, then integrate into the original checkout for a usable user result. No dependency upgrades or global installation.

- Final feature verification: Release build succeeded; 50/50 CTest entries passed (117.50s). Existing pytest venv selected explicitly; no dependency install. Independent reviews and observed MiniMax input-token accounting regression resolved.
- PTY verified rotation, Chinese/emoji, tool completion, cancellation both before headers and after partial text, then successful next turns. Current MiniMax-M3 synthetic streaming request completed and persisted successfully in 2.0s.
- Delivery: b37dc2b fast-forwarded into original main; original Release rebuild succeeded and 9/9 focused checks passed. Ready package verification passed with unchanged .env hash; packaged EXE live streaming smoke succeeded in 1.922s. Evidence copied into original out/dynamic-cli-validation and loopback stopped.
