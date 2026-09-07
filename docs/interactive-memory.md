# Interactive sessions, compaction, and long-term memory

AgentFramework is a local-first C++17 interactive coding-agent runtime. Conversation records and memories are local JSONL files; the configured Anthropic-compatible provider is used only for model work.

## Start it

Start `AgentFramework.exe` with no arguments for normal interactive use; a double-click does the same. The executable looks for a regular `.env` beside itself first, then in the current working directory. The executable-relative file wins. The selected file is loaded with preserve semantics: an environment variable already set in the process wins over the selected `.env` value. It resumes the newest Session, or asks for a workspace when none exists.

`.env` contains a provider credential. Do not share it, commit it, paste it into chat, or include it in a bug report. Rotate a key if it is exposed.

Source-tree build in **Developer PowerShell for Visual Studio 2022**:

```powershell
Set-Location E:\desktop\How_to_build_a_agent\AgentFramework
cmake -S . -B build\vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build\vs2022 --config Debug
.\build\vs2022\Debug\AgentFramework.exe
```

Build the fixed 2026-09-03 eCFR Knowledge Pack once. This is an explicit,
networked build step; normal application startup never downloads or rebuilds
the corpus, model, Python runtime, or indexes.

```powershell
Set-Location E:\desktop\How_to_build_a_agent\AgentFramework
powershell -ExecutionPolicy Bypass -File .\scripts\build_ecfr_knowledge_pack.ps1 -KnowledgeRoot 'E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge' -Snapshot '2026-09-03' -DocumentCount 30000
```

The builder verifies the official Python 3.11.9 archive hash, records every
resolved wheel, pins `BAAI/bge-m3` to revision
`5617a9f61b028005a4858fdac845db406aefb181`, produces exactly 30,000 complete
Markdown section documents, builds BM25 plus normalized 1024-dimensional
vectors, verifies all file hashes/SQLite/vector dimensions, atomically
publishes the pack, and updates `active-pack.json`. An interrupted run resumes
only a staging tree whose build intent and runtime lock are identical; it never
deletes an existing published pack.

Build and start the Release Ready package:

```powershell
Set-Location E:\desktop\How_to_build_a_agent\AgentFramework
cmake -S . -B build\vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build\vs2022 --config Release --target agent_ready_package_verify
.\out\AgentFramework-Ready\AgentFramework.exe
```

The Ready package is intentionally and verifiably four files:
`AgentFramework.exe`, `cpr.dll`, `libcurl.dll`, and `.env`. The external
Knowledge Pack, corpus, model, Python runtime, indexes, reports, sessions,
tasks, memories, logs, and caches are never copied into Ready. Its packaged
`.env` supplies defaults such as `AGENT_RUNTIME_ROOT`, but a pre-existing
process environment value overrides it; do not copy real runtime data into the
package simply to make it look pre-populated.

With `AGENT_ENABLE_RAG=1`, set `AGENT_RAG_PACK_ROOT` to the published absolute
pack path, or omit that variable and let a double-click launch read only
`..\AgentFramework-Knowledge\active-pack.json` relative to the EXE directory.
Discovery never depends on the current working directory. After the explicit
build has completed, retrieval and model embedding run locally and can work
offline; MiniMax answers still require provider network access.

Rebuilding the Ready package overwrites only those four managed regular files. If the Ready directory contains `runtime_data` or any other unmanaged entry, staging refuses to continue and preserves the directory unchanged. Move the data elsewhere or select an external `AGENT_RUNTIME_ROOT` before rebuilding; this prevents a package rebuild from deleting local history.

## Local state and evidence

The default runtime root is `runtime_data`, relative to the process working directory. `AGENT_RUNTIME_ROOT` may set another nonempty path. For a double-clickable local installation that must see the same Sessions and memories regardless of its launch directory, configure an absolute runtime root outside the Ready package; the local packaged configuration should point at the existing durable data directory rather than copying that data into Ready.

| Layer | Purpose | Local location |
| --- | --- | --- |
| Workspace | The selected directory; it scopes workspace memories and tool work. | Referenced by records; it is not copied into runtime data. |
| Session | One interactive conversation, with exact event history and a bounded active projection. | `runtime_data/sessions/<session-id>/events.jsonl` |
| Task | One model/tool execution inside a Session. | `runtime_data/tasks/<task-id>/events.jsonl` |
| Session summary | Bounded cumulative summary of an older committed prefix. | A `session_compacted` event in the Session JSONL. |
| Long-term memory | Durable preferences, decisions, facts, workflows, and constraints. | `runtime_data/memories/events.jsonl` |

Session and Task JSONL evidence stays append-only. Compaction only removes old messages from the active projection after a validated event; it does not erase committed evidence needed for audit, replay, or consolidation. Forgetting appends a tombstone rather than rewriting prior memory events.

When a Session is created, its workspace identity is converted to an absolute, lexically normalized path, weakly canonicalized when possible, and ASCII-case-folded on Windows. This gives equivalent path spellings one durable memory scope. Legacy Session records with relative workspaces are refused when loaded because their tool target could otherwise change with the process working directory.

