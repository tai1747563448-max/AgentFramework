# Python RAG Sidecar Implementation Plan

> Apply brainstorming, test-driven-development, systematic-debugging,
> verification-before-completion, requesting-code-review, and
> receiving-code-review throughout this plan.

**Goal:** Add a deterministic offline Python knowledge base behind the existing
C++17 `KnowledgeProvider`, with durable validated evidence and no change to the
single-agent state vocabulary.

**Architecture:** Python builds and queries a read-only SQLite/BM25 index. C++
launches one bounded Python process per retrieval through the internal direct
process runner, validates exact JSON, and returns the existing `EvidencePack`.
RuntimeEngine remains the only orchestrator.

**Base:** `main` at `1b6c8a4399e3e7a5f163a79acc0f57d6c2077085`

**Branch/worktree:** `feat/python-rag-sidecar` in
`.worktrees/python-rag-sidecar`

## Global constraints

- C++17 Runtime; Python standard library only.
- One Agent, one foreground task, sequential tool calls.
- No HTTP service, embeddings, vector service, model download, package install,
  Git tool, network tool, shell tool, or multi-agent work.
- `ContextPreparationFailed` enters the existing `Failed` state directly.
- All ordinary verification is offline. Do not run a credentialed Provider.
- Build/query error output is fixed and cannot disclose SQL text, paths,
  source content, environment, or traceback.
- Every milestone commit is pushed only to `E:\GitBackups\AgentFramework.git`
  and all local/tracking/bare SHA values are verified.

## Task 1: Lock the design

**Files**

- Add `docs/superpowers/specs/2026-08-21-python-rag-sidecar-design.md`
- Add `docs/superpowers/plans/2026-08-21-python-rag-sidecar.md`

**Checks**

- Confirm the only process protocol is one-shot stdin/stdout JSON.
- Confirm retrieval uses the durable Issue only.
- Confirm `.rag` is reserved and no model-visible tool is added.
- Confirm lexical BM25 is a real first RAG implementation and embeddings are a
  future compatible replacement, not hidden scope.

**Commit**

```text
docs: design python rag sidecar
```

## Task 2: Build the deterministic Python index and query engine

**Files**

- Add `rag/agent_rag/__init__.py`
- Add `rag/agent_rag/protocol.py`
- Add `rag/agent_rag/indexer.py`
- Add `rag/agent_rag/retriever.py`
- Add `rag/agent_rag/cli.py`
- Add `rag/agent_rag_cli.py`
- Add `rag/tests/test_agent_rag.py`
- Modify `CMakeLists.txt`

### RED

Write Python tests first for:

- Unicode English/Chinese/code tokenization;
- deterministic source ordering and line-based stable source IDs;
- `.git`, `.agent`, `.rag`, build, secret, binary, invalid UTF-8, oversized,
  file-link, and directory-link exclusion;
- maximum file/corpus/chunk bounds;
- query protocol exact keys and integer bounds;
- BM25 relevance, deterministic tie order, top-k, and total-byte cap;
- complete rebuild removing stale chunks;
- failed rebuild preserving an existing valid database;
- link/reparse index refusal and read-only query;
- fixed CLI errors with no traceback/path/source leak.

Run:

```powershell
python -E -s -m unittest discover -s rag/tests -v
```

Expected RED: import/module failures because production files do not exist.

### GREEN

Implement only the standard-library package:

- NFKC/casefold hybrid tokenizer;
- bounded deterministic no-link traversal;
- strict UTF-8 line chunks with three-line overlap and oversized-line splitting;
- SQLite schema/version/meta/chunks/postings tables and indexes;
- sibling temporary build plus atomic replace;
- read-only BM25 query and exact JSON protocol;
- fixed CLI exit messages.

Register the Python suite in CTest only when `Python3::Interpreter` is found;
do not make Python mandatory for a RAG-disabled C++ build.

Re-run the focused Python suite twice. Run `git diff --check`.

**Commit**

```text
feat: add deterministic python knowledge index
```

## Task 3: Enforce durable EvidencePack invariants

**Files**

- Add `src/domain/evidence_validation.h`
- Add `src/domain/evidence_validation.cpp`
- Add `tests/domain/evidence_validation_test.cpp`
- Modify `src/application/runtime_engine.cpp`
- Modify `src/application/state_reducer.cpp`
- Modify `tests/application/runtime_engine_test.cpp`
- Modify `tests/application/state_reducer_test.cpp`
- Modify `tests/adapters/jsonl_event_store_test.cpp`
- Modify `CMakeLists.txt`

### RED

Write focused tests that require:

- empty and normal packs are accepted;
- more than 20 items, duplicate/empty/control/oversized source IDs, empty or
  oversized content, total content over 32 KiB, invalid UTF-8/NUL, nonfinite
  metadata doubles, metadata depth/nodes/string budget overflow are rejected;
- a Fake KnowledgeProvider returning bad evidence appends exactly one
  `ContextPreparationFailed`, enters `Failed`, and never calls the model;
- Reducer rejects the same bad `ContextPrepared` payload;
- forged JSONL with bad evidence fails read/replay.

Build the new/modified focused targets. Expected RED: missing validation API or
current Engine/Reducer acceptance.

### GREEN

Implement a dependency-free recursive validator and enforce it in Engine and
Reducer before evidence becomes durable state. Preserve arbitrary bounded
metadata shapes and all existing valid evidence fixtures.

Run domain, reducer, engine, JSONL, and Anthropic adapter tests.

**Commit**

```text
feat: validate durable rag evidence
```

## Task 4: Add the C++ PythonRagKnowledgeProvider

