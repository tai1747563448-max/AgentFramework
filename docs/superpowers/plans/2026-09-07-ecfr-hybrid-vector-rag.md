# eCFR Hybrid Vector RAG Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a double-clickable AgentFramework that retrieves from a reproducible 30,000-document eCFR Knowledge Pack using local BGE-M3 dense vectors plus BM25, injects bounded evidence into MiniMax-M3, and proves positive, negative, damaged-asset, retrieval-quality, and live-provider behavior.

**Architecture:** A Python build pipeline downloads 49 official eCFR title XML files, emits exactly 30,000 complete section Markdown files, chunks them structurally, and publishes an integrity-checked external Knowledge Pack containing SQLite BM25 metadata and a normalized 1024-dimensional float16 matrix. A persistent Python JSONL sidecar loads the model and pack once; a strict C++ provider talks to it through a pinned reproc++ transport and returns the existing `EvidencePack` boundary to `RuntimeEngine`.

**Tech Stack:** C++17, CMake 3.21+, reproc++ v14.2.7, Python 3.11.9 x64 embeddable archive SHA-256 `009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b`, pytest, defusedxml 0.7.1, NumPy 2.4.6, PyTorch 2.11.0+cu130, sentence-transformers 6.0.1, SQLite, BAAI/bge-m3 revision `5617a9f61b028005a4858fdac845db406aefb181`, Anthropic-compatible MiniMax-M3.

**Spec:** `docs/superpowers/specs/2026-09-07-ecfr-hybrid-vector-rag-design.md`

## Global Constraints

- Target root is `E:\desktop\How_to_build_a_agent\AgentFramework`; the existing dirty worktree is user-owned and must not be reset, cleaned, or broadly staged.
- Before editing an already-modified file, capture its current diff; commits stage only named task-owned paths or exact hunks and never sweep unrelated WIP.
- The first published corpus snapshot is `2026-09-03`, uses the 49 non-reserved eCFR titles, and contains exactly 30,000 complete non-reserved section Markdown files.
- `BAAI/bge-m3` is local and pinned to revision `5617a9f61b028005a4858fdac845db406aefb181`; dense vectors are real, L2-normalized, 1024-dimensional little-endian float16 rows.
- Chunking uses at most 768 BGE-M3 tokens and at most 96 tokens of overlap; chunks never cross a Markdown document.
- Hybrid retrieval fuses BM25 top 100 and dense top 100 using RRF constant 60, returns 1..20 evidence items, defaults to 6, and never exceeds 32,768 content bytes.
- Hybrid mode fails closed on missing, corrupt, stale, or wrong-shape model/index assets; lexical-only retrieval requires explicit `AGENT_RAG_MODE=lexical`.
- The existing Ready directory remains the EXE, `.env`, and required DLLs only; corpus, model, Python runtime, indexes, and reports live in the external Knowledge Pack.
- Normal application startup performs no download, model update, corpus conversion, embedding build, or index rebuild.
- All production behavior changes follow RED-GREEN-REFACTOR; each test must be observed failing for the intended reason before implementation.
- Do not print, persist, stage, or inspect the user's MiniMax credential value; live tests load it through existing configuration without echoing environment or provider bodies.
- No completion claim is allowed until the hardening round is strict-valid, Debug and Release suites pass, the 30,000-file pack verifies, retrieval metrics exist, double-click-equivalent startup passes, and a fresh MiniMax-M3 smoke succeeds.

## File Map

### Python corpus and pack pipeline

- Create `rag/agent_rag/ecfr_source.py`: official URL construction, bounded download, response manifest, retry policy.
- Create `rag/agent_rag/ecfr_document.py`: immutable section/document records, XML traversal, Markdown serialization, stable selection.
- Create `rag/agent_rag/pack.py`: strict pack paths, manifests, hashing, atomic publish, complete-pack verification.
- Create `rag/agent_rag/chunker.py`: tokenizer-aware structural chunks and stable chunk IDs.
- Create `rag/agent_rag/embedding.py`: pinned BGE-M3 adapter and deterministic test adapter.
- Create `rag/agent_rag/hybrid_index.py`: schema-v2 SQLite plus vector matrix construction and incremental reuse.
- Create `rag/agent_rag/hybrid_retriever.py`: BM25, exact dense search, RRF, dedupe, neighbor expansion, budgets.
- Create `rag/agent_rag/sidecar.py`: strict JSONL protocol-v2 server and lifecycle.
- Create `rag/agent_rag/evaluation.py`: annotated case validation, metrics, latency and comparison report.
- Modify `rag/agent_rag/cli.py`: `download-ecfr`, `build-corpus`, `build-index`, `verify-pack`, `serve`, and `evaluate` commands.
- Modify `rag/agent_rag/protocol.py`: exact-key protocol-v2 request/response parsing while retaining v1 compatibility entrypoints for old tests.
- Create `rag/requirements-rag.lock`: exact Python package versions and indexes.
- Create `scripts/build_ecfr_knowledge_pack.ps1`: reproducible external runtime/model/corpus/index orchestration.

### Python tests and evaluation assets

- Create `rag/tests/test_ecfr_source.py`.
- Create `rag/tests/test_ecfr_document.py`.
- Create `rag/tests/test_pack.py`.
- Create `rag/tests/test_chunker.py`.
- Create `rag/tests/test_embedding.py`.
- Create `rag/tests/test_hybrid_index.py`.
- Create `rag/tests/test_hybrid_retriever.py`.
- Create `rag/tests/test_sidecar.py`.
- Create `rag/tests/test_evaluation.py`.
- Create `rag/eval/ecfr_eval_schema.json` and `rag/eval/negative_cases.jsonl`.
- Modify `rag/tests/test_agent_rag_pytest.py` and `rag/tests/test_agent_rag.py` only where old v1 compatibility must be asserted.

### C++ persistent provider and runtime integration

- Create `src/ports/jsonl_process.h`: long-lived line request/response process seam.
- Create `src/adapters/process/reproc_jsonl_process.h` and `.cpp`: pinned reproc++ child lifecycle, bounds, deadlines, termination.
- Create `src/adapters/rag/rag_protocol.h` and `.cpp`: protocol-v2 exact JSON decoder and EvidencePack validation.
- Create `src/adapters/rag/persistent_rag_knowledge_provider.h` and `.cpp`: opt-out, lazy sidecar start, handshake, request IDs, one safe pre-query restart.
- Modify `src/config/runtime_config.h` and `.cpp`: replace production RAG settings with `RagConfig` and absolute pack discovery while retaining old parser coverage.
- Modify `src/main.cpp`: create the persistent transport/provider and keep empty-provider behavior when disabled.
- Modify `src/adapters/anthropic/anthropic_messages_client.cpp`: inject the evidence boundary, provenance and legal/safety instructions.
- Modify `src/domain/evidence_validation.cpp`: retain generic domain bounds and accept validated schema-v2 metadata.
- Modify `CMakeLists.txt`: pin and link reproc++ v14.2.7; register new sources and tests.