## Commands

| Command | Effect | Example |
| --- | --- | --- |
| `/memories` | List active global and matching-workspace memories. | `/memories` |
| `/remember <text>` | Store one explicit `fact` after safety validation. | `/remember Prefer C++17 examples.` |
| `/forget <memory-id>` | Append a tombstone for an active memory. | `/forget memory-0123abcd0123abcd0123abcd0123abcd` |
| `/memory on` | Enable retrieval and automatic boundary consolidation for this CLI process. | `/memory on` |
| `/memory off` | Disable retrieval and automatic consolidation temporarily; it does not delete memories. | `/memory off` |
| `/memory status` | Show whether memory is on or off. | `/memory status` |
| `/new [workspace]` | Consolidate the current Session when enabled, then create a Session; no argument keeps the workspace. | `/new E:\work\demo` |
| `/clear` | Same boundary behavior as `/new` in the current workspace. | `/clear` |
| `/resume <session-id>` | Validate and load the target first. Only then consolidate the current Session when enabled and switch to the loaded target. | `/resume session-0123abcd0123abcd0123abcd0123abcd` |
| `/status` | Show model, workspace, Session, turns, retained messages, compaction, memory mode, and active-memory count. | `/status` |
| `/exit` | Consolidate the current Session when enabled, then exit. | `/exit` |

EOF has the same boundary-consolidation behavior as `/exit`. An invalid or unloadable `/resume` target does not consolidate or switch away from the current Session. `/memory off` lasts only for this process; explicit `/remember`, `/memories`, and `/forget` still work while the feature is configured. When `AGENT_ENABLE_MEMORY=0`, `/memory status` and `/status` print `Memory: off`; `/memories`, `/remember`, and `/forget` report that memory is unavailable (and `/memory on` cannot enable an unavailable feature).

## Compaction and memory behavior

Before a new turn, the Session engine estimates retained context in bytes. If the threshold is exceeded and more than the retained-turn count exists, it prints `Compacting context...` before the potentially slow model request, then the configured model summarizes the previous summary plus the oldest compactable exact turns. The summary is plain, bounded text and is persisted only after validation; the newest retained turns remain exact.

Below the hard limit, compaction failure is non-destructive: the turn continues with retained context and a visible warning. At or above the hard limit, the new turn is not started, so no pending turn is stranded. This is a byte heuristic, not a provider-tokenizer measurement.

Consolidation runs at `/new`, `/clear`, `/exit`, EOF, and startup catch-up. Catch-up scans durable Sessions before normal model work. It reads original committed Session events, not just the compacted projection. Each model request is bounded to at most 32 committed turns and 32 KiB of serialized transcript, and a checkpoint is appended after every successful prefix. If one turn alone exceeds that source bound, it remains intact in Session evidence, automatic extraction skips it, and the checkpoint advances so startup does not retry it forever; use `/remember` for an important durable fact from such a turn. Model, parsing, or persistence failure does not block switching or exit and does not advance the affected checkpoint, so the bounded batch can be retried.

Retrieval is local and deterministic: active global memories plus entries whose workspace scope exactly equals the current Session workspace. Ranking uses normalized ASCII words, Unicode code points, and adjacent CJK pairs, with bounded top-K/byte output and stable score, update-time, then ID ordering. Retrieved memory is labeled data in a delimited system-prompt section, never instructions.

Turn-local opt-outs include `不要使用记忆`, `不要参考记忆`, `别参考历史`, `忽略之前的记忆`, `do not use memory`, `don't use memory`, `ignore previous memory`, and `ignore memories`. They disable conversational-memory retrieval only for that turn; they neither delete memories nor change later turns. Static RAG, when separately enabled, remains a different evidence system.

Scope is conservative: empty scope is global, otherwise it must exactly match the workspace string. Every automatic model candidate is normalized to the exact current Session workspace, even if the model requested `global`. `/remember` is also always scoped to the current Session workspace. No current runtime path creates a global memory; existing global records can be listed, retrieved, and forgotten by ID. The model has no memory-management tools. Model summaries and extracted candidates are not guaranteed correct, so verify important decisions against source evidence.

## Safety and recovery

Before persistence, memory content must be valid UTF-8, bounded, nonblank, and valid for category/scope/provenance. The runtime rejects the configured credential and strings resembling provider keys, bearer tokens, passwords, or private-key blocks. This is not permission to enter secrets: never put secrets in prompts, `/remember`, workspace files, or docs.

JSONL storage rejects malformed records, duplicate or extra keys, wrong legal JSON shapes, invalid identifiers/sequences, and unsafe linked paths, including linked ancestor directories. An operating-system file lock held across validation and append prevents cooperating concurrent Session writers from both accepting the same next sequence; a competing access may receive a safe busy failure. A corrupt memory store is not silently treated as empty: the command or turn reports a safe persistence failure and leaves existing bytes unchanged. Preserve the affected event file before manual recovery.

## Configuration

Put configuration in the discovered `.env`. `AGENT_BASE_URL`, `AGENT_MODEL`, and exactly one of `AGENT_API_KEY` or `AGENT_AUTH_TOKEN` are required and nonempty. Numeric settings are base-10 positive integers unless a tighter range is shown.