**Files**

- Add `src/adapters/rag/python_rag_knowledge_provider.h`
- Add `src/adapters/rag/python_rag_knowledge_provider.cpp`
- Add `tests/adapters/python_rag_knowledge_provider_test.cpp`
- Modify `CMakeLists.txt`

### RED

Use a Fake ProcessRunner and real temporary script/index files. Test first:

- exact program and `-E -s -X utf8 <script> query --index <index>` arguments;
- script parent cwd, literal Issue only in stdin, top-k/schema/byte cap;
- strict ordered conversion of all evidence fields and metadata;
- runner failure, timeout, nonzero exit, stdout truncation, malformed JSON,
  extra/missing keys, bad version/type/bounds, duplicate IDs, invalid UTF-8,
  and bad metadata;
- link/reparse script and index refusal where privileges permit;
- raw runner stderr/error text never appears in returned RuntimeError.

Expected RED: missing adapter header.

### GREEN

Implement exact request creation, stable canonical-file checks, fixed error
mapping, exact JSON decoder, and the same EvidencePack invariant. Do not parse
or expose stderr.

Run the focused adapter target on Windows and compile/run it with WSL/g++ when
the available environment can do so without installing dependencies.

**Commit**

```text
feat: bridge cpp runtime to python rag
```

## Task 5: Prove a real Python/C++ retrieval workflow

**Files**

- Add `tests/integration/rag_integration_test.cpp`
- Modify `CMakeLists.txt`

### RED

When Python is available, write an integration test that:

1. creates a Unicode trusted corpus and separate workspace/runtime root;
2. invokes the real Python build command through `DirectProcessRunner`;
3. queries through the real `PythonRagKnowledgeProvider`;
4. proves Chinese/code-relevant evidence, stable source IDs, and no absolute
   source path;
5. runs a real RuntimeEngine with a Fake Model;
6. proves the model receives the structured evidence;
7. proves JSONL persistence and replay reproduce the same evidence;
8. proves a broken index becomes direct `ContextPreparationFailed -> Failed`.

Expected RED: missing integration wiring or behavior.

### GREEN

Add only the CMake definitions/dependencies needed to run the real sidecar.
Run the integration twice to prove rebuilding/query lifecycle stability.

**Commit**

```text
test: prove real python rag workflow
```

## Task 6: Wire opt-in production composition and protect `.rag`

**Files**

- Modify `src/config/runtime_config.h`
- Modify `src/config/runtime_config.cpp`
- Modify `src/main.cpp`
- Modify `src/adapters/workspace/workspace_path_policy.cpp`
- Modify `tests/cli/cli_app_test.cpp`
- Modify `tests/adapters/workspace_primitives_test.cpp`
- Modify `tests/integration/runtime_integration_test.cpp`
- Modify `.env.example`
- Modify `README.md`

### RED

Add tests first for:

- default RAG disabled and no Python configuration required;
- exact `0|1` flag;
- enabled mode requires nonempty script/index;
- Python default and explicit Unicode values;
- top-k `1..20`, timeout seconds `1..60`, overflow and malformed values;
- `.rag` excluded/refused by all workspace file operations;
- RAG-disabled production composition retains empty evidence and starts no
  process;
- enabled composition exposes exactly the same five/eight tools; RAG is not a
  tool.

Expected RED: missing config fields or `.rag` currently accessible.

### GREEN

Compose either `EmptyKnowledgeProvider` or `PythonRagKnowledgeProvider` through
one owning `std::unique_ptr<KnowledgeProvider>`. Keep `verify-log` before all
configuration. Update docs with build/query/run commands, trust boundaries,
metadata/source citation behavior, and explicit lexical-vs-embedding scope.

**Commit**

```text
feat: wire opt-in python rag sidecar
```

## Task 7: Whole-milestone verification, review, and local merge

**Files**

- Add `docs/superpowers/reports/2026-08-21-python-rag-sidecar-review.md`
- Modify only finding-owned files if review requires fixes

### Verification before review

Use a fresh disconnected MSVC build directory with cached pinned dependency
sources. Run:

- clean full Debug build;
- all CTest targets with output on failure;
- Python suite directly and through CTest;
- RAG integration twice;
- credential-free `run` and `verify-log`;
- `ctest -N` and confirm no live test;
- fixed-range `git diff --check`;
- production definition scan proving exactly five default and eight build-opt-in
  tools, with no RAG/shell/program/argv tool;
- secret/traceback/path scans over process errors and docs;
- tracked-scope and clean-worktree checks.

No live Provider or network call is run.

### Independent review

Request one read-only review of the fixed commit range. Review protocol parsing,
SQLite/path safety, bounds, deterministic ranking, Event/Reducer causality,
configuration defaults, raw-error suppression, and scope. Resolve every
Critical/Important finding through its own RED -> GREEN fix round and request a
small re-review until `Ready: Yes`.

### Merge and backup

- Push every feature milestone to local `backup/feat/python-rag-sidecar`.
- Verify local/tracking/bare/`ls-remote` SHA equality.
- Merge with `--no-ff` into local `main` only after `Ready: Yes`.
- Repeat fresh merged build, CTest, Python, integration, CLI, registration, and
  diff checks.
- Push `main` to the E-drive bare backup and verify all SHAs.
- Remove the clean merged worktree; retain the bare feature branch as a local
  milestone record.

## Out of this plan

Recovery/resume semantics, evaluation harnesses, the final real Issue scenario,
and the complete Chinese code manual remain subsequent milestones of the
already active single-Agent goal. They must not be claimed complete here.