### C++ and process tests

- Create `tests/process/jsonl_sidecar_fixture.py`.
- Create `tests/adapters/reproc_jsonl_process_test.cpp`.
- Create `tests/adapters/rag_protocol_test.cpp`.
- Create `tests/adapters/persistent_rag_knowledge_provider_test.cpp`.
- Modify `tests/adapters/anthropic_messages_client_test.cpp`.
- Modify `tests/config/runtime_config_test.cpp`.
- Modify `tests/integration/rag_integration_test.cpp`.
- Modify `tests/cli/interactive_process_test.cpp` and `tests/cli/ready_package_contract_test.cmake` for external-pack discovery without changing the four-file Ready inventory.

### Documentation and hardening evidence

- Modify `.env.example` without adding a real key.
- Modify `docs/interactive-memory.md` with build, verify, double-click, offline, legal-snapshot and recovery behavior.
- Use `.assistant-hardening/rounds/round-002.jsonl` and generated report paths through the hardening scripts; do not hand-edit the state database.
- External artifacts go only under `E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03`.

---

### Task 1: Start a fresh hardening round and freeze the baseline

**Files:**
- Modify through skill scripts: `.assistant-hardening/state.json`
- Create through skill scripts: `.assistant-hardening/rounds/round-002.jsonl`
- Read: `C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\references\state-schema.md`
- Read: `C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\references\surface-taxonomy.md`
- Read: `C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\references\probe-catalog.md`
- Read: `C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\references\local-offline-assistant-standard.md`

**Interfaces:**
- Consumes: current source, config, Ready scripts, tests, existing round-001 state.
- Produces: a fresh discovery ledger covering retrieval, prompt assembly, model backend, deployment bundle, eval runner, unrelated-chat and opt-out behavior.

- [ ] **Step 1: Capture repository and environment facts without secrets**

Run:

```powershell
git status --short
git rev-parse HEAD
python --version
python -c "import numpy,torch; print(numpy.__version__); print(torch.__version__); print(torch.cuda.is_available())"
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader
```

Expected: dirty paths are recorded, Python is 3.11.x, and no command reads `.env` or prints environment variables.

- [ ] **Step 2: Read all required hardening references completely**

Run four separate `Get-Content -Raw` commands for the files listed above. Expected: no truncation; if output truncates, use numbered bounded line reads until EOF.

- [ ] **Step 3: Initialize round 002 rather than resuming round 001**

Run:

```powershell
python C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\scripts\init_round.py E:\desktop\How_to_build_a_agent\AgentFramework
```

Expected: a new active round is created because round 001 was completed.

- [ ] **Step 4: Run and log fresh pre-change probes**

Run the current focused Python and C++ RAG tests, inspect prompt serialization and Ready discovery, then append at least these probes with `add_probe.py`: lexical retrieval works; semantic paraphrase is missed by BM25; unrelated greeting currently reaches the knowledge provider; explicit opt-out behavior; corrupt index fails; missing RAG assets at double-click startup; EvidencePack cannot override system instructions; Ready contains no knowledge assets.

Expected: every probe has `origin=fresh`, a named surface, pass/fail result, and exact command or source evidence. Failed behavior becomes a finding before production edits.

- [ ] **Step 5: Commit no source changes**

Round state is operational evidence and remains in the existing user WIP unless the repository's current hardening policy already tracks it. Do not stage unrelated `.assistant-hardening` history in a source commit.

### Task 2: Add strict Knowledge Pack manifests and trusted-path validation

**Files:**
- Create: `rag/agent_rag/pack.py`
- Test: `rag/tests/test_pack.py`

**Interfaces:**
- Consumes: `pathlib.Path`, SHA-256, strict JSON/JSONL files.
- Produces: `PackLayout`, `DocumentManifest`, `PackManifest`, `verify_complete_pack(root: Path) -> PackManifest`, `atomic_publish(staging: Path, destination: Path) -> None`.

- [ ] **Step 1: Write failing manifest and damaged-pack tests**

Add tests shaped as:

```python
def test_verify_pack_rejects_chunk_vector_count_mismatch(tmp_path: Path) -> None:
    root = write_minimal_pack(tmp_path, document_count=1, chunk_count=2, vector_rows=1)
    with pytest.raises(PackError, match="pack counts are inconsistent"):
        verify_complete_pack(root)

def test_atomic_publish_preserves_existing_complete_pack_on_failure(tmp_path: Path) -> None:
    destination = write_complete_pack(tmp_path / "published", pack_id="old")
    staging = write_incomplete_pack(tmp_path / "staging")
    with pytest.raises(PackError):
        atomic_publish(staging, destination)
    assert json.loads((destination / "pack.json").read_text("utf-8"))["pack_id"] == "old"
```

Also cover exact keys, duplicate JSON keys, UTF-8, absolute/traversal paths, symlink/reparse/hardlink leaves, `complete=false`, unexpected entries, SHA mismatch and model revision mismatch.

- [ ] **Step 2: Run the tests and observe RED**

Run: `python -m pytest -q rag/tests/test_pack.py`

Expected: collection fails because `agent_rag.pack` does not exist.

- [ ] **Step 3: Implement the strict pack types and verifier**

Use immutable dataclasses and explicit parsers:

```python
@dataclass(frozen=True)
class PackManifest:
    schema_version: int
    pack_id: str
    snapshot_date: str
    document_count: int
    chunk_count: int
    embedding_model: str
    embedding_revision: str
    embedding_dimensions: int
    complete: bool

def verify_complete_pack(root: Path) -> PackManifest:
    trusted_root = require_trusted_directory(root)
    manifest = parse_exact_json(trusted_root / "pack.json", PACK_KEYS)
    validate_file_hashes_and_counts(trusted_root, manifest)
    return manifest
```

Write JSON through a temporary sibling file, flush and `os.fsync`, then `os.replace`; publication may replace only the exact destination after both paths are resolved under the configured knowledge parent.

- [ ] **Step 4: Run focused and existing Python tests**

Run:

```powershell
python -m pytest -q rag/tests/test_pack.py
python -m pytest -q rag/tests
```

Expected: all pass; old v1 index tests remain green.

- [ ] **Step 5: Commit only pack files**

```powershell
git add -- rag/agent_rag/pack.py rag/tests/test_pack.py
git commit -m "feat: validate external RAG knowledge packs"
```

### Task 3: Download and convert official eCFR XML into 30,000 complete Markdown documents

**Files:**
- Create: `rag/agent_rag/ecfr_source.py`
- Create: `rag/agent_rag/ecfr_document.py`
- Test: `rag/tests/test_ecfr_source.py`
- Test: `rag/tests/test_ecfr_document.py`
- Modify: `rag/agent_rag/cli.py`

