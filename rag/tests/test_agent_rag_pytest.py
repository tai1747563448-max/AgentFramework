from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.indexer import build_index  # noqa: E402
from agent_rag.protocol import ProtocolError, parse_query_request  # noqa: E402
from agent_rag.retriever import query_index  # noqa: E402


def query_request(query: str) -> str:
    return json.dumps(
        {
            "schema_version": 1,
            "query": query,
            "top_k": 5,
            "max_total_bytes": 32_768,
        },
        ensure_ascii=False,
    )


@pytest.mark.parametrize(
    "query",
    [
        pytest.param("unsafe\x00query", id="nul-byte"),
        pytest.param("x" * 65_536, id="oversized-request"),
    ],
)
def test_query_request_rejects_unsafe_query_boundaries(query: str) -> None:
    with pytest.raises(ProtocolError):
        parse_query_request(query_request(query))


def test_index_round_trip_uses_pytest_tmp_path(tmp_path: Path) -> None:
    source = tmp_path / "知识库"
    source.mkdir()
    (source / "pytest指南.md").write_text(
        "pytest fixture validates a real BM25 index round trip.\n",
        encoding="utf-8",
        newline="",
    )
    index = tmp_path / "index" / "knowledge.sqlite"

    summary = build_index(source, index)
    items = query_index(
        index,
        "pytest fixture",
        top_k=3,
        max_total_bytes=32_768,
    )

    assert summary.files_indexed == 1
    assert [item["metadata"]["path"] for item in items] == ["pytest指南.md"]
    assert "BM25 index round trip" in items[0]["content"]
