from __future__ import annotations

from dataclasses import replace
import hashlib
from pathlib import Path
import sqlite3
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
from agent_rag import hybrid_retriever as hybrid_retriever_module  # type: ignore[import-not-found]


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
    neighbor_of: str | None = None,
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
        neighbor_of=(neighbor_of or "primary") if is_neighbor else None,
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


def test_rrf_can_apply_a_validated_dense_weight() -> None:
    ranked = reciprocal_rank_fusion(
        ["lexical", "shared"],
        ["dense", "shared"],
        constant=60,
        dense_weight=3.0,
    )

    assert [item.chunk_id for item in ranked] == ["shared", "dense", "lexical"]
    assert ranked[0].score == pytest.approx(1 / 62 + 3 / 62)
    assert ranked[1].score == pytest.approx(3 / 61)
    with pytest.raises(HybridRetrievalError, match="RRF constant"):
        reciprocal_rank_fusion([], [], dense_weight=float("nan"))


def test_evidence_selection_preserves_primary_and_budget() -> None:
    primary = [_evidence("p1", "primary one"), _evidence("p2", "primary two")]
    neighbors = [_evidence("n1", "neighbor", is_neighbor=True)]

    items = select_evidence(primary, neighbors, top_k=2, max_total_bytes=100)

    assert any(not item.is_neighbor for item in items)
    assert sum(len(item.content.encode("utf-8")) for item in items) <= 100


def test_evidence_selection_never_emits_an_orphan_neighbor() -> None:
    primary = [_evidence("p1", "primary one"), _evidence("p2", "primary two")]
    neighbors = [
        _evidence("n1", "linked neighbor", is_neighbor=True, neighbor_of="p1"),
        _evidence("n2", "orphan neighbor", is_neighbor=True, neighbor_of="missing"),
    ]

    items = select_evidence(primary, neighbors, top_k=4, max_total_bytes=100)

    selected_ids = {item.chunk_id for item in items}
    assert "n1" in selected_ids
    assert "n2" not in selected_ids
    assert all(
        not item.is_neighbor or item.neighbor_of in selected_ids for item in items
    )


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
    progress: list[tuple[str, int, int]] = []
    retriever = HybridRetriever(
        root,
        embedding=backend,
        progress=lambda phase, completed, total, **_: progress.append(
            (phase, completed, total)
        ),
    )

    items = retriever.query("alpha legal", top_k=3, max_total_bytes=1024, mode="lexical")

    assert backend.calls == 0
    assert items[0].citation == "1 CFR 1.1"
    assert items[0].lexical_rank == 1
    assert items[0].dense_rank is None
    assert items[0].official_url.startswith("https://www.ecfr.gov/")
    assert progress == [
        ("retriever-integrity", 0, 2),
        ("retriever-integrity", 1, 2),
        ("retriever-integrity", 2, 2),
        ("retriever-database", 0, 2),
        ("retriever-database", 1, 2),
        ("retriever-database", 2, 2),
        ("retriever-embedding-check", 0, 1),
        ("retriever-embedding-check", 1, 1),
    ]


def test_lexical_posting_scan_excludes_high_frequency_terms(tmp_path: Path) -> None:
    documents: list[DocumentSource] = []
    chunks: list[ChunkRecord] = []
    for index in range(8):
        document = _document(
            f"doc-{index:032x}",
            f"{index + 1} CFR {index + 1}.1",
        )
        documents.append(document)
        chunks.append(
            _chunk(
                document,
                f"{index:x}",
                "common rare beta" if index == 0 else "common beta",
            )
        )
    backend = KeywordEmbedding()
    root = build_index_from_chunks(
        tmp_path / "index", documents, chunks, backend
    ).database.parent
    retriever = HybridRetriever(root, embedding=backend)
    statements: list[str] = []
    with sqlite3.connect(root / "metadata.sqlite3") as connection:
        connection.set_trace_callback(statements.append)
        ranked = retriever._lexical(connection, "common rare")

    posting_scans = [
        statement for statement in statements if "FROM postings AS p" in statement
    ]
    assert len(posting_scans) == 1
    assert "'rare'" in posting_scans[0]
    assert "'common'" not in posting_scans[0]
    assert ranked[0] == chunks[0].chunk_id


def test_lexical_query_does_not_recompute_average_chunk_length(tmp_path: Path) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)
    statements: list[str] = []
    with sqlite3.connect(root / "metadata.sqlite3") as connection:
        connection.set_trace_callback(statements.append)
        retriever._lexical(connection, "alpha")

    assert not any("AVG(token_count)" in statement for statement in statements)


