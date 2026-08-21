# Python RAG Sidecar Design

Date: 2026-08-21

Status: approved under the user's standing authorization

## 1. Goal

Add a real, local knowledge base to the existing single Coding Agent without
moving orchestration out of the C++17 Runtime. The Runtime continues to own the
task state machine, budgets, durable events, model calls, tools, and terminal
status. Python owns only document ingestion, chunking, indexing, ranking, and
the query protocol behind the existing `KnowledgeProvider` port.

This milestone must be useful in the eventual real workflow:

1. an operator builds a knowledge database from trusted local documentation;
2. `agent run` retrieves relevant, source-addressable chunks before each model
   round;
3. the C++ Runtime validates and durably records the evidence;
4. the Provider receives the evidence as explicitly untrusted reference data;
5. a failed or malformed sidecar enters the existing direct `Failed` state
   through `ContextPreparationFailed`, with a fixed reason.

## 2. Non-goals

This milestone does not add:

- an HTTP service, daemon, socket listener, or background worker;
- embeddings, model downloads, a vector database, or network access;
- a model-visible RAG/indexing tool;
- arbitrary Python/program/argv execution chosen by the model;
- changes to the task-state vocabulary or an additional failure state;
- automatic retry, crash recovery, evaluation, Git mutation, or multi-agent
  behavior;
- indexing of known secret stores or conservatively named secret-bearing files,
  binary files, links/reparse points, `.git`, `.agent`, `.rag`, build output, or
  runtime logs. V1 is not a content secret scanner, so the source tree remains
  operator-curated and trusted.

Embedding retrieval can be added later behind the same Python output contract.
The deterministic lexical implementation is deliberately the first production
retriever because it is offline, inspectable, dependency-free, and testable on
Windows and Linux.

## 3. Alternatives considered

### 3.1 Implement retrieval in C++

Rejected. It would erase the already chosen C++ Runtime/Python knowledge
boundary and make future NLP/index experiments expensive.

### 3.2 Run a local Python HTTP server

Rejected for V1. It adds service lifecycle, port allocation, authentication,
stale-process recovery, and another network boundary without improving one
foreground task.

### 3.3 Use an embedding model and vector database immediately

Rejected for the first safe implementation. It would require heavyweight
dependencies or model downloads, introduce nondeterministic versioned assets,
and make ordinary tests dependent on hardware and external packages.

### 3.4 Chosen: one-shot Python process with SQLite/BM25

The C++ adapter launches a configured Python interpreter with adapter-owned
arguments, sends one bounded JSON request on stdin, reads one bounded JSON
response on stdout, validates it, and lets the process exit. SQLite is used as
the persistent knowledge database; BM25 ranking is implemented with Python's
standard library.

## 4. Architecture

```text
CLI / composition root
        |
        v
C++ RuntimeEngine -----> KnowledgeProvider port
                              |
                              v
                    PythonRagKnowledgeProvider
                              |
                     DirectProcessRunner
                              |
                              v
             python -E -s agent_rag_cli.py query
                              |
                              v
               read-only SQLite knowledge index
```

The model never sees the Python executable, script path, index path, process
arguments, or environment. The direct process runner remains an internal port,
not a model tool. Provider credentials are excluded by its existing environment
allowlist and secret-name filter.

`verify-log` remains dispatched before configuration and composes no Python,
process runner, knowledge adapter, HTTP transport, or Provider.

## 5. Python package and commands

Tracked files live under `rag/`:

```text
rag/agent_rag_cli.py
rag/agent_rag/__init__.py
rag/agent_rag/protocol.py
rag/agent_rag/indexer.py
rag/agent_rag/retriever.py
rag/agent_rag/cli.py
rag/tests/test_agent_rag.py
```

The wrapper is invoked with `python -E -s` so Python-specific environment
variables and the user site-packages directory are ignored while the bundled
package remains importable from the wrapper directory.

### 5.1 Build command

```text
python -E -s -X utf8 rag/agent_rag_cli.py build \
  --source <trusted-document-root> \
  --index <knowledge.sqlite>
```

The build command performs a complete deterministic rebuild into a temporary
sibling database and atomically replaces the destination only after successful
commit and validation. A failed build leaves the previous database intact.
Stdout contains one bounded JSON summary; stderr contains only fixed errors.