**Interfaces:**
- Consumes: snapshot date, pack staging root, official `ecfr.gov` endpoints.
- Produces: `download_titles(config: DownloadConfig) -> DownloadSummary`, `parse_title_xml(path: Path, metadata: TitleMetadata) -> Iterator[EcfrDocument]`, `select_documents(documents, count=30_000)`, `write_corpus(...) -> CorpusSummary`.

- [ ] **Step 1: Write failing downloader boundary tests**

Use a local scripted HTTP fixture, never the real network in unit tests:

```python
def test_downloader_rejects_cross_domain_redirect(tmp_path: Path, http_server: ScriptedServer) -> None:
    http_server.respond(302, {"Location": "https://example.com/stolen.xml"}, b"")
    with pytest.raises(DownloadError, match="redirect target is not allowed"):
        download_one(http_server.url, tmp_path / "title-001.xml", allowed_host="ecfr.gov")

def test_resume_reuses_only_matching_regular_single_link_file(tmp_path: Path) -> None:
    target = write_download_with_manifest(tmp_path, b"<ECFR/>")
    assert download_one_from_fixture(target, response_must_not_be_used()) == DownloadDisposition.REUSED
```

Cover response-byte limits, timeout/429/5xx retry counts, no retry for 404, wrong content type, interrupted partial file, SHA conflict, symlink/reparse/hardlink and host matching.

- [ ] **Step 2: Write failing XML-to-Markdown tests**

Provide compact fixtures with `DIV1`..`DIV8 TYPE="SECTION"`, paragraphs, lists, tables, notes and `[Reserved]`:

```python
def test_section_becomes_one_complete_markdown_with_provenance(tmp_path: Path) -> None:
    documents = list(parse_title_xml(write_xml_fixture(tmp_path), title_metadata()))
    assert [d.citation for d in documents] == ["1 CFR 1.1"]
    markdown = serialize_markdown(documents[0])
    assert "official_url: https://www.ecfr.gov/" in markdown
    assert "Definitions." in markdown
    assert "| Term | Meaning |" in markdown

def test_stable_selection_uses_body_bytes_then_citation_then_sha() -> None:
    chosen = select_documents(sample_documents(), count=2)
    assert [item.document_id for item in chosen] == ["doc-long-a", "doc-long-b"]
```

Cover DTD/entity rejection, nesting/text limits, empty and reserved sections, duplicate-equal citation, duplicate-conflicting citation, unsafe path characters, UTF-8/LF and Windows path length.

- [ ] **Step 3: Run both suites and observe RED**

Run: `python -m pytest -q rag/tests/test_ecfr_source.py rag/tests/test_ecfr_document.py`

Expected: missing module/API failures.

- [ ] **Step 4: Implement bounded official downloads**

Use `urllib.request` with a no-automatic-redirect handler, explicit `User-Agent`, 60-second per-attempt timeout, maximum five attempts, capped exponential delays of 1/2/4/8 seconds, and `.partial-<uuid>` sibling files. Accept only HTTPS and host `www.ecfr.gov`; record URL, retrieved UTC, ETag, Last-Modified, byte count and SHA-256 in `manifest/downloads.jsonl`.

- [ ] **Step 5: Implement streaming secure XML parsing and Markdown**

Reject any input containing case-insensitive `<!DOCTYPE` or `<!ENTITY` before parsing and use `defusedxml.ElementTree.iterparse`. Maintain the current title/chapter/subchapter/part stack, emit only `DIV8` elements whose `TYPE` is `SECTION`, render supported inline/block tags deterministically, and clear processed elements to bound memory.

- [ ] **Step 6: Implement the exact 30,000 selection and CLI commands**

Add strict command forms:

```text
download-ecfr --snapshot 2026-09-03 --staging-root <absolute-path>
build-corpus --snapshot 2026-09-03 --staging-root <absolute-path> --count 30000
```

The parser streams candidates into a staging-only SQLite table rather than retaining all 220,000+ sections in RAM. SQL orders by normalized UTF-8 body bytes descending, citation ascending and body SHA ascending, then selects exactly 30,000 rows. `build-corpus` writes each file plus one exact `documents.jsonl` record, verifies disk count and hashes, and writes `reports/corpus-build.json` only after success.

- [ ] **Step 7: Run focused and full Python tests**

Run:

```powershell
python -m pytest -q rag/tests/test_ecfr_source.py rag/tests/test_ecfr_document.py
python -m pytest -q rag/tests
```

Expected: all pass without network access.

- [ ] **Step 8: Commit corpus pipeline paths**

```powershell
git add -- rag/agent_rag/ecfr_source.py rag/agent_rag/ecfr_document.py rag/agent_rag/cli.py rag/tests/test_ecfr_source.py rag/tests/test_ecfr_document.py
git commit -m "feat: build complete eCFR markdown corpus"
```

### Task 4: Add tokenizer-aware structural chunking

**Files:**
- Create: `rag/agent_rag/chunker.py`
- Test: `rag/tests/test_chunker.py`

**Interfaces:**
- Consumes: `DocumentManifest`, Markdown text, a `TokenCodec` exposing `encode(text) -> list[int]` and `decode(ids) -> str`.
- Produces: `ChunkRecord` and `chunk_document(document, markdown, tokenizer, max_tokens=768, overlap_tokens=96) -> list[ChunkRecord]`.

- [ ] **Step 1: Write failing chunk invariant tests**

```python
def test_chunks_respect_bge_token_limit_and_never_cross_document() -> None:
    chunks = chunk_documents(two_documents(), character_tokenizer(), 8, 2)
    assert all(chunk.token_count <= 8 for chunk in chunks)
    assert {chunk.document_id for chunk in chunks} == {"doc-a", "doc-b"}
    assert all(chunk.previous_id is None or chunk.previous_id.startswith(chunk.document_id) for chunk in chunks)

def test_oversized_paragraph_uses_sentence_then_token_windows() -> None:
    chunks = chunk_document(document(), long_paragraph_markdown(), word_tokenizer(), 6, 2)
    assert [chunk.body for chunk in chunks] == expected_sentence_preserving_windows()
```

Cover headings, list items, table rows, 96-token maximum overlap, line and character offsets, stable IDs, empty body, prefix accounting and repeatability.

- [ ] **Step 2: Run and observe RED**

Run: `python -m pytest -q rag/tests/test_chunker.py`

Expected: missing `agent_rag.chunker`.

- [ ] **Step 3: Implement chunking as pure functions**

Define:

```python
@dataclass(frozen=True)
class ChunkRecord:
    chunk_id: str
    document_id: str
    content: str
    start_line: int
    end_line: int
    start_char: int
    end_char: int
    token_count: int
    sha256: str
    previous_id: str | None
    next_id: str | None
```

Parse front matter separately, prepend citation hierarchy to embedding content, and retain the unmodified cited body as evidence content.

- [ ] **Step 4: Run focused and full Python tests**

Run: `python -m pytest -q rag/tests/test_chunker.py rag/tests`

Expected: all pass.

- [ ] **Step 5: Commit chunker paths**

