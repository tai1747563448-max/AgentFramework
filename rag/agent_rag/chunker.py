from __future__ import annotations

from bisect import bisect_left, bisect_right
from dataclasses import dataclass, replace
import hashlib
import re
from typing import Callable, Iterable, Protocol, Sequence


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
    hint: int | None = None,
) -> int:
    first = bisect_right(boundaries, start)

    def boundary_fits(index: int) -> bool:
        boundary = boundaries[index]
        trimmed_start, trimmed_end = _trim_span(body, start, boundary)
        if trimmed_end <= trimmed_start:
            return True
        text = f"{prefix}\n{body[trimmed_start:trimmed_end]}"
        return _token_count(codec, text) <= maximum

    if first < len(boundaries) and boundary_fits(first):
        chosen_index = first
        if hint is not None:
            hint_index = min(
                len(boundaries) - 1,
                max(first, bisect_right(boundaries, hint) - 1),
            )
            if hint_index > first:
                if boundary_fits(hint_index):
                    chosen_index = hint_index
                else:
                    low = first + 1
                    high = hint_index - 1
                    while low <= high:
                        middle = (low + high) // 2
                        if boundary_fits(middle):
                            chosen_index = middle
                            low = middle + 1
                        else:
                            high = middle - 1
                    return boundaries[chosen_index]
        step = 1
        failed_index: int | None = None
        while chosen_index < len(boundaries) - 1:
            probe = min(len(boundaries) - 1, chosen_index + step)
            if boundary_fits(probe):
                chosen_index = probe
                step *= 2
                continue
            failed_index = probe
            break
        if failed_index is None:
            return boundaries[chosen_index]
        low = chosen_index + 1
        high = failed_index - 1
        while low <= high:
            middle = (low + high) // 2
            if boundary_fits(middle):
                chosen_index = middle
                low = middle + 1
            else:
                high = middle - 1
        return boundaries[chosen_index]

    # Split oversized tokens with bounded exponential search.
    def character_fits(boundary: int) -> bool:
        trimmed_start, trimmed_end = _trim_span(body, start, boundary)
        if trimmed_end <= trimmed_start:
            return True
        return (
            _token_count(codec, f"{prefix}\n{body[trimmed_start:trimmed_end]}")
            <= maximum
        )

    chosen = start + 1
    if not character_fits(chosen):
        raise ChunkError("token budget cannot hold document text")
    step = 1
    failed = len(body) + 1
    while chosen < len(body):
        probe = min(len(body), chosen + step)
        if character_fits(probe):
            chosen = probe
            step *= 2
            continue
        failed = probe
        break
    low = chosen + 1
    high = failed - 1
    while low <= high:
        boundary = (low + high) // 2
        if character_fits(boundary):
            chosen = boundary
            low = boundary + 1
        else:
            high = boundary - 1
    return chosen


def _token_offsets(codec: TokenCodec, text: str) -> list[tuple[int, int]] | None:
    if not callable(codec):
        return None
    try:
        encoded = codec(  # type: ignore[operator]
            text,
            add_special_tokens=False,
            return_offsets_mapping=True,
            truncation=False,
            verbose=False,
        )
        raw_offsets = encoded["offset_mapping"]
    except (TypeError, KeyError, NotImplementedError):
        return None
    except Exception as error:
        raise ChunkError("tokenizer failed") from error
    offsets: list[tuple[int, int]] = []
    previous_start = 0
    for raw in raw_offsets:
        try:
            start, end = raw
        except (TypeError, ValueError):
            return None
        if (
            type(start) is not int
            or type(end) is not int
            or start < previous_start
            or start < 0
            or end < start
            or end > len(text)
        ):
            return None
        previous_start = start
        if end > start:
            offsets.append((start, end))
    if text and not offsets:
        return None
    return offsets