def test_integrity_check_runs_at_open_not_again_for_each_query(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root, backend, _ = _built(tmp_path)
    original = hybrid_retriever_module._readonly_connection
    checks: list[bool] = []

    def tracked(path: Path, *, verify_integrity: bool = True):
        checks.append(verify_integrity)
        return original(path, verify_integrity=verify_integrity)

    monkeypatch.setattr(hybrid_retriever_module, "_readonly_connection", tracked)
    retriever = HybridRetriever(root, embedding=backend)
    retriever.query("alpha", top_k=3, max_total_bytes=1024, mode="lexical")

    assert checks == [True, False]


def test_preverified_database_can_skip_duplicate_open_check(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root, backend, _ = _built(tmp_path)
    original = hybrid_retriever_module._readonly_connection
    checks: list[bool] = []

    def tracked(path: Path, *, verify_integrity: bool = True):
        checks.append(verify_integrity)
        return original(path, verify_integrity=verify_integrity)

    monkeypatch.setattr(hybrid_retriever_module, "_readonly_connection", tracked)
    retriever = HybridRetriever(
        root,
        embedding=backend,
        verify_database_integrity=False,
    )
    retriever.query("alpha", top_k=3, max_total_bytes=1024, mode="lexical")

    assert checks == [False, False]


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
    assert all(item.citation == "1 CFR 1.1" for item in cited)
    assert backend.calls == 2


def test_single_citation_inside_natural_language_uses_exact_route(
    tmp_path: Path,
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend, dense_min=0.9)

    cited = retriever.query(
        "请根据知识库说明 1 CFR 1.1 的要求和官方 URL。",
        top_k=3,
        max_total_bytes=1024,
        mode="hybrid",
    )

    assert cited
    assert all(item.citation == "1 CFR 1.1" for item in cited)
    assert backend.calls == 1


def test_missing_single_citation_fails_closed_without_semantic_fallback(
    tmp_path: Path,
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    for mode in ("lexical", "dense", "hybrid"):
        result = retriever.query_result(
            "根据美国联邦法规，21 CFR 11.10 对封闭系统有哪些控制要求？",
            top_k=3,
            max_total_bytes=1024,
            mode=mode,
        )

        assert result.items == []
        assert result.outcome == "authoritative_no_match"
    assert backend.calls == 0


def test_multiple_distinct_citations_are_all_covered_by_exact_route(
    tmp_path: Path,
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    items = retriever.query(
        "Compare 1 CFR 1.1 and 2 CFR 2.1.",
        top_k=3,
        max_total_bytes=1024,
        mode="hybrid",
    )

    assert {item.citation for item in items} == {"1 CFR 1.1", "2 CFR 2.1"}
    assert backend.calls == 1


@pytest.mark.parametrize("mode", ["lexical", "dense", "hybrid"])
def test_multiple_citations_fail_closed_when_any_one_is_missing(
    tmp_path: Path, mode: str
) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    result = retriever.query_result(
        "Compare 1 CFR 1.1 and 21 CFR 11.10.",
        top_k=3,
        max_total_bytes=1024,
        mode=mode,
    )

    assert result.items == []
    assert result.outcome == "authoritative_no_match"
    assert backend.calls == 0


@pytest.mark.parametrize("mode", ["lexical", "hybrid"])
def test_exact_route_can_rank_a_relevant_chunk_after_the_first_hundred(
    tmp_path: Path, mode: str
) -> None:
    document = _document("doc-cccccccccccccccccccccccccccccccc", "3 CFR 3.1")
    chunks = [
        _chunk(
            document,
            f"{index:04x}",
            "target alpha requirement" if index == 119 else f"beta boilerplate {index}",
        )
        for index in range(120)
    ]
    backend = KeywordEmbedding()
    root = build_index_from_chunks(
        tmp_path / "index", [document], chunks, backend
    ).database.parent
    backend.calls = 0
    retriever = HybridRetriever(root, embedding=backend)

    items = retriever.query(
        "What is the target alpha requirement in 3 CFR 3.1?",
        top_k=2,
        max_total_bytes=1024,
        mode=mode,
    )

    assert any("target alpha requirement" in item.content for item in items)


def test_explicit_citation_count_must_fit_evidence_top_k(tmp_path: Path) -> None:
    root, backend, _ = _built(tmp_path)
    retriever = HybridRetriever(root, embedding=backend)

    with pytest.raises(HybridRetrievalError, match="query parameters"):
        retriever.query(
            "Compare 1 CFR 1.1 and 2 CFR 2.1.",
            top_k=1,
            max_total_bytes=1024,
            mode="hybrid",
        )


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