### 5.2 Query command

```text
python -E -s -X utf8 rag/agent_rag_cli.py query --index <knowledge.sqlite>
```

The query command reads exactly one JSON object from stdin and writes exactly
one JSON object to stdout. The index is opened read-only. The query path rejects
link/reparse-point indexes before opening; this is a deterministic safety check,
not a hostile-concurrent-process sandbox.

## 6. Document ingestion

The first indexer accepts strict UTF-8 regular files with these source forms:

- documentation: `.md`, `.txt`, `.rst`, `.adoc`;
- C/C++: `.c`, `.cc`, `.cpp`, `.cxx`, `.h`, `.hh`, `.hpp`, `.hxx`;
- Python: `.py`;
- build description: `.cmake` and exact `CMakeLists.txt`.

The traversal is lexically sorted and never follows a symlink/reparse point or
accepts a multiply linked regular file.
It skips protected components, hidden runtime/index directories, common build
directories, and filenames whose stem contains a `.`, `-`, or `_` separated
`secret`, `secrets`, `credential`, `credentials`, `password`, `passwords`,
`passwd`, `token`, or `tokens` token. It also skips unsupported extensions,
files larger than 1 MiB, invalid UTF-8, and NUL-containing files. It stops at
fixed corpus budgets: 50,000 inspected entries, 10,000 accepted files, and 64
MiB accepted source bytes. Reaching a
budget is a build error, not silent partial success.

Files are divided on line boundaries into chunks of at most 4,096 UTF-8 bytes,
with up to three lines of overlap. A single oversized line is split at UTF-8
code-point boundaries. Empty/whitespace-only chunks are omitted. Each chunk has
a stable source ID:

```text
relative/posix/path.md#L<start>-L<end>
```

When one physical line must be split, its chunks append `-P<part>` (starting at
1), for example `generated.txt#L7-L7-P2`; ordinary line-range IDs remain
unchanged.

The database stores the relative path, line interval, text, UTF-8 byte length,
SHA-256 of the chunk, token count, and term frequencies. It never stores the
absolute source root.

## 7. Tokenization and ranking

Input is Unicode NFKC-normalized and case-folded. The tokenizer emits:

- alphanumeric/underscore identifier tokens;
- camel/snake identifier segments;
- individual CJK characters;
- overlapping CJK bigrams.

This gives deterministic useful matching for English, code identifiers, and
Chinese documentation without an external segmenter. Query terms are deduped
and bounded. Ranking uses BM25 with fixed `k1=1.5` and `b=0.75`; ties are broken
by source ID. No timestamps or randomness participate in ranking.

## 8. Query protocol

The C++ adapter sends:

```json
{
  "schema_version": 1,
  "query": "the durable task issue",
  "top_k": 5,
  "max_total_bytes": 32768
}
```

Only the durable Issue is used as the retrieval query in V1. Tool output and
model text do not influence retrieval, which avoids an unnecessary feedback
channel and keeps replay explanations simple.

The Python sidecar returns:

```json
{
  "schema_version": 1,
  "items": [
    {
      "source_id": "guide.md#L10-L24",
      "content": "...",
      "metadata": {
        "path": "guide.md",
        "start_line": 10,
        "end_line": 24,
        "sha256": "lowercase hex",
        "score": 3.125
      }
    }
  ]
}
```

Keys are exact. Items are ordered by descending score then source ID. The
response contains at most `top_k` items and at most 32 KiB of evidence content.
No absolute path, environment value, traceback, SQL error, or source text is
written to stderr on failure.

## 9. C++ adapter validation

`PythonRagKnowledgeProvider` accepts a `ProcessRunner` and immutable config. It
owns no process and starts one per `retrieve` call. It validates before launch:

- nonempty strict-UTF-8 Issue, at most 16 KiB;
- configured top-k in `1..20` and timeout in `1..60` seconds;
- absolute/canonical regular script and index paths without link components;
- a nonempty strict-UTF-8 Python program name.

It launches only:

```text
<python> -E -s -X utf8 <script> query --index <index>
```

