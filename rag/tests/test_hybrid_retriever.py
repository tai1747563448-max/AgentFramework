from __future__ import annotations

from dataclasses import replace
import hashlib
from pathlib import Path
import sys

import numpy as np
import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.chunker import ChunkRecord, DocumentSource  # type: ignore[import-not-found]
from agent_rag.embedding import BGE_M3_MODEL  # type: ignore[import-not-found]
from agent_rag.hybrid_index import build_index_from_chunks  # type: ignore[import-not-found]
from agent_rag.hybrid_retriever import (  # type: ignore[import-not-found]
    EvidenceItem,
    HybridRetriever,
    HybridRetrievalError,
    reciprocal_rank_fusion,
    select_evidence,
)


class WordTokenizer:
    def encode(self, text: str) -> list[str]:
        return text.split()

    def decode(self, tokens: list[str]) -> str:
        return " ".join(tokens)

    def get_vocab(self) -> dict[str, int]:
        return {"alpha": 1, "beta": 2, "gamma": 3, "legal": 4, "query": 5}


class KeywordEmbedding:
    model = BGE_M3_MODEL
    revision = "retrieval-fixture"
    dimensions = 3
    tokenizer = WordTokenizer()

    def __init__(self) -> None:
        self.calls = 0

    def encode(self, texts: list[str], *, batch_size: int = 16) -> np.ndarray:
        del batch_size
        self.calls += 1
        rows = []
        for text in texts:
            lowered = text.casefold()
            rows.append(
                [
                    1.0 if "alpha" in lowered else 0.0,
                    1.0 if "beta" in lowered or "1 cfr" in lowered else 0.0,
                    1.0 if "gamma" in lowered or "1 cfr" in lowered else 0.0,
                ]
            )
        return np.asarray(rows, dtype=np.float32)


def _document(identifier: str, citation: str) -> DocumentSource:
    return DocumentSource(
        document_id=identifier,
        citation=citation,
        path=f"corpus/{identifier}.md",
        section_title="Fixture section",
        snapshot_date="2026-09-03",
        official_url=f"https://www.ecfr.gov/on/2026-09-03/{citation.replace(' ', '-')}",
        document_sha256=hashlib.sha256(identifier.encode()).hexdigest(),
        body="fixture body",
        body_start_line=5,
    )


def _chunk(
    document: DocumentSource,
    marker: str,
    content: str,
    *,
    previous_id: str | None = None,
    next_id: str | None = None,
) -> ChunkRecord:
    return ChunkRecord(
        chunk_id=f"{document.document_id}-chunk-{marker:0<16}"[:59],
        document_id=document.document_id,
        citation=document.citation,
        path=document.path,
        content=content,
        embedding_text=f"{document.citation}\nFixture section\n{content}",
        start_line=5,
        end_line=6,
        start_char=0,
        end_char=len(content),
        token_count=max(1, len(content.split())),
        sha256=hashlib.sha256(content.encode()).hexdigest(),
        document_sha256=document.document_sha256,
        snapshot_date=document.snapshot_date,
        official_url=document.official_url,
        previous_id=previous_id,
        next_id=next_id,
    )


def _evidence(
    chunk_id: str,
    content: str,
    *,
    document_id: str = "doc-a",
    sha256: str | None = None,
    is_neighbor: bool = False,
) -> EvidenceItem:
    return EvidenceItem(
        chunk_id=chunk_id,
        document_id=document_id,
        content=content,
        citation="1 CFR 1.1",
        path="corpus/one.md",
        start_line=1,
        end_line=2,
        snapshot_date="2026-09-03",
        official_url="https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1",
        content_sha256=sha256 or hashlib.sha256(content.encode()).hexdigest(),
        document_sha256="d" * 64,
        lexical_rank=1,
        dense_rank=1,
        fusion_score=0.1,
        is_neighbor=is_neighbor,
        neighbor_of="primary" if is_neighbor else None,
    )


