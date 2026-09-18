from __future__ import annotations

from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sqlite3
import sys

import numpy as np
import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.chunker import ChunkRecord, DocumentSource  # type: ignore[import-not-found]
from agent_rag.embedding import (  # type: ignore[import-not-found]
    BACKEND_SENTENCE_TRANSFORMERS,
    BGE_M3_MODEL,
    PRECISION_FLOAT32,
)
from agent_rag.hybrid_index import (  # type: ignore[import-not-found]
    HybridIndexError,
    build_index_from_chunks,
)


class WordTokenizer:
    def encode(self, text: str) -> list[str]:
        return text.split()

    def decode(self, tokens: list[str]) -> str:
        return " ".join(tokens)

    def get_vocab(self) -> dict[str, int]:
        return {"legal": 1, "alpha": 2, "beta": 3}


class DeterministicEmbedding:
    model = BGE_M3_MODEL
    dimensions = 4
    tokenizer = WordTokenizer()
    backend = BACKEND_SENTENCE_TRANSFORMERS
    precision = PRECISION_FLOAT32

    def __init__(
        self,
        revision: str = "revision-a",
        *,
        backend: str = BACKEND_SENTENCE_TRANSFORMERS,
        precision: str = PRECISION_FLOAT32,
    ) -> None:
        self.revision = revision
        self.backend = backend
        self.precision = precision
        self.calls: list[list[str]] = []

    def encode(self, texts: list[str], *, batch_size: int = 16) -> np.ndarray:
        del batch_size
        self.calls.append(list(texts))
        rows = []
        for text in texts:
            digest = hashlib.sha256(text.encode("utf-8")).digest()
            rows.append([float(digest[index] + 1) for index in range(self.dimensions)])
        return np.asarray(rows, dtype=np.float32)


class FailingEmbedding(DeterministicEmbedding):
    def encode(self, texts: list[str], *, batch_size: int = 16) -> np.ndarray:
        del texts, batch_size
        raise RuntimeError("injected failure")


def _document(document_id: str, citation: str) -> DocumentSource:
    return DocumentSource(
        document_id=document_id,
        citation=citation,
        path=f"corpus/{document_id}.md",
        section_title="Fixture",
        snapshot_date="2026-09-03",
        official_url="https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1",
        document_sha256="d" * 64,
        body="alpha beta",
        body_start_line=10,
    )


def _chunk(document: DocumentSource, suffix: str, content: str) -> ChunkRecord:
    digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
    return ChunkRecord(
        chunk_id=f"{document.document_id}-chunk-{suffix:0<16}"[:59],
        document_id=document.document_id,
        citation=document.citation,
        path=document.path,
        content=content,
        embedding_text=f"{document.citation}\nFixture\n{content}",
        start_line=10,
        end_line=10,
        start_char=0,
        end_char=len(content),
        token_count=3,
        sha256=digest,
        document_sha256=document.document_sha256,
        snapshot_date=document.snapshot_date,
        official_url=document.official_url,
        previous_id=None,
        next_id=None,
    )


