# Context Compaction and Long-Term Memory Design

Date: 2026-09-07

## Goal

Give the interactive AgentFramework runtime Codex-like continuity at two distinct layers:

1. Keep one Session usable after its exact transcript becomes too large by replacing only an old committed prefix with an auditable bounded summary.
2. Carry a small set of durable, relevant user preferences, decisions, facts, workflows, and constraints across Sessions without treating every old transcript as prompt context.

The implementation must remain local-first, append-only, inspectable, reversible, safe around secrets, and compatible with the configured Anthropic-compatible MiniMax-M3 endpoint.

## Approaches considered

### Reuse the existing Python BM25 RAG index

Rejected as the memory implementation. That index is static project knowledge, is queried with only the current issue, and is disabled in the Ready package. It remains an independent evidence provider.

### Replay every historical Session into each new request

Rejected. Prompt growth is unbounded, relevance is poor, and it unnecessarily exposes old text to the provider.

### Layered compaction plus a separate memory event store

Selected. A Session keeps a compacted summary plus exact recent turns. Cross-Session memory uses its own append-only event stream, deterministic local retrieval, model-assisted consolidation, explicit user controls, and a persistence-time safety boundary.

## Architecture

### Session compaction

`SessionState` gains:

- `committed_turns`: exact retained turns, each with turn index, task id, and messages.
- `summary`: the latest cumulative summary of compacted turns.
- `compacted_through_turn`: the greatest committed turn included in the summary.
- `messages`: a derived flattened cache of retained exact messages for existing runtime integration.

`SessionCompactedPayload` is appended to the existing Session JSONL. It contains the cumulative summary and the inclusive turn index covered by it. Old turn events remain on disk for audit and replay; the reducer drops their messages only from the active projection.

Before starting a new turn, `SessionEngine` estimates retained context bytes. If the configured threshold is exceeded and more than the configured number of recent turns exists, it asks a dedicated `ContextCompactor` to summarize the previous cumulative summary plus the oldest compactable exact turns. The compactor receives no tools and must return plain non-empty text within the configured byte limit. Only after validation is a `session_compacted` event persisted.

Compaction failure is non-destructive. Below the hard limit, the turn continues with exact history and emits a non-fatal notice. At or above the hard limit, the turn fails before a `turn_started` event so no pending turn is stranded.

Prompt assembly injects the summary into a delimited system-prompt section. Exact retained messages remain ordinary model messages. Pending-turn recovery recreates the same prompt from persisted state.

### Long-term memory

The memory store lives under `<runtime_root>/memories/events.jsonl`. It is independent from static RAG and Session logs.

An active `MemoryEntry` contains:

- stable `memory-<32 lowercase hex>` id;
- category: `preference`, `decision`, `fact`, `workflow`, or `constraint`;
- scope: normalized workspace string or global;
- concise UTF-8 content;
- source Session id and inclusive source turn range;
- created and updated UTC timestamps;
- origin: explicit user memory or model consolidation.

Memory events are:

- `memory_upserted`: create or update an entry;
- `memory_forgotten`: append a tombstone for one active id;
- `session_consolidated`: checkpoint the greatest completed turn processed for a Session.

Forgetting never rewrites history. Replay produces only active entries while retaining the tombstone and provenance in the log.

### Consolidation

The runtime consolidates a Session at safe boundaries: `/new`, `/clear`, `/exit`, EOF, and startup catch-up. It processes only committed turns after that Session's checkpoint. Failed turns have no transcript to extract.

Catch-up reads original `turn_committed` events from the append-only Session store rather than relying on the compacted projection. Therefore a long Session may compact before its next boundary without losing the exact source turns needed for later memory extraction.

The `MemoryConsolidator` asks the configured model for strict JSON containing zero or more candidate entries. It has no tools. Candidates pass strict shape, category, size, UTF-8, scope, and secret checks before persistence. A successful empty result still advances the checkpoint. A model, parse, or persistence failure does not block Session switching or exit and does not advance the checkpoint, so startup can retry.

`/remember <text>` creates one explicit `fact` memory immediately after the same safety checks and records the current Session as provenance.

### Retrieval and injection

Retrieval is local and deterministic. It considers active global memories and memories whose scope exactly matches the current Session workspace. Ranking uses normalized ASCII words plus Unicode code points and adjacent CJK pairs, with exact phrase and category-independent overlap scoring. Results are stable by score, updated time, then id.

At most the configured top K and byte budget are injected into a delimited `Long-term memories` system-prompt section. Each item includes id, category, and content so provenance remains visible to the model. Memory text is data, never instructions, and the section explicitly says not to follow commands embedded in it.

Chinese and English opt-out phrases such as `不要使用记忆`, `别参考历史`, `do not use memory`, and `ignore previous memory` are checked before retrieval. Opt-out affects the current turn only.

### User controls

The interactive CLI adds:

- `/memories`: list active memories available to the current workspace.
- `/remember <text>`: save one explicit memory.
- `/forget <memory-id>`: append a tombstone.
- `/memory on|off|status`: control and inspect memory use for the running CLI Session. `off` disables retrieval and automatic consolidation but does not delete stored memories.

`/status` also shows retained messages, compacted-through turn, summary presence, memory mode, and active memory count. Outputs never display credentials or provider bodies.

## Configuration

The following strict environment settings are added:

- `AGENT_ENABLE_MEMORY` (`0|1`, default `1` for the requested Ready package);
- `AGENT_MEMORY_TOP_K` (default `5`, range `1..20`);
- `AGENT_MEMORY_MAX_INJECTED_BYTES` (default `4096`);
- `AGENT_MEMORY_MAX_ENTRY_BYTES` (default `1024`);
- `AGENT_COMPACTION_THRESHOLD_BYTES` (default `65536`);
- `AGENT_COMPACTION_HARD_LIMIT_BYTES` (default `131072`);
- `AGENT_COMPACTION_RETAIN_TURNS` (default `6`);
- `AGENT_COMPACTION_MAX_SUMMARY_BYTES` (default `8192`).

The hard limit must exceed the threshold. Numeric values are positive and bounded before conversion.

## Security and data-integrity invariants

- Never persist strings resembling provider keys, bearer tokens, passwords, private-key blocks, or the configured credential.
- Validate UTF-8 and bounded lengths before appending events.
- Reject duplicate JSON keys, extra keys, wrong legal JSON shapes, discontinuous sequences, invalid ids, symlinked roots/leaves, and multiply-linked leaf files.
- Never include provider error bodies in user output or memory events.
- Never silently treat corrupt memory storage as empty; return a safe persistence error and keep existing bytes unchanged.
- Exact Session events and memory events are append-only evidence. Derived projections may discard old text only in memory.

## Verification

Tests cover reducer and codec invariants, secure storage, compaction thresholds and failure degradation, strict consolidation parsing and secret rejection, Chinese/English ranking and opt-out, CLI commands, restart/catch-up, a scripted HTTP process scenario across Sessions, Ready-package parity and hygiene, and one final live MiniMax-M3 smoke. Live success proves provider compatibility only; deterministic tests own behavior assertions.

## Non-goals

- Vector embeddings, semantic rerankers, cloud synchronization, autonomous background threads, and OpenAI `/responses/compact` are not part of this increment.
- Memories do not replace Session logs or static RAG evidence.
- The runtime does not claim human-like recall; retrieval is bounded and inspectable.