```powershell
git add -- rag/agent_rag/chunker.py rag/tests/test_chunker.py
git commit -m "feat: add token-aware legal document chunking"
```

### Task 5: Build pinned BGE-M3 embeddings and schema-v2 hybrid indexes

**Files:**
- Create: `rag/agent_rag/embedding.py`
- Create: `rag/agent_rag/hybrid_index.py`
- Create: `rag/requirements-rag.lock`
- Test: `rag/tests/test_embedding.py`
- Test: `rag/tests/test_hybrid_index.py`
- Modify: `rag/agent_rag/cli.py`

**Interfaces:**
- Consumes: verified corpus manifest, `ChunkRecord`, pinned local model directory.
- Produces: `EmbeddingBackend.encode(texts: Sequence[str]) -> np.ndarray`, `BgeM3Embedding`, `build_hybrid_index(pack_root, embedding, device, batch_size) -> IndexBuildSummary`.

- [ ] **Step 1: Write failing embedding contract tests**

```python
def test_embedding_backend_rejects_wrong_dimensions() -> None:
    backend = FixedEmbeddingBackend(np.ones((1, 3), dtype=np.float32))
    with pytest.raises(EmbeddingError, match="expected 1024 dimensions"):
        encode_normalized(backend, ["query"], dimensions=1024)

def test_normalization_produces_unit_float32_before_f16_storage() -> None:
    result = normalize_rows(np.array([[3.0, 4.0]], dtype=np.float32))
    np.testing.assert_allclose(np.linalg.norm(result, axis=1), [1.0])
```

Also cover empty, nonfinite, zero vector, row-count mismatch, fixed revision, tokenizer fingerprint and deterministic test backend.

- [ ] **Step 2: Write failing index integrity and incremental tests**

```python
def test_index_rows_follow_stable_chunk_id_order(tmp_path: Path) -> None:
    summary = build_fixture_index(tmp_path, chunks_in_reverse_order(), fixed_embedding())
    assert read_vector_row_ids(summary.database) == sorted_chunk_ids()

def test_model_change_forces_full_vector_rebuild(tmp_path: Path) -> None:
    first = build_fixture_index(tmp_path, chunks(), fixed_embedding(revision="a"))
    second = build_fixture_index(tmp_path, chunks(), fixed_embedding(revision="b"))
    assert second.reused_vector_rows == 0
```

Cover SQLite foreign keys/integrity, float16 byte size, endian marker, vector/database SHA, atomic output, document/chunk removals, unchanged reuse and failed build preserving published index.

- [ ] **Step 3: Run and observe RED**

Run: `python -m pytest -q rag/tests/test_embedding.py rag/tests/test_hybrid_index.py`

Expected: missing module/API failures.

- [ ] **Step 4: Implement the real and deterministic embedding adapters**

`BgeM3Embedding` loads only a local directory with `local_files_only=True`, verifies the model lock before import/use, sets maximum sequence length 768, calls `SentenceTransformer.encode(..., normalize_embeddings=True, convert_to_numpy=True)`, and validates shape/finite/unit norm. `FixedEmbeddingBackend` is test-only and stays under `rag/tests`.

- [ ] **Step 5: Implement schema-v2 SQLite and vector publication**

Use tables `metadata`, `documents`, `chunks`, and `postings`; enforce primary/foreign keys and unique `vector_row`. Write vectors in sorted chunk ID order to a sibling temporary matrix, flush/fsync, verify expected bytes as `rows * 1024 * 2`, hash it, then atomically replace index artifacts only inside staging.

- [ ] **Step 6: Pin runtime dependencies and CLI**

Write exact lock entries:

```text
--extra-index-url https://download.pytorch.org/whl/cu130
defusedxml==0.7.1
numpy==2.4.6
sentence-transformers==6.0.1
torch==2.11.0+cu130
```

Add `build-index --pack-root <absolute> --device auto --batch-size 16`. `auto` chooses CUDA only if `torch.cuda.is_available()` and an allocation/embedding self-test succeeds; otherwise it records CPU fallback explicitly.

- [ ] **Step 7: Run focused/full tests and one local BGE smoke**

Run:

```powershell
python -m pytest -q rag/tests/test_embedding.py rag/tests/test_hybrid_index.py
python -m pytest -q rag/tests
python -E -s -X utf8 rag/agent_rag_cli.py verify-model --model-root E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03\model\bge-m3
```

Expected: deterministic tests pass; model smoke reports revision, dimensions 1024, finite unit vectors and the actual selected device without printing input text.

- [ ] **Step 8: Commit index paths**

```powershell
git add -- rag/agent_rag/embedding.py rag/agent_rag/hybrid_index.py rag/requirements-rag.lock rag/agent_rag/cli.py rag/tests/test_embedding.py rag/tests/test_hybrid_index.py
git commit -m "feat: build pinned BGE-M3 hybrid indexes"
```

### Task 6: Implement exact dense plus BM25 RRF retrieval

**Files:**
- Create: `rag/agent_rag/hybrid_retriever.py`
- Test: `rag/tests/test_hybrid_retriever.py`

**Interfaces:**
- Consumes: verified `PackManifest`, read-only immutable SQLite, memory-mapped vector matrix, `EmbeddingBackend`.
- Produces: `HybridRetriever.query(query: str, top_k: int, max_total_bytes: int, mode: str) -> list[EvidenceItem]`.

- [ ] **Step 1: Write failing ranking and budget tests**

```python
def test_rrf_fuses_lexical_and_dense_with_constant_60() -> None:
    ranked = reciprocal_rank_fusion(["a", "b"], ["b", "c"], constant=60)
    assert [item.chunk_id for item in ranked] == ["b", "a", "c"]

def test_neighbor_never_displaces_all_primary_hits() -> None:
    items = select_evidence(primary_hits(), neighbors(), top_k=2, max_total_bytes=100)
    assert any(not item.is_neighbor for item in items)
    assert sum(len(item.content.encode("utf-8")) for item in items) <= 100
```

Cover BM25 top 100, dense top 100, stable ties, same-content dedupe, maximum two primary hits per document, direct neighbors only, no paragraph truncation, query bounds, explicit lexical mode and hybrid fail-closed corruption.

- [ ] **Step 2: Run and observe RED**

Run: `python -m pytest -q rag/tests/test_hybrid_retriever.py`

Expected: missing module/API failures.

- [ ] **Step 3: Implement read-only retrieval**

Reuse the current tokenizer/BM25 formula, return ranks rather than treating raw scores as comparable, embed one query, scan `vectors.f16` in bounded row blocks, and select dense top 100 with `numpy.argpartition` followed by a stable full sort of candidates. Open SQLite with `mode=ro&immutable=1` and `PRAGMA query_only=ON`.

- [ ] **Step 4: Implement RRF and evidence assembly**

Use exactly `1/(60+rank)`, strict stable tie-breaks, SHA dedupe, per-document cap, neighbor linkage from SQLite, and final 32 KiB accounting on UTF-8 evidence content only. Attach citation, relative path, lines, date, official URL, content/document SHA, two optional ranks and fusion score.