def _overlap_start(
    body: str,
    codec: TokenCodec,
    boundaries: list[int],
    current_start: int,
    current_end: int,
    overlap_tokens: int,
    hint: int | None = None,
) -> int:
    if overlap_tokens == 0:
        return current_end
    minimum = bisect_left(boundaries, current_start)
    latest = bisect_left(boundaries, current_end) - 1
    if latest < minimum:
        return current_end

    def overlap_fits(index: int) -> bool:
        boundary = boundaries[index]
        start, end = _trim_span(body, boundary, current_end)
        if start >= end:
            return True
        return _token_count(codec, body[start:end]) <= overlap_tokens

    chosen_index = latest
    if hint is not None:
        hinted = min(latest, max(minimum, bisect_left(boundaries, hint)))
        if overlap_fits(hinted):
            chosen_index = hinted
        else:
            failed = hinted
            step = 1
            found: int | None = None
            while failed < latest:
                probe = min(latest, hinted + step)
                if overlap_fits(probe):
                    found = probe
                    break
                if probe == latest:
                    break
                failed = probe
                step *= 2
            if found is None:
                return current_end
            low = failed + 1
            high = found - 1
            chosen_index = found
            while low <= high:
                middle = (low + high) // 2
                if overlap_fits(middle):
                    chosen_index = middle
                    high = middle - 1
                else:
                    low = middle + 1
            chosen = boundaries[chosen_index]
            return chosen if chosen > current_start else current_end
    elif not overlap_fits(latest):
        return current_end
    step = 1
    failed_index: int | None = None
    while chosen_index > minimum:
        probe = max(minimum, chosen_index - step)
        if overlap_fits(probe):
            chosen_index = probe
            step *= 2
            continue
        failed_index = probe
        break
    if failed_index is not None:
        low = failed_index + 1
        high = chosen_index - 1
        while low <= high:
            middle = (low + high) // 2
            if overlap_fits(middle):
                chosen_index = middle
                high = middle - 1
            else:
                low = middle + 1
    chosen = boundaries[chosen_index]
    return chosen if chosen > current_start else current_end


def _line_range(
    source: DocumentSource,
    line_breaks: Sequence[int],
    start: int,
    end: int,
) -> tuple[int, int]:
    start_line = source.body_start_line + bisect_left(line_breaks, start)
    end_line = source.body_start_line + bisect_left(line_breaks, end)
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
    prefix_tokens = _token_count(tokenizer, prefix)
    if prefix_tokens >= max_tokens:
        raise ChunkError("document hierarchy exceeds token budget")
    body = source.body
    boundaries = _candidate_boundaries(body)
    line_breaks = [match.start() for match in re.finditer("\n", body)]
    offsets = _token_offsets(tokenizer, body)
    offset_ends = [end for _, end in offsets] if offsets is not None else []
    body_budget = max(1, max_tokens - prefix_tokens)
    spans: list[tuple[int, int]] = []
    start = 0
    while start < len(body):
        start, _ = _trim_span(body, start, len(body))
        if start >= len(body):
            break
        hint = None
        if offsets:
            first_token = bisect_right(offset_ends, start)
            if first_token < len(offsets):
                hint_token = min(len(offsets) - 1, first_token + body_budget - 1)
                hint = offsets[hint_token][1]
        end = _farthest_end(
            body,
            prefix,
            tokenizer,
            boundaries,
            start,
            max_tokens,
            hint,
        )
        trimmed_start, trimmed_end = _trim_span(body, start, end)
        if trimmed_end <= trimmed_start:
            raise ChunkError("chunking made no progress")
        spans.append((trimmed_start, trimmed_end))
        if trimmed_end >= len(body):
            break
        overlap_hint = None
        if offsets:
            end_token = bisect_right(offset_ends, trimmed_end)
            if end_token > 0:
                overlap_token = max(0, end_token - overlap_tokens)
                overlap_hint = offsets[overlap_token][0]
        next_start = _overlap_start(
            body,
            tokenizer,
            boundaries,
            trimmed_start,
            trimmed_end,
            overlap_tokens,
            overlap_hint,
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
        start_line, end_line = _line_range(source, line_breaks, start, end)
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
    progress: Callable[..., None] | None = None,
) -> list[ChunkRecord]:
    prepared = tuple(sources)
    records: list[ChunkRecord] = []
    seen: set[str] = set()
    if progress is not None and prepared:
        progress("index-chunking", 0, len(prepared))
    for completed, source in enumerate(prepared, start=1):
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
        if progress is not None:
            progress("index-chunking", completed, len(prepared))
    return records