| Setting | Default / requirement | Range or behavior |
| --- | --- | --- |
| `AGENT_BASE_URL` | required | Nonempty Anthropic-compatible base URL. |
| `AGENT_MODEL` | required | Nonempty model name. |
| `AGENT_API_KEY` | exactly one auth mode | Nonempty API-key credential; do not share. |
| `AGENT_AUTH_TOKEN` | exactly one auth mode | Nonempty bearer credential; do not share. |
| `AGENT_MAX_TOKENS` | `4096` | Positive integer representable by the runtime. |
| `AGENT_MAX_MODEL_ROUNDS` | `16` | Positive integer representable by the runtime. |
| `AGENT_MAX_TOOL_CALLS` | `64` | Positive integer representable by the runtime. |
| `AGENT_MAX_TASK_SECONDS` | `1800` | Positive integer whose milliseconds fit the runtime. |
| `AGENT_MODEL_TIMEOUT_SECONDS` | `120` | Positive integer whose milliseconds fit the runtime. |
| `AGENT_ENABLE_BUILD_TOOLS` | `0` | Exactly `0` or `1`. |
| `AGENT_BUILD_TIMEOUT_SECONDS` | `300` | `1..600`. |
| `AGENT_ENABLE_RAG` | `0` (`1` in Ready configuration) | Exactly `0` or `1`. |
| `AGENT_RAG_PACK_ROOT` | EXE-relative active pointer | Optional absolute complete external pack path; never a Ready subdirectory. |
| `AGENT_RAG_MODE` | `hybrid` | `hybrid` or explicit `lexical`; hybrid fails closed if vectors/model are unavailable. |
| `AGENT_RAG_TOP_K` | `6` | `1..20`. |
| `AGENT_RAG_MAX_TOTAL_BYTES` | `32768` | `1..32768` evidence-content bytes. |
| `AGENT_RAG_STARTUP_TIMEOUT_SECONDS` | `120` | `1..600`. |
| `AGENT_RAG_QUERY_TIMEOUT_SECONDS` | `30` | `1..120`. |
| `AGENT_RAG_DEVICE` | `auto` | `auto`, `cuda`, or `cpu`; auto tests CUDA and safely chooses CPU when unavailable. |
| `AGENT_RUNTIME_ROOT` | `runtime_data` | Optional nonempty path. |
| `AGENT_SYSTEM_PROMPT` | `You are a coding agent. Inspect the workspace, make focused edits, and verify the result. Use only the tools explicitly provided.` | Optional; an explicitly empty value is allowed. |
| `AGENT_ENABLE_MEMORY` | `1` | Exactly `0` or `1`; memory off still leaves compaction active. |
| `AGENT_MEMORY_TOP_K` | `5` | `1..20`. |
| `AGENT_MEMORY_MAX_INJECTED_BYTES` | `4096` | `1..1048576`. |
| `AGENT_MEMORY_MAX_ENTRY_BYTES` | `1024` | `1..1048576`. |
| `AGENT_COMPACTION_THRESHOLD_BYTES` | `65536` | `1..16777216`; strictly below the hard limit. |
| `AGENT_COMPACTION_HARD_LIMIT_BYTES` | `131072` | `1..16777216`; strictly above the threshold. |
| `AGENT_COMPACTION_RETAIN_TURNS` | `6` | `1..10000`. |
| `AGENT_COMPACTION_MAX_SUMMARY_BYTES` | `8192` | `1..8192`. |

## Honest limits

- Compaction uses a byte heuristic, not a tokenizer.
- Local lexical retrieval is not semantic/vector recall or reranking; relevant memories can be missed.
- Model summary and extraction quality are not guaranteed.
- Offline unit/process tests prove deterministic scenarios only, not provider behavior, credentials, latency, availability, or production reliability.
- A pasted credential must be rotated even when persistence would reject it.
- The Knowledge Pack is a fixed 2026-09-03 eCFR snapshot, not a live legal
  database. Retrieved text is informational and is not legal advice; verify
  current official text and consult a qualified professional for high-risk use.

## Knowledge Pack verification and recovery

Verify a published pack with its own isolated runtime:

```powershell
& 'E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03\runtime\python.exe' -E -s -X utf8 'E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03\sidecar\agent_rag_cli.py' verify-pack --pack-root 'E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03'
```

`enabled rag requires an external knowledge pack` means neither an explicit
absolute pack nor the EXE-relative pointer was available. Re-run the one build
command above, or restore `active-pack.json` as the exact two-key JSON object
`{"schema_version":1,"pack_root":"<absolute published pack>"}` under
`out\AgentFramework-Knowledge`.

`rag active pack pointer is invalid` means the pointer is malformed, linked, or
names an incomplete/untrusted pack. Preserve the damaged file for diagnosis,
verify the published pack with the command above, then re-run the builder to
replace only the pointer after successful verification. Do not move the Python
runtime, model, corpus, or index into Ready and do not edit `pack.json` by hand.
