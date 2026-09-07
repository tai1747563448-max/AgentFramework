from __future__ import annotations

from dataclasses import dataclass, replace
import hashlib
import re
from typing import Iterable, Protocol, Sequence


_DOCUMENT_ID = re.compile(r"doc-[0-9a-f]{32}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class ChunkError(RuntimeError):
    pass


class TokenCodec(Protocol):
    def encode(self, text: str) -> Sequence[object]: ...
    def decode(self, tokens: Sequence[object]) -> str: ...


@dataclass(frozen=True)
class DocumentSource:
    document_id: str
    citation: str
    path: str
    section_title: str
    snapshot_date: str
    official_url: str
    document_sha256: str
    body: str
    body_start_line: int


@dataclass(frozen=True)
class ChunkRecord:
    chunk_id: str
    document_id: str
    citation: str
    path: str
    content: str
    embedding_text: str
    start_line: int
    end_line: int
    start_char: int
    end_char: int
    token_count: int
    sha256: str
    document_sha256: str
    snapshot_date: str
    official_url: str
    previous_id: str | None
    next_id: str | None


def _token_count(codec: TokenCodec, text: str) -> int:
    try:
        count = len(codec.encode(text))
    except Exception as error:
        raise ChunkError("tokenizer failed") from error
    if count < 0:
        raise ChunkError("tokenizer returned an invalid count")
    return count


def _validate_source(source: DocumentSource) -> None:
    if (
        _DOCUMENT_ID.fullmatch(source.document_id) is None
        or not source.citation
        or not source.path
        or source.path.startswith(("/", "\\"))
        or ".." in source.path.replace("\\", "/").split("/")
        or not source.section_title
        or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", source.snapshot_date)
        or not source.official_url.startswith("https://www.ecfr.gov/")
        or _SHA256.fullmatch(source.document_sha256) is None
        or not source.body.strip()
        or type(source.body_start_line) is not int
        or source.body_start_line <= 0
    ):
        raise ChunkError("document source is invalid")


def _prefix(source: DocumentSource) -> str:
    return f"{source.citation}\n{source.section_title}"


def _candidate_boundaries(body: str) -> list[int]:
    boundaries = {0, len(body)}
    for match in re.finditer(r"\S+(?:[ \t]+|\n*)", body):
        boundaries.add(match.end())
    for match in re.finditer(r"(?:\r?\n){2,}|(?<=[.!?。！？])(?:[ \t]+|\r?\n)", body):
        boundaries.add(match.end())
    if len(boundaries) <= 2:
        boundaries.update(range(1, len(body) + 1))
    return sorted(boundaries)


def _trim_span(body: str, start: int, end: int) -> tuple[int, int]:
    while start < end and body[start].isspace():
        start += 1
    while end > start and body[end - 1].isspace():
        end -= 1
    return start, end


def _farthest_end(
    body: str,
    prefix: str,
    codec: TokenCodec,
    boundaries: list[int],
    start: int,
    maximum: int,
) -> int:
    chosen = -1
    for boundary in boundaries:
        if boundary <= start:
            continue
        trimmed_start, trimmed_end = _trim_span(body, start, boundary)
        if trimmed_end <= trimmed_start:
            continue
        text = f"{prefix}\n{body[trimmed_start:trimmed_end]}"
        if _token_count(codec, text) <= maximum:
            chosen = boundary
            continue
        break
    if chosen >= 0:
        return chosen
    for boundary in range(start + 1, len(body) + 1):
        trimmed_start, trimmed_end = _trim_span(body, start, boundary)
        if trimmed_end <= trimmed_start:
            continue
        if _token_count(codec, f"{prefix}\n{body[trimmed_start:trimmed_end]}") > maximum:
            return max(start + 1, boundary - 1)
        chosen = boundary
    if chosen < 0:
        raise ChunkError("token budget cannot hold document text")
    return chosen


def _overlap_start(
    body: str,
    codec: TokenCodec,
    boundaries: list[int],
    current_start: int,
    current_end: int,
    overlap_tokens: int,
) -> int:
    if overlap_tokens == 0:
        return current_end
    chosen = current_end
    for boundary in reversed(boundaries):
        if boundary < current_start or boundary >= current_end:
            continue
        start, end = _trim_span(body, boundary, current_end)
        if start >= end:
            continue
        if _token_count(codec, body[start:end]) <= overlap_tokens:
            chosen = boundary
        else:
            break
    return chosen if chosen > current_start else current_end


def _line_range(source: DocumentSource, start: int, end: int) -> tuple[int, int]:
    start_line = source.body_start_line + source.body.count("\n", 0, start)
    end_line = source.body_start_line + source.body.count("\n", 0, end)
    return start_line, end_line


def chunk_document(
    source: DocumentSource,
    tokenizer: TokenCodec,
    *,
    max_tokens: int = 768,
    overlap_tokens: int = 96,
) -> list[ChunkRecord]:
    _validate_source(source)
    if (
        type(max_tokens) is not int
        or not 4 <= max_tokens <= 8192
        or type(overlap_tokens) is not int
        or overlap_tokens < 0
        or overlap_tokens >= max_tokens
    ):
        raise ChunkError("chunk limits are invalid")
    prefix = _prefix(source)
    if _token_count(tokenizer, prefix) >= max_tokens:
        raise ChunkError("document hierarchy exceeds token budget")
    body = source.body
    boundaries = _candidate_boundaries(body)
    spans: list[tuple[int, int]] = []
    start = 0
    while start < len(body):
        start, _ = _trim_span(body, start, len(body))
        if start >= len(body):
            break
        end = _farthest_end(body, prefix, tokenizer, boundaries, start, max_tokens)
        trimmed_start, trimmed_end = _trim_span(body, start, end)
        if trimmed_end <= trimmed_start:
            raise ChunkError("chunking made no progress")
        spans.append((trimmed_start, trimmed_end))
        if trimmed_end >= len(body):
            break
        next_start = _overlap_start(
            body,
            tokenizer,
            boundaries,
            trimmed_start,
            trimmed_end,
            overlap_tokens,
        )
        start = next_start if next_start > trimmed_start else trimmed_end
    if not spans:
        raise ChunkError("document produced no chunks")
    records: list[ChunkRecord] = []
    for start, end in spans:
        content = body[start:end]
        embedding_text = f"{prefix}\n{content}"
        token_count = _token_count(tokenizer, embedding_text)
        if token_count > max_tokens:
            raise ChunkError("chunk exceeded token budget")
        content_sha256 = hashlib.sha256(content.encode("utf-8")).hexdigest()
        seed = f"{source.document_id}\0{start}\0{end}\0{content_sha256}".encode("utf-8")
        chunk_id = source.document_id + "-chunk-" + hashlib.sha256(seed).hexdigest()[:16]
        start_line, end_line = _line_range(source, start, end)
        records.append(
            ChunkRecord(
                chunk_id=chunk_id,
                document_id=source.document_id,
                citation=source.citation,
                path=source.path,
                content=content,
                embedding_text=embedding_text,
                start_line=start_line,
                end_line=end_line,
                start_char=start,
                end_char=end,
                token_count=token_count,
                sha256=content_sha256,
                document_sha256=source.document_sha256,
                snapshot_date=source.snapshot_date,
                official_url=source.official_url,
                previous_id=None,
                next_id=None,
            )
        )
    linked: list[ChunkRecord] = []
    for index, record in enumerate(records):
        linked.append(
            replace(
                record,
                previous_id=records[index - 1].chunk_id if index > 0 else None,
                next_id=records[index + 1].chunk_id if index + 1 < len(records) else None,
            )
        )
    return linked


def chunk_documents(
    sources: Iterable[DocumentSource],
    tokenizer: TokenCodec,
    *,
    max_tokens: int = 768,
    overlap_tokens: int = 96,
) -> list[ChunkRecord]:
    records: list[ChunkRecord] = []
    seen: set[str] = set()
    for source in sources:
        if source.document_id in seen:
            raise ChunkError("duplicate document id")
        seen.add(source.document_id)
        records.extend(
            chunk_document(
                source,
                tokenizer,
                max_tokens=max_tokens,
                overlap_tokens=overlap_tokens,
            )
        )
    return records
