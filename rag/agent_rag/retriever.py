from __future__ import annotations

from collections import defaultdict
import math
import os
from pathlib import Path
import re
import sqlite3
import stat
import unicodedata
from urllib.parse import quote


class IndexQueryError(RuntimeError):
    pass


_ASCII_IDENTIFIER = re.compile(r"[A-Za-z0-9_]+")
_CAMEL_BOUNDARY = re.compile(
    r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])"
)


def _is_cjk(character: str) -> bool:
    code = ord(character)
    return (
        0x3400 <= code <= 0x4DBF
        or 0x4E00 <= code <= 0x9FFF
        or 0xF900 <= code <= 0xFAFF
        or 0x20000 <= code <= 0x2FA1F
    )


def tokenize(text: str) -> list[str]:
    normalized = unicodedata.normalize("NFKC", text)
    tokens: list[str] = []
    occupied = [False] * len(normalized)
    for match in _ASCII_IDENTIFIER.finditer(normalized):
        for index in range(match.start(), match.end()):
            occupied[index] = True
        original = match.group(0)
        folded = original.casefold()
        tokens.append(folded)
        parts: list[str] = []
        for snake_part in original.split("_"):
            if not snake_part:
                continue
            parts.extend(_CAMEL_BOUNDARY.split(snake_part))
        for part in parts:
            folded_part = part.casefold()
            if folded_part and folded_part != folded:
                tokens.append(folded_part)

    cjk_run: list[str] = []
    other_run: list[str] = []

    def flush_cjk() -> None:
        if not cjk_run:
            return
        tokens.extend(cjk_run)
        tokens.extend(
            cjk_run[index] + cjk_run[index + 1]
            for index in range(len(cjk_run) - 1)
        )
        cjk_run.clear()

    def flush_other() -> None:
        if other_run:
            tokens.append("".join(other_run).casefold())
            other_run.clear()

    for index, character in enumerate(normalized):
        if occupied[index]:
            flush_cjk()
            flush_other()
            continue
        if _is_cjk(character):
            flush_other()
            cjk_run.append(character)
        elif character.isalnum():
            flush_cjk()
            other_run.append(character)
        else:
            flush_cjk()
            flush_other()
    flush_cjk()
    flush_other()
    return tokens


def _is_link_or_reparse(path: Path) -> bool:
    try:
        status = os.lstat(path)
    except OSError as error:
        raise IndexQueryError("index path inspection failed") from error
    if stat.S_ISLNK(status.st_mode):
        return True
    if stat.S_ISREG(status.st_mode) and status.st_nlink != 1:
        return True
    attributes = getattr(status, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse)


def _has_link_component(path: Path) -> bool:
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for part in absolute.parts[1:]:
        current /= part
        if not current.exists() and not current.is_symlink():
            continue
        if _is_link_or_reparse(current):
            return True
    return False


def _readonly_connection(index_path: Path) -> sqlite3.Connection:
    if (
        not index_path.exists()
        or not index_path.is_file()
        or _has_link_component(index_path)
    ):
        raise IndexQueryError("index is unavailable")
    absolute = index_path.absolute()
    uri_path = quote(absolute.as_posix(), safe="/:")
    try:
        connection = sqlite3.connect(
            f"file:{uri_path}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        version = connection.execute(
            "SELECT value FROM metadata WHERE key = 'schema_version'"
        ).fetchone()
        if version != ("1",):
            connection.close()
            raise IndexQueryError("index schema is invalid")
        return connection
    except (sqlite3.Error, OSError) as error:
        raise IndexQueryError("index could not be opened") from error


def query_index(
    index_path: Path,
    query: str,
    *,
    top_k: int,
    max_total_bytes: int,
) -> list[dict[str, object]]:
    if (
        type(query) is not str
        or not query
        or "\x00" in query
        or len(query.encode("utf-8")) > 16_384
        or type(top_k) is not int
        or not 1 <= top_k <= 20
        or type(max_total_bytes) is not int
        or not 1 <= max_total_bytes <= 32_768
    ):
        raise IndexQueryError("query parameters are invalid")
    query_terms = list(dict.fromkeys(tokenize(query)))[:64]
    if not query_terms:
        return []

    connection = _readonly_connection(Path(index_path))
    try:
        chunk_count = connection.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        if chunk_count == 0:
            return []
        average_length = connection.execute(
            "SELECT AVG(token_count) FROM chunks"
        ).fetchone()[0]
        if average_length is None or average_length <= 0:
            return []
        placeholders = ",".join("?" for _ in query_terms)
        rows = connection.execute(
            "SELECT p.term, p.source_id, p.tf, c.token_count "
            "FROM postings AS p JOIN chunks AS c USING(source_id) "
            f"WHERE p.term IN ({placeholders})",
            query_terms,
        ).fetchall()
        if not rows:
            return []

        document_frequency: dict[str, int] = defaultdict(int)
        for term, count in connection.execute(
            "SELECT term, COUNT(*) FROM postings "
            f"WHERE term IN ({placeholders}) GROUP BY term",
            query_terms,
        ):
            document_frequency[term] = count

        scores: dict[str, float] = defaultdict(float)
        k1 = 1.5
        b = 0.75
        for term, source_id, term_frequency, document_length in rows:
            frequency = document_frequency[term]
            inverse_frequency = math.log(
                1.0 + (chunk_count - frequency + 0.5) / (frequency + 0.5)
            )
            denominator = term_frequency + k1 * (
                1.0 - b + b * document_length / average_length
            )
            scores[source_id] += inverse_frequency * (
                term_frequency * (k1 + 1.0) / denominator
            )

        ranked = sorted(scores, key=lambda item: (-scores[item], item))
        items: list[dict[str, object]] = []
        retained_bytes = 0
        for source_id in ranked:
            if len(items) >= top_k:
                break
            row = connection.execute(
                "SELECT path, start_line, end_line, content, sha256 "
                "FROM chunks WHERE source_id = ?",
                (source_id,),
            ).fetchone()
            if row is None:
                raise IndexQueryError("index references a missing chunk")
            path, start_line, end_line, content, sha256 = row
            content_bytes = len(content.encode("utf-8"))
            if retained_bytes + content_bytes > max_total_bytes:
                continue
            retained_bytes += content_bytes
            items.append(
                {
                    "source_id": source_id,
                    "content": content,
                    "metadata": {
                        "path": path,
                        "start_line": start_line,
                        "end_line": end_line,
                        "sha256": sha256,
                        "score": round(scores[source_id], 6),
                    },
                }
            )
        return items
    except (sqlite3.Error, UnicodeError, ValueError, TypeError) as error:
        raise IndexQueryError("index query failed") from error
    finally:
        connection.close()