def _fixtures() -> tuple[list[DocumentSource], list[ChunkRecord]]:
    left = _document("doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "1 CFR 1.1")
    right = _document("doc-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "2 CFR 2.1")
    return [left, right], [
        _chunk(right, "b", "beta legal"),
        _chunk(left, "a", "alpha legal"),
    ]


def _row_ids(database: Path) -> list[str]:
    connection = sqlite3.connect(database)
    try:
        return [
            row[0]
            for row in connection.execute("SELECT chunk_id FROM chunks ORDER BY vector_row")
        ]
    finally:
        connection.close()


def test_index_rows_follow_stable_chunk_id_order(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    progress: list[tuple[str, int, int]] = []

    summary = build_index_from_chunks(
        tmp_path / "index",
        documents,
        list(reversed(chunks)),
        DeterministicEmbedding(),
        progress=lambda phase, completed, total, **_: progress.append(
            (phase, completed, total)
        ),
    )

    assert _row_ids(summary.database) == sorted(chunk.chunk_id for chunk in chunks)
    assert summary.rows == 2
    assert summary.reused_vector_rows == 0
    assert ("index-reuse", 0, 2) in progress
    assert ("index-reuse", 2, 2) in progress
    assert ("index-embedding", 0, 2) in progress
    assert ("index-embedding", 2, 2) in progress
    assert ("index-database", 0, 1) in progress
    assert ("index-database", 1, 1) in progress
    assert ("index-vectors", 0, 1) in progress
    assert ("index-vectors", 1, 1) in progress


def test_index_has_foreign_keys_unique_rows_and_little_endian_f16(
    tmp_path: Path,
) -> None:
    documents, chunks = _fixtures()
    summary = build_index_from_chunks(
        tmp_path / "index", documents, chunks, DeterministicEmbedding()
    )

    assert summary.vectors.stat().st_size == 2 * 4 * 2
    metadata = json.loads(summary.vector_metadata.read_text("utf-8"))
    assert metadata["dtype"] == "<f2"
    assert metadata["matrix_sha256"] == hashlib.sha256(summary.vectors.read_bytes()).hexdigest()
    assert metadata["database_sha256"] == hashlib.sha256(summary.database.read_bytes()).hexdigest()
    connection = sqlite3.connect(summary.database)
    try:
        assert connection.execute("PRAGMA foreign_key_check").fetchall() == []
        assert connection.execute("PRAGMA integrity_check").fetchone() == ("ok",)
        rows = connection.execute("SELECT vector_row FROM chunks ORDER BY vector_row").fetchall()
        assert rows == [(0,), (1,)]
    finally:
        connection.close()


def test_unchanged_rows_are_reused_and_model_change_forces_full_rebuild(
    tmp_path: Path,
) -> None:
    documents, chunks = _fixtures()
    root = tmp_path / "index"
    first_backend = DeterministicEmbedding(revision="a")
    build_index_from_chunks(root, documents, chunks, first_backend)
    same_backend = DeterministicEmbedding(revision="a")
    same = build_index_from_chunks(root, documents, chunks, same_backend)

    assert same.reused_vector_rows == 2
    assert same.encoded_vector_rows == 0
    assert same_backend.calls == []

    changed = build_index_from_chunks(
        root, documents, chunks, DeterministicEmbedding(revision="b")
    )
    assert changed.reused_vector_rows == 0
    assert changed.encoded_vector_rows == 2


def test_document_and_chunk_removals_do_not_leave_stale_rows(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    root = tmp_path / "index"
    build_index_from_chunks(root, documents, chunks, DeterministicEmbedding())

    summary = build_index_from_chunks(
        root, documents[:1], [chunks[1]], DeterministicEmbedding()
    )

    assert summary.rows == 1
    assert _row_ids(summary.database) == [chunks[1].chunk_id]
    connection = sqlite3.connect(summary.database)
    try:
        assert connection.execute("SELECT COUNT(*) FROM documents").fetchone() == (1,)
    finally:
        connection.close()


def test_changed_chunk_reencodes_only_that_row(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    root = tmp_path / "index"
    build_index_from_chunks(root, documents, chunks, DeterministicEmbedding())
    changed_content = "alpha revised legal"
    changed = replace(
        chunks[1],
        content=changed_content,
        embedding_text=f"{chunks[1].citation}\nFixture\n{changed_content}",
        sha256=hashlib.sha256(changed_content.encode("utf-8")).hexdigest(),
    )
    backend = DeterministicEmbedding()

    summary = build_index_from_chunks(root, documents, [chunks[0], changed], backend)

    assert summary.reused_vector_rows == 1
    assert summary.encoded_vector_rows == 1
    assert backend.calls == [[changed.embedding_text]]


def test_failed_build_preserves_published_index(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    root = tmp_path / "index"
    summary = build_index_from_chunks(root, documents, chunks, DeterministicEmbedding())
    before = {
        path.name: path.read_bytes()
        for path in (summary.database, summary.vectors, summary.vector_metadata)
    }
    changed = replace(chunks[0], embedding_text=chunks[0].embedding_text + " changed")

    with pytest.raises(HybridIndexError, match="embedding failed"):
        build_index_from_chunks(root, documents, [changed, chunks[1]], FailingEmbedding())

    assert {name: (root / name).read_bytes() for name in before} == before


def test_schema3_index_publishes_row_count_and_sum_token_count(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    summary = build_index_from_chunks(
        tmp_path / "index", documents, chunks, DeterministicEmbedding()
    )
    vector_metadata = json.loads(summary.vector_metadata.read_text(encoding="utf-8"))
    assert vector_metadata["schema_version"] == 3
    assert vector_metadata["row_count"] == len(chunks)
    expected_total = sum(chunk.token_count for chunk in chunks)
    assert vector_metadata["sum_token_count"] == expected_total
    connection = sqlite3.connect(summary.database)
    try:
        index_names = {
            str(name[0])
            for name in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'index'"
            )
        }
        assert "chunks_vector_row_cover" in index_names
        assert (
            connection.execute(
                "SELECT value FROM metadata WHERE key = 'row_count'"
            ).fetchone()[0]
            == str(len(chunks))
        )
        assert (
            connection.execute(
                "SELECT value FROM metadata WHERE key = 'sum_token_count'"
            ).fetchone()[0]
            == str(expected_total)
        )
    finally:
        connection.close()


def test_schema3_build_rejects_invalid_token_count(tmp_path: Path) -> None:
    documents, chunks = _fixtures()
    bad = replace(chunks[0], token_count=0)
    with pytest.raises(HybridIndexError, match="chunk metadata is invalid"):
        build_index_from_chunks(
            tmp_path / "index", documents, [bad, chunks[1]], DeterministicEmbedding()
        )


def test_legacy_schema2_metadata_remains_readable_on_disk(tmp_path: Path) -> None:
    """Schema 2 packs are not auto-upgraded by the index writer.  Their
    on-disk layout must remain readable and stable."""
    documents, chunks = _fixtures()
    summary = build_index_from_chunks(
        tmp_path / "index", documents, chunks, DeterministicEmbedding()
    )
    vector_metadata = json.loads(summary.vector_metadata.read_text(encoding="utf-8"))
    legacy_metadata = {
        key: value
        for key, value in vector_metadata.items()
        if key not in {"row_count", "sum_token_count"}
    }
    legacy_metadata["schema_version"] = 2
    summary.vector_metadata.write_text(
        json.dumps(legacy_metadata, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    rebuilt = build_index_from_chunks(
        tmp_path / "index", documents, chunks, DeterministicEmbedding()
    )
    rebuilt_metadata = json.loads(rebuilt.vector_metadata.read_text(encoding="utf-8"))
    assert rebuilt_metadata["schema_version"] == 3
    assert rebuilt_metadata["row_count"] == len(chunks)