with the JSON request on stdin, script directory as cwd, 64 KiB stdout, 4 KiB
stderr, and the configured timeout. Nonzero exit, timeout, runner failure,
truncated stdout, malformed JSON, duplicate source IDs, bad metadata, and all
bound violations become fixed `RuntimeError` values. Raw stderr never enters an
event, terminal output, or model request.

For this Python protocol, metadata has exactly `path`, `start_line`, `end_line`,
`sha256`, and `score`. The adapter requires a canonical relative POSIX path,
positive ordered line numbers, a finite nonnegative score, and a 64-character
lowercase SHA-256 equal to the returned content. `source_id` must be the exact
`path#Lx-Ly` form, or `path#Lx-Lx-Pn` with a positive part number for a split
physical line.

The adapter maps valid items to the existing `EvidencePack` without flattening
them into a conversation message. The Anthropic adapter already labels the
serialized evidence as untrusted reference data.

## 10. Domain and replay invariants

A provider-neutral `evidence_pack_is_valid` invariant is enforced at three
boundaries:

1. the Python adapter before returning;
2. `RuntimeEngine` before `ContextPrepared` is appended;
3. `StateReducer` while applying/replaying `ContextPrepared`.

The invariant permits an empty pack and arbitrary bounded `Value` metadata,
but otherwise requires:

- at most 20 items;
- unique source IDs, each 1..512 UTF-8 bytes without controls;
- nonempty content, each at most 8 KiB and total at most 32 KiB;
- metadata at most 16 levels, 256 nodes, and 4 KiB of keys/string values per
  item; all doubles finite and all strings strict UTF-8 without NUL.

When applying `ModelCallStarted`, the Reducer revalidates the nested request
EvidencePack and requires it to equal the immediately prepared durable context.
This binds the audit record to the evidence actually submitted to the model.

A faulty knowledge provider therefore appends exactly one
`ContextPreparationFailed` event and enters the existing `Failed` state. A
forged JSONL `ContextPrepared` payload is rejected during replay. No new failure
state or trailing `TaskFailed` event is added.

## 11. Configuration and composition

New environment settings:

- `AGENT_ENABLE_RAG=0|1`, default `0`;
- `AGENT_RAG_PYTHON`, default `python`;
- `AGENT_RAG_SCRIPT`, required and nonempty when enabled;
- `AGENT_RAG_INDEX`, required and nonempty when enabled;
- `AGENT_RAG_TOP_K`, integer `1..20`, default `5`;
- `AGENT_RAG_TIMEOUT_SECONDS`, integer `1..60`, default `10`.

When disabled, `main.cpp` composes `EmptyKnowledgeProvider` and does not inspect
the script/index filesystem or start Python. When enabled, it composes
`PythonRagKnowledgeProvider` with the existing `DirectProcessRunner`.

The workspace path policy adds `.rag` to protected components so the model's
file tools cannot list, read, create, or overwrite a workspace-local index.

## 12. Test and acceptance strategy

All ordinary tests are offline.

Python tests cover deterministic Unicode/CJK retrieval, code identifiers,
stable source IDs, rebuild replacement, stale-chunk removal, bounds, protected
paths, link refusal, malformed protocol, read-only query, and fixed failures.

C++ fake-runner tests cover the exact process request, literal stdin query,
ordered mapping, metadata, every process failure class, JSON/schema violations,
duplicates, UTF-8 and size budgets, and raw-error suppression.

Engine/Reducer/JSONL tests cover faulty/forged evidence at all durable
boundaries and preserve direct `Failed` semantics. A real integration test uses
the configured Python interpreter to build a temporary Unicode corpus, query it
through `DirectProcessRunner` and `PythonRagKnowledgeProvider`, pass the evidence
to a fake model, persist it, and replay the same state.

Final acceptance requires:

- fresh MSVC Debug configure/build;
- all C++ offline CTest targets;
- Python unit tests through CTest when an interpreter is available;
- the real Python/C++ RAG integration;
- credential-free `run` and `verify-log` behavior unchanged;
- RAG-disabled composition starting no Python;
- no live Provider, network request, package install, or GitHub operation;
- independent code review with no unresolved Critical or Important finding;
- local branch, tracking ref, and E-drive bare-backup SHA equality.
