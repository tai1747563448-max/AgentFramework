from __future__ import annotations

from pathlib import Path
import re
import sys


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.chunker import (  # type: ignore[import-not-found]
    DocumentSource,
    chunk_document,
    chunk_documents,
)


class WordCodec:
    def encode(self, text: str) -> list[str]:
        return re.findall(r"\S+", text)

    def decode(self, tokens: list[str]) -> str:
        return " ".join(tokens)


class CountingWordCodec(WordCodec):
    def __init__(self) -> None:
        self.encode_calls = 0
        self.max_encoded_words = 0

    def encode(self, text: str) -> list[str]:
        self.encode_calls += 1
        tokens = super().encode(text)
        self.max_encoded_words = max(self.max_encoded_words, len(tokens))
        return tokens


class OffsetWordCodec(CountingWordCodec):
    def __init__(self) -> None:
        super().__init__()
        self.offset_calls = 0

    def __call__(self, text: str, **kwargs: object) -> dict[str, list[tuple[int, int]]]:
        assert kwargs["add_special_tokens"] is False
        assert kwargs["return_offsets_mapping"] is True
        self.offset_calls += 1
        return {"offset_mapping": [match.span() for match in re.finditer(r"\S+", text)]}


class InstrumentedText(str):
    def __new__(cls, value: str) -> "InstrumentedText":
        instance = super().__new__(cls, value)
        instance.count_calls = 0
        return instance

    def count(self, *args: object, **kwargs: object) -> int:
        self.count_calls += 1
        return super().count(*args, **kwargs)


def _source(document_id: str, body: str, citation: str = "1 CFR 1.1") -> DocumentSource:
    return DocumentSource(
        document_id=document_id,
        citation=citation,
        path=f"corpus/{document_id}.md",
        section_title="Definitions",
        snapshot_date="2026-09-03",
        official_url="https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1",
        document_sha256="a" * 64,
        body=body,
        body_start_line=10,
    )


def test_chunks_respect_token_limit_and_neighbor_ids_stay_in_document() -> None:
    source = _source(
        "doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "one two three four five six.\n\nseven eight nine ten eleven twelve.",
    )

    chunks = chunk_document(source, WordCodec(), max_tokens=10, overlap_tokens=2)

    assert len(chunks) >= 2
    assert all(chunk.token_count <= 10 for chunk in chunks)
    assert all(chunk.document_id == source.document_id for chunk in chunks)
    assert all(chunk.chunk_id.startswith(source.document_id + "-chunk-") for chunk in chunks)
    assert chunks[0].previous_id is None
    assert chunks[-1].next_id is None
    for left, right in zip(chunks, chunks[1:]):
        assert left.next_id == right.chunk_id
        assert right.previous_id == left.chunk_id


def test_oversized_single_sentence_uses_token_windows_with_bounded_overlap() -> None:
    source = _source(
        "doc-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu",
    )

    chunks = chunk_document(source, WordCodec(), max_tokens=9, overlap_tokens=2)

    assert len(chunks) >= 2
    for previous, current in zip(chunks, chunks[1:]):
        previous_words = previous.content.split()
        current_words = current.content.split()
        assert current_words[:2] == previous_words[-2:]
    assert all(chunk.token_count <= 9 for chunk in chunks)


def test_table_rows_remain_together_when_the_table_fits() -> None:
    table = "| Term | Meaning |\n| --- | --- |\n| A | Alpha |"
    source = _source("doc-cccccccccccccccccccccccccccccccc", f"Intro.\n\n{table}\n\nEnd.")

    chunks = chunk_document(source, WordCodec(), max_tokens=30, overlap_tokens=2)

    containing = [chunk for chunk in chunks if "| Term | Meaning |" in chunk.content]
    assert len(containing) == 1
    assert table in containing[0].content


def test_offsets_and_ids_are_repeatable() -> None:
    source = _source(
        "doc-dddddddddddddddddddddddddddddddd",
        "First paragraph.\n\nSecond paragraph.",
    )

    first = chunk_document(source, WordCodec(), max_tokens=12, overlap_tokens=1)
    second = chunk_document(source, WordCodec(), max_tokens=12, overlap_tokens=1)

    assert first == second
    assert first[0].start_line == 10
    assert first[-1].end_line == 12
    assert first[0].start_char == 0
    assert first[-1].end_char == len(source.body)


def test_chunk_documents_never_links_across_documents() -> None:
    left = _source("doc-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "one two three four five six")
    right = _source(
        "doc-ffffffffffffffffffffffffffffffff",
        "seven eight nine ten eleven twelve",
        citation="2 CFR 2.1",
    )

    chunks = chunk_documents([left, right], WordCodec(), max_tokens=8, overlap_tokens=1)

    by_document = {left.document_id: [], right.document_id: []}
    for chunk in chunks:
        by_document[chunk.document_id].append(chunk)
    assert by_document[left.document_id][-1].next_id is None
    assert by_document[right.document_id][0].previous_id is None


def test_long_document_chunk_boundary_search_is_sublinear() -> None:
    codec = CountingWordCodec()
    source = _source(
        "doc-11111111111111111111111111111111",
        " ".join(f"word-{index}" for index in range(20_000)),
    )

    chunks = chunk_document(source, codec, max_tokens=768, overlap_tokens=96)

    assert len(chunks) > 20
    assert all(chunk.token_count <= 768 for chunk in chunks)
    assert codec.encode_calls < 1_500
    assert codec.max_encoded_words < 2_000


def test_fast_tokenizer_offsets_seed_near_budget_boundaries() -> None:
    codec = OffsetWordCodec()
    body = InstrumentedText(" ".join(f"word-{index}" for index in range(20_000)))
    source = _source(
        "doc-22222222222222222222222222222222",
        body,
    )

    chunks = chunk_document(source, codec, max_tokens=768, overlap_tokens=96)

    assert len(chunks) > 20
    assert all(chunk.token_count <= 768 for chunk in chunks)
    assert codec.offset_calls == 1
    assert codec.encode_calls < 600
    assert codec.max_encoded_words < 2_000
    assert body.count_calls == 0