def _built(tmp_path: Path) -> tuple[Path, KeywordEmbedding, list[ChunkRecord]]:
    left = _document("doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "1 CFR 1.1")
    right = _document("doc-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "2 CFR 2.1")
    first = _chunk(left, "a", "alpha legal opening")
    second = _chunk(left, "b", "context following alpha")
    first = replace(first, next_id=second.chunk_id)
    second = replace(second, previous_id=first.chunk_id)
    third = _chunk(right, "c", "beta legal rule")
    fourth = _chunk(right, "d", "gamma unrelated")
    chunks = [fourth, second, third, first]
    backend = KeywordEmbedding()
    summary = build_index_from_chunks(tmp_path / "index", [left, right], chunks, backend)
    backend.calls = 0
    return summary.database.parent, backend, chunks


def test_rrf_fuses_lexical_and_dense_with_constant_60() -> None:
    ranked = reciprocal_rank_fusion(["a", "b"], ["b", "c"], constant=60)

    assert [item.chunk_id for item in ranked] == ["b", "a", "c"]
    assert ranked[0].lexical_rank == 2
    assert ranked[0].dense_rank == 1
    assert ranked[0].score == pytest.approx(1 / 62 + 1 / 61)


def test_evidence_selection_preserves_primary_and_budget() -> None:
    primary = [_evidence("p1", "primary one"), _evidence("p2", "primary two")]
    neighbors = [_evidence("n1", "neighbor", is_neighbor=True)]

    items = select_evidence(primary, neighbors, top_k=2, max_total_bytes=100)

    assert any(not item.is_neighbor for item in items)
    assert sum(len(item.content.encode("utf-8")) for item in items) <= 100


def test_evidence_dedupes_sha_and_caps_two_primary_hits_per_document() -> None:
    duplicate_sha = "a" * 64
    primary = [
        _evidence("p1", "one", sha256=duplicate_sha),
        _evidence("p2", "same", sha256=duplicate_sha),
        _evidence("p3", "three"),
        _evidence("p4", "four"),
        _evidence("p5", "other", document_id="doc-b"),
    ]

    items = select_evidence(primary, [], top_k=5, max_total_bytes=100)

    assert [item.chunk_id for item in items] == ["p1", "p3", "p5"]


def test_evidence_never_truncates_a_paragraph_to_fit_budget() -> None:
    primary = [_evidence("large", "x" * 30), _evidence("small", "fits")]

    items = select_evidence(primary, [], top_k=2, max_total_bytes=10)

    assert [item.content for item in items] == ["fits"]


def test_lexical_mode_does_not_call_embedding_and_returns_source_metadata(
    tmp_path: Path,
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    items = retriever.query("alpha legal", top_k=3, max_total_bytes=1024, mode="lexical")

    assert backend.calls == 0
    assert items[0].citation == "1 CFR 1.1"
    assert items[0].lexical_rank == 1
    assert items[0].dense_rank is None
    assert items[0].official_url.startswith("https://www.ecfr.gov/")


def test_hybrid_dense_and_lexical_fusion_is_repeatable(tmp_path: Path) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    first = retriever.query("alpha", top_k=3, max_total_bytes=1024, mode="hybrid")
    second = retriever.query("alpha", top_k=3, max_total_bytes=1024, mode="hybrid")

    assert first == second
    assert backend.calls == 2
    assert first[0].lexical_rank is not None
    assert first[0].dense_rank is not None


def test_dense_evaluation_mode_and_raw_max_score_are_available(tmp_path: Path) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    items = retriever.query("beta", top_k=3, max_total_bytes=1024, mode="dense")

    assert items[0].dense_rank == 1
    assert items[0].lexical_rank is None
    assert retriever.max_dense_score("beta") == pytest.approx(1.0)


def test_dense_no_answer_gate_fails_closed_but_exact_citation_bypasses_it(
    tmp_path: Path,
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend, dense_min=0.9)

    assert retriever.query(
        "alpha gamma", top_k=3, max_total_bytes=1024, mode="hybrid"
    ) == []
    cited = retriever.query(
        "1 CFR § 1.1", top_k=3, max_total_bytes=1024, mode="hybrid"
    )
    assert cited
    assert cited[0].citation == "1 CFR 1.1"


@pytest.mark.parametrize(
    "query,top_k,budget,mode",
    [
        ("", 1, 10, "hybrid"),
        ("bad\x00query", 1, 10, "hybrid"),
        ("x" * 16_385, 1, 10, "hybrid"),
        ("ok", 0, 10, "hybrid"),
        ("ok", 21, 10, "hybrid"),
        ("ok", 1, 0, "hybrid"),
        ("ok", 1, 32_769, "hybrid"),
        ("ok", 1, 10, "unknown"),
    ],
)
def test_query_bounds_are_strict(
    tmp_path: Path, query: str, top_k: int, budget: int, mode: str
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    with pytest.raises(HybridRetrievalError, match="query parameters"):
        retriever.query(query, top_k=top_k, max_total_bytes=budget, mode=mode)


def test_hybrid_open_fails_closed_on_corrupt_vector_matrix(tmp_path: Path) -> None:
    root, backend, _ = _built(tmp_path)
    matrix = root / "vectors.f16"
    data = bytearray(matrix.read_bytes())
    data[0] ^= 0xFF
    matrix.write_bytes(data)

    with pytest.raises(HybridRetrievalError, match="index artifacts"):
        HybridRetriever(root, embedding=backend)