- [ ] **Step 5: Run focused/full tests**

Run: `python -m pytest -q rag/tests/test_hybrid_retriever.py rag/tests`

Expected: all pass.

- [ ] **Step 6: Commit retriever paths**

```powershell
git add -- rag/agent_rag/hybrid_retriever.py rag/tests/test_hybrid_retriever.py
git commit -m "feat: fuse dense and BM25 legal retrieval"
```

### Task 7: Add the strict persistent JSONL sidecar protocol

**Files:**
- Create: `rag/agent_rag/sidecar.py`
- Modify: `rag/agent_rag/protocol.py`
- Modify: `rag/agent_rag/cli.py`
- Test: `rag/tests/test_sidecar.py`
- Modify: `rag/tests/test_agent_rag_pytest.py`

**Interfaces:**
- Consumes: verified pack root and `HybridRetriever`.
- Produces: `parse_v2_message(line: str) -> ClientMessage`, `run_sidecar(pack_root: Path, stdin, stdout) -> int`; retains `parse_query_request` for v1 compatibility tests.

- [ ] **Step 1: Write failing protocol and lifecycle tests**

```python
def test_sidecar_emits_ready_once_and_reuses_loaded_retriever() -> None:
    output, factory = run_scripted_sidecar([health_request("r1"), query_request("r2", "tax")])
    assert [line["op"] for line in output] == ["ready", "health_result", "query_result"]
    assert factory.calls == 1

@pytest.mark.parametrize("line", duplicate_extra_nan_wrong_id_and_oversized_lines())
def test_protocol_rejects_owned_schema_violations(line: str) -> None:
    with pytest.raises(ProtocolError):
        parse_v2_message(line)
```

Cover initialize/ready, health, query, shutdown, EOF, invalid UTF-8, control bytes, query bounds, max response bytes, safe stderr, pack load failure and no partial response.

- [ ] **Step 2: Run and observe RED**

Run: `python -m pytest -q rag/tests/test_sidecar.py`

Expected: missing sidecar/protocol-v2 API failures.

- [ ] **Step 3: Implement exact JSONL messages**

Every request has exact keys `schema_version`, `request_id`, `op`, `payload`; schema is integer 2; request IDs match `req-` plus 32 lowercase hex. Parse with `object_pairs_hook` to reject duplicate keys, `parse_constant` to reject NaN/Infinity, a 65,536-byte line cap, and explicit UTF-8/control checks.

- [ ] **Step 4: Implement one-load sidecar lifecycle**

`serve --pack-root <absolute>` loads and verifies the pack before writing one `ready` line. It flushes each response, returns safe structured error codes, does not include tracebacks or query text, and exits zero only on validated `shutdown` or clean EOF.

- [ ] **Step 5: Run sidecar, compatibility and full Python suites**

Run:

```powershell
python -m pytest -q rag/tests/test_sidecar.py
python -m pytest -q rag/tests/test_agent_rag_pytest.py rag/tests/test_agent_rag.py
python -m pytest -q rag/tests
```

Expected: all pass; protocol v1 command behavior remains covered but is no longer the production path.

- [ ] **Step 6: Commit sidecar paths**

```powershell
git add -- rag/agent_rag/sidecar.py rag/agent_rag/protocol.py rag/agent_rag/cli.py rag/tests/test_sidecar.py rag/tests/test_agent_rag_pytest.py
git commit -m "feat: serve hybrid RAG over strict JSONL"
```

### Task 8: Add a long-lived reproc++ JSONL transport

**Files:**
- Create: `src/ports/jsonl_process.h`
- Create: `src/adapters/process/reproc_jsonl_process.h`
- Create: `src/adapters/process/reproc_jsonl_process.cpp`
- Create: `tests/process/jsonl_sidecar_fixture.py`
- Create: `tests/adapters/reproc_jsonl_process_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: absolute executable, argument array, cwd, startup/query/stop deadlines, line bounds.
- Produces: `JsonlProcess::start`, `exchange`, `running`, `stop`; `ReprocJsonlProcess` owns one child and is non-copyable.

- [ ] **Step 1: Write failing transport process tests**

Tests launch the Python fixture and assert literal Unicode arguments, one process reused for two exchanges, stdout line framing, concurrent bounded stderr drain, delayed startup timeout, query timeout, oversized line rejection, unexpected exit and destructor termination of a spawned descendant.

Use the wished-for interface:

```cpp
agent::ReprocJsonlProcess process;
auto started = process.start({python, {fixture, "echo"}, cwd, 2'000, 65'536, 4'096});
REQUIRE(started.has_value());
REQUIRE(process.exchange(R"({"id":1})", 1'000).value() == R"({"id":1})");
REQUIRE(process.exchange(R"({"id":2})", 1'000).value() == R"({"id":2})");
```

- [ ] **Step 2: Pin reproc++ and observe RED**

Add FetchContent tag `v14.2.7` with commit `06034a7fca1ec46eddb4997f7764db89380c5216`, static build options, source/test registration, and then build only the new test.

Run:

```powershell
cmake -S . -B build-rag-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-rag-debug --config Debug --target reproc_jsonl_process_tests
```

Expected: compile failure for missing transport implementation.

- [ ] **Step 3: Implement bounded persistent I/O**

Write stdin plus newline, close only on stop, read exactly one stdout line with deadline, continuously drain stderr into a bounded ring buffer, reject NUL/invalid UTF-8/oversize, and kill the process tree on timeout or protocol loss. Never invoke a shell; pass literal argument vectors.

- [ ] **Step 4: Run focused and existing process tests**

Run:

```powershell
cmake --build build-rag-debug --config Debug --target reproc_jsonl_process_tests direct_process_runner_tests
ctest --test-dir build-rag-debug -C Debug -R "reproc_jsonl_process_tests|direct_process_runner_tests" --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 5: Commit exact transport paths and CMake hunk**

Stage the new files and only the reproc/source/test hunks in `CMakeLists.txt`; inspect `git diff --cached` before committing.

Commit: `feat: add persistent bounded JSONL process transport`.

### Task 9: Decode protocol v2 and provide EvidencePack through C++

**Files:**
- Create: `src/adapters/rag/rag_protocol.h`
- Create: `src/adapters/rag/rag_protocol.cpp`
- Create: `src/adapters/rag/persistent_rag_knowledge_provider.h`
- Create: `src/adapters/rag/persistent_rag_knowledge_provider.cpp`
- Create: `tests/adapters/rag_protocol_test.cpp`
- Create: `tests/adapters/persistent_rag_knowledge_provider_test.cpp`
- Modify: `src/domain/evidence_validation.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `JsonlProcess`, `RagConfig`, `TaskState.issue`.
- Produces: `decode_ready`, `decode_query_result`, and `PersistentRagKnowledgeProvider::retrieve(const TaskState&) -> Result<EvidencePack>`.

- [ ] **Step 1: Write failing protocol decoder tests**

Generate valid and invalid ready/query lines. Assert exact schema, request ID, pack ID, 30,000 document count, dimensions 1024, item count, domain, relative paths, dates, optional integer ranks, finite nonnegative fusion score and recomputed content SHA.

```cpp
const auto decoded = agent::rag::decode_query_result(valid_response(), "req-00000000000000000000000000000001", 6, 32768);
REQUIRE(decoded.has_value());
REQUIRE(decoded.value().items.front().metadata.at("citation").as_string() == "40 CFR 60.1");
```

Every owned-schema mutation receives a fixed `ProtocolFailure` with no echoed payload.

- [ ] **Step 2: Write failing provider lifecycle and opt-out tests**

Use a `FakeJsonlProcess` to prove lazy one-time start, handshake before query, unique request IDs, two retrievals reuse one process, pre-query crash restarts once, in-flight failure does not replay, destructor stops, lexical mode payload, unrelated interactive command, and Chinese/English opt-out return empty evidence without starting the child.

- [ ] **Step 3: Build and observe RED**

Run:

```powershell
cmake --build build-rag-debug --config Debug --target rag_protocol_tests persistent_rag_knowledge_provider_tests
```

Expected: missing headers/types or test failures for unimplemented behavior.

- [ ] **Step 4: Implement strict decoding and provider state machine**

Keep JSON parsing separate from process lifecycle. Provider states are `Stopped`, `Starting`, `Ready`, `Broken`; mutex-protect them because model/session orchestration may become concurrent. Generate request IDs locally, accept only matching responses, and map start/load failures to retryable `DependencyUnavailable`, timeouts to retryable `RequestTimeout`, and malformed responses to nonretryable `ProtocolFailure`.

- [ ] **Step 5: Run focused and domain regression tests**

Run:

```powershell
cmake --build build-rag-debug --config Debug --target rag_protocol_tests persistent_rag_knowledge_provider_tests evidence_validation_tests
ctest --test-dir build-rag-debug -C Debug -R "rag_protocol_tests|persistent_rag_knowledge_provider_tests|evidence_validation_tests" --output-on-failure
```

Expected: all pass.

- [ ] **Step 6: Commit provider files and exact CMake/domain hunks**

Inspect the cached diff and commit: `feat: retrieve validated evidence from persistent RAG`.

### Task 10: Wire configuration, EXE-relative pack discovery and runtime construction

**Files:**
- Modify: `src/config/runtime_config.h`
- Modify: `src/config/runtime_config.cpp`
- Modify: `src/main.cpp`
- Modify: `.env.example`
- Modify: `tests/config/runtime_config_test.cpp`
- Modify: `tests/cli/interactive_process_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: environment values and executable path.
- Produces: `RagConfig { enabled, mode, pack_root, top_k, max_total_bytes, startup_timeout_ms, query_timeout_ms, device }` and provider construction.

- [ ] **Step 1: Write failing exact configuration tests**

Cover defaults and every invalid setting:

```cpp
TEST_CASE(rag_pack_config_requires_absolute_external_complete_pack) {
    FakeEnvironment env = valid_environment();
    env.values["AGENT_ENABLE_RAG"] = "1";
    env.values["AGENT_RAG_PACK_ROOT"] = "relative-pack";
    require_config_error(agent::load_runtime_config(env), "AGENT_RAG_PACK_ROOT must be absolute");
}
```

Assert Ready defaults `hybrid`, top K 6, 32768 bytes, startup 120 seconds, query 30 seconds, device auto; invalid flags/enums/ranges do not echo values. Add process coverage proving a synthetic sibling `active-pack.json` is found from EXE directory under a different cwd.

- [ ] **Step 2: Build and observe RED**

Run: `cmake --build build-rag-debug --config Debug --target runtime_config_tests interactive_process_tests`

Expected: assertions fail because the old Python/script/index configuration is still production behavior.

- [ ] **Step 3: Implement new config while protecting credential-free commands**

Parse RAG settings only for commands that build a runtime; `verify-log` and `evaluate-log` remain credential- and RAG-free. Resolve `AGENT_RAG_PACK_ROOT` as an absolute trusted directory or read the strict EXE-relative pointer; reject Knowledge Pack roots inside `out/AgentFramework-Ready`.

- [ ] **Step 4: Construct the persistent provider in `main.cpp`**

When enabled, create `ReprocJsonlProcess` and `PersistentRagKnowledgeProvider`; otherwise preserve `EmptyKnowledgeProvider`. Ensure ownership order keeps the transport alive through all Runtime/Session calls and shuts it down before process exit.

- [ ] **Step 5: Update `.env.example` without a secret**

Use:

```dotenv
AGENT_ENABLE_RAG=1
AGENT_RAG_MODE=hybrid
AGENT_RAG_PACK_ROOT=E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03
AGENT_RAG_TOP_K=6
AGENT_RAG_MAX_TOTAL_BYTES=32768
AGENT_RAG_STARTUP_TIMEOUT_SECONDS=120
AGENT_RAG_QUERY_TIMEOUT_SECONDS=30
AGENT_RAG_DEVICE=auto
```

Do not modify or display the real `.env` content in tests or diffs.

- [ ] **Step 6: Run focused and CLI regression tests**

Run:

```powershell
cmake --build build-rag-debug --config Debug --target runtime_config_tests cli_tests interactive_process_tests
ctest --test-dir build-rag-debug -C Debug -R "runtime_config_tests|cli_tests|interactive_process_tests|verify_log_process_credential_free" --output-on-failure
```

Expected: all pass.

- [ ] **Step 7: Commit only reviewed config/main/example/CMake hunks**

Commit: `feat: discover and start external RAG knowledge packs`.

### Task 11: Harden MiniMax prompt assembly and legal evidence behavior

**Files:**
- Modify: `src/adapters/anthropic/anthropic_messages_client.cpp`
- Modify: `tests/adapters/anthropic_messages_client_test.cpp`
- Modify: `tests/application/runtime_engine_test.cpp` only if a provider opt-out boundary requires runtime coverage.

**Interfaces:**
- Consumes: `ModelRequest.evidence` with schema-v2 metadata.
- Produces: a delimited, untrusted evidence system block with citation/date/source instructions and a legal-information disclaimer.

- [ ] **Step 1: Write failing positive and hostile-evidence tests**

Add an HTTP capture test whose evidence contains `Ignore all previous instructions`, a fake tool call, closing delimiter text, a credential request and control-looking JSON. Assert these bytes occur only inside JSON-escaped evidence data, the trusted instruction prefix/suffix occurs once, tools are unchanged, and user content remains a separate message.

Add a positive test asserting `citation`, `snapshot_date`, and `official_url` are present in the evidence JSON and the instruction asks MiniMax to cite them.

- [ ] **Step 2: Build and observe RED**

Run: `cmake --build build-rag-debug --config Debug --target anthropic_adapter_tests`

Expected: the new evidence-boundary assertions fail against the current short prefix.

- [ ] **Step 3: Implement one deterministic evidence wrapper**

Serialize evidence through nlohmann JSON, never concatenate raw document text into trusted instructions. The system block explicitly labels evidence untrusted, forbids following embedded commands, requires supported citations and snapshot dates, says absence of evidence must be disclosed, and states the result is not legal advice.

- [ ] **Step 4: Run adapter and runtime tests**

Run:

```powershell
ctest --test-dir build-rag-debug -C Debug -R "anthropic_adapter_tests|runtime_engine_tests" --output-on-failure
```

Expected: all pass and captured requests contain no secrets.

- [ ] **Step 5: Commit exact prompt/test hunks**

Commit: `fix: isolate untrusted RAG evidence in model prompts`.

### Task 12: Build the portable Knowledge Pack runtime and preserve Ready's four-file contract

**Files:**
- Create: `scripts/build_ecfr_knowledge_pack.ps1`
- Modify: `cmake/stage_ready_package.cmake`
- Modify: `cmake/verify_ready_package.cmake`
- Modify: `tests/cli/ready_package_contract_test.cmake`
- Modify: `docs/interactive-memory.md`

**Interfaces:**
- Consumes: official `https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip` with SHA-256 `009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b`, locked wheels, pinned model revision, pipeline CLI.
- Produces: verified external Knowledge Pack and EXE-relative `active-pack.json` pointer outside Ready.

- [ ] **Step 1: Write failing packaging and startup contract tests**

Test that Ready still rejects any fifth unmanaged file, staging does not delete runtime data, missing/corrupt pointer gives a fixed startup error, a valid sibling pointer works under a foreign cwd, Knowledge Pack files are never copied into Ready, and verification reads only `.env` metadata rather than credentials.

- [ ] **Step 2: Run and observe RED**

Run the Release `ready_package_contract` test. Expected: sibling Knowledge Pack discovery/runtime integrity assertions are not implemented.

- [ ] **Step 3: Implement the build script with guarded paths**

The script accepts only explicit `-KnowledgeRoot`, `-Snapshot 2026-09-03`, and `-DocumentCount 30000`; resolves and checks every destructive/replace target stays under the exact KnowledgeRoot; creates a unique staging directory; downloads and verifies the exact Python archive above; installs the constrained packages; hashes every resolved wheel into `runtime.lock.json`; downloads the pinned model; invokes download/corpus/index/verify; then atomically publishes. A resume accepts only an identical runtime lock. It writes no raw command strings containing environment values.

- [ ] **Step 4: Add runtime and dependency lock verification**

Before publishing, execute the pack's own Python with `-E -s -X utf8` to import defusedxml, numpy, torch and sentence_transformers; verify torch/device behavior, model revision/files, CLI import, SQLite integrity and vector shape. Hash Python/runtime files into `model.lock.json`/`pack.json`.

- [ ] **Step 5: Keep Ready minimal and document recovery**

The CMake scripts continue to manage only `.env`, EXE and runtime DLLs. Documentation gives the one explicit pack-build command, the Ready build command, double-click behavior, offline-after-build status, fixed snapshot/legal caveat, verification command, and exact recovery for missing/corrupt packs.

- [ ] **Step 6: Run Release packaging tests**

Run:

```powershell
cmake -S . -B build-rag-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-rag-release --config Release --target agent_ready_package_verify
ctest --test-dir build-rag-release -C Release -R "ready_package_contract|interactive_startup_process" --output-on-failure
```

Expected: Ready inventory is still exactly four files on this build, sibling synthetic pack startup passes from a different cwd, real `.env` content is not printed.

- [ ] **Step 7: Commit script, docs, packaging tests and exact CMake hunks**

Commit: `feat: package double-click hybrid RAG runtime`.

### Task 13: Add a 300-case retrieval evaluation and damaged-environment suite

**Files:**
- Create: `rag/agent_rag/evaluation.py`
- Create: `rag/eval/ecfr_eval_schema.json`
- Create: `rag/eval/negative_cases.jsonl`
- Create: `rag/tests/test_evaluation.py`
- Modify: `rag/agent_rag/cli.py`

**Interfaces:**
- Consumes: JSONL cases with `case_id`, `language`, `query`, `relevant_document_ids`, `relevant_chunk_ids`, `kind`, `rationale`; a verified retriever.
- Produces: per-mode metrics and `reports/retrieval-eval.json` with Recall@5/10, MRR, nDCG@10, no-answer false-positive rate, P50/P95 and peak memory.

- [ ] **Step 1: Write failing metric and split-integrity tests**

```python
def test_metrics_use_zero_for_missed_query() -> None:
    result = score_case(relevant={"d1"}, ranked=["d2", "d3"])
    assert result.recall_at_5 == 0.0
    assert result.reciprocal_rank == 0.0

def test_evaluation_rejects_case_overlap_between_development_and_holdout() -> None:
    with pytest.raises(EvaluationError, match="case split overlap"):
        validate_splits([case("same")], [case("same")])
```

Cover formulas, stable JSON, duplicate IDs, missing labels, unknown docs/chunks, category/language counts, no-answer scoring, warm-up exclusion and latency percentiles.

- [ ] **Step 2: Run and observe RED**

Run: `python -m pytest -q rag/tests/test_evaluation.py`

Expected: missing evaluation module.

- [ ] **Step 3: Implement evaluator and strict CLI**

Add:

```text
evaluate --pack-root <absolute> --cases <absolute-jsonl> --mode lexical|dense|hybrid --output <absolute-json>
```

Run all three modes against identical case order, record pack/model IDs, and fail publication if hybrid Recall@5 or MRR is below either single-path score, or neither metric strictly improves. Fit the pack's dense-cosine no-answer gate only on the development split by selecting the highest-recall threshold whose development false-positive rate is at most 5%; record that numeric threshold in `pack.json` and apply it unchanged to holdout and runtime queries. An exact normalized CFR citation match bypasses only this no-answer gate, not evidence validation or byte limits.

- [ ] **Step 4: Define required evaluation composition**

The final external case file contains at least: 60 exact citation, 60 English paraphrase, 60 Chinese cross-language, 40 multi-concept, 30 adjacent-context, 25 no-answer and 25 injection/damaged-source cases. Development and holdout IDs are disjoint; every positive label resolves to the published manifest and stores an audit rationale.

- [ ] **Step 5: Run focused/full evaluation tests**

Run: `python -m pytest -q rag/tests/test_evaluation.py rag/tests`

Expected: all pass.

- [ ] **Step 6: Commit evaluator and static negative/schema assets**

```powershell
git add -- rag/agent_rag/evaluation.py rag/eval/ecfr_eval_schema.json rag/eval/negative_cases.jsonl rag/tests/test_evaluation.py rag/agent_rag/cli.py
git commit -m "test: measure hybrid RAG retrieval quality"
```

### Task 14: Build and verify the real 30,000-document Knowledge Pack

**Files:**
- Create outside Git: `E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge\ecfr-2026-09-03\...`
- Create outside Git: pack manifests, corpus, model, index and reports defined by the spec.

**Interfaces:**
- Consumes: official eCFR bulk endpoints, pinned Hugging Face model, completed build script.
- Produces: the real complete Knowledge Pack used by the EXE.

- [ ] **Step 1: Check capacity and exact target paths**

Resolve the KnowledgeRoot, confirm it is outside the repo and Ready directory, record free disk, inspect any existing target without deleting it, and choose a unique staging directory. Expected: sufficient space for raw XML, 30,000 Markdown files, model/runtime, chunk database, float16 vectors and temporary build duplication.

- [ ] **Step 2: Build with resumable official downloads**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_ecfr_knowledge_pack.ps1 -KnowledgeRoot 'E:\desktop\How_to_build_a_agent\AgentFramework-Knowledge' -Snapshot '2026-09-03' -DocumentCount 30000
```

Poll long-running work at intervals below 60 seconds and report material milestones only. Expected: 49 title XML files, exactly 30,000 corpus Markdown files and no cross-domain downloads.

- [ ] **Step 3: Verify corpus facts before embedding**

Run the pack verifier plus independent filesystem/manifest counts and SHA sampling. Expected: 30,000 unique document IDs, citations, paths and body hashes; zero empty/reserved/conflicting documents; every sampled Markdown contains complete section provenance.

- [ ] **Step 4: Build real vectors and hybrid index**

Use batch size 16 initially; reduce only on measured CUDA out-of-memory and record the final value/device. Expected: all vector rows are real BGE-M3 outputs, dimension 1024, finite normalized values, vector rows equal chunk rows, and incremental rerun reuses all unchanged rows.

- [ ] **Step 5: Create and audit 300 evaluation cases**

Generate candidates from stratified selected documents, verify every label against the actual Markdown and chunk index, produce the exact category counts from Task 13, and manually inspect all no-answer/injection cases plus a stratified sample of positives before locking development/holdout splits.

- [ ] **Step 6: Run BM25, dense and hybrid quality evaluation**

Write `reports/retrieval-eval.json`. Expected: valid metrics and latency/memory data; hybrid meets the spec gate. If it does not, change only documented chunk/fusion parameters through a new failing evaluation regression, rebuild affected indexes, and rerun the untouched holdout once.

- [ ] **Step 7: Publish only after full pack verification**

Expected: `pack.json.complete=true`, all hashes/counts/integrity checks pass, the old published pack remains untouched until atomic replacement, and `active-pack.json` points to the new absolute root.

### Task 15: End-to-end integration, hardening closure and live MiniMax-M3 proof

**Files:**
- Modify: `tests/integration/rag_integration_test.cpp`
- Modify: `tests/cli/interactive_process_test.cpp`
- Modify through scripts: `.assistant-hardening/rounds/round-002.jsonl`
- Generate through scripts: `.assistant-hardening/rounds/round-002-report.md`

**Interfaces:**
- Consumes: complete real pack, Ready EXE, configured MiniMax-M3 credential through `.env`.
- Produces: fresh deterministic, damaged-environment, double-click-equivalent, quality and live-provider evidence.

- [ ] **Step 1: Write failing end-to-end process tests before final integration edits**

Use a tiny fixture pack to prove one sidecar PID/model-load marker across two interactive turns, citation metadata survives Task JSONL replay, corrupt vector matrix prevents model call, missing model gives a stable safe error, explicit opt-out does not start sidecar, and hostile evidence cannot cause the scripted model/tool fixture to execute an action.

- [ ] **Step 2: Run and observe RED, then make the smallest integration fixes**

Run the focused integration/process tests, confirm the intended failures, edit only their owning boundaries, and rerun until green. Do not loosen strict validators to make fixtures pass.

- [ ] **Step 3: Run all Debug verification**

```powershell
cmake --build build-rag-debug --config Debug
ctest --test-dir build-rag-debug -C Debug --output-on-failure
python -m pytest -q rag/tests
```

Expected: zero failures with complete output retained in a report-safe log.

- [ ] **Step 4: Run all Release and Ready verification**

```powershell
cmake --build build-rag-release --config Release --target agent_ready_package_verify
ctest --test-dir build-rag-release -C Release --output-on-failure
```

Expected: zero failures; Ready inventory remains four files; external real pack verifies separately.

- [ ] **Step 5: Run double-click-equivalent real-pack conversations**

Launch the Ready EXE with its directory as executable location but a different cwd. Ask one Chinese question whose answer is present, one unanswerable legal question, one opt-out question and one prompt-injection negative. Expected: correct official citation/URL/date on the positive, no fabricated citation on the no-answer, no sidecar lookup on opt-out, no instruction execution on injection, and a single sidecar load across the conversation.

- [ ] **Step 6: Run one fresh live MiniMax-M3 smoke without exposing the key**

Use the existing opt-in live-test path and `.env` discovery; capture only exit status, stable request ID, cited source IDs and redacted response summary. Expected: MiniMax-M3 answers from the real EvidencePack and no output contains credential-like text.

- [ ] **Step 7: Complete hardening probes and findings**

After the fresh ledger exists, read `references/experience-log.md`, add applicable experience probes, rerun differential/carryover probes, mark each finding fixed/carryover/not-reproducible with evidence, and explicitly cover retrieval opt-out, unrelated chat, corrupt/missing/wrong-shape assets, prompt injection, stored event parity, Ready parity and live/offline claim boundaries.

- [ ] **Step 8: Generate and strict-validate round 002**

```powershell
python C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\scripts\summarize_round.py E:\desktop\How_to_build_a_agent\AgentFramework
python C:\Users\Lenovo\.codex\skills\hardening-ai-assistants\scripts\validate_round.py E:\desktop\How_to_build_a_agent\AgentFramework --strict
```

Expected: valid state, no unrecorded failed probe, trend metrics and a next-round seed.

- [ ] **Step 9: Invoke verification and code-review skills**

Use `superpowers:verification-before-completion`, then `superpowers:requesting-code-review`. Resolve every valid review finding through a failing regression test and rerun the affected plus full suites.

- [ ] **Step 10: Commit only final integration/docs/hardening paths that belong to this feature**

Inspect `git diff --cached --stat` and `git diff --cached`; commit as `feat: deliver verified eCFR hybrid vector RAG`. Do not include the external Knowledge Pack or unrelated memory/session WIP.

## Completion Record

The implementation is complete only when the executor can fill every checkbox with a concrete command result and provide: commit IDs; Debug/Release/pytest counts; round-002 strict validation; corpus/index/model hashes and counts; BM25/dense/hybrid metrics; P50/P95 and peak memory; Ready inventory; sidecar reuse evidence; double-click-equivalent outcomes; and one redacted live MiniMax-M3 result. A passing deterministic suite without the real 30,000-document pack, or a working live answer without deterministic and negative tests, is explicitly incomplete.
