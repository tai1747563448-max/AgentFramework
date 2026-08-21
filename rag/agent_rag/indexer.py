from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import sqlite3
import stat
import tempfile

from .retriever import tokenize


class IndexBuildError(RuntimeError):
    pass


@dataclass(frozen=True)
class BuildLimits:
    max_entries: int = 50_000
    max_files: int = 10_000
    max_file_bytes: int = 1 * 1024 * 1024
    max_source_bytes: int = 64 * 1024 * 1024
    chunk_bytes: int = 4_096
    overlap_lines: int = 3


@dataclass(frozen=True)
class BuildSummary:
    schema_version: int
    files_indexed: int
    chunks_indexed: int
    files_skipped: int
    links_skipped: int


@dataclass(frozen=True)
class _Chunk:
    source_id: str
    path: str
    start_line: int
    end_line: int
    content: str
    sha256: str
    token_count: int
    term_counts: Counter[str]


_ALLOWED_SUFFIXES = {
    ".md",
    ".txt",
    ".rst",
    ".adoc",
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".py",
    ".cmake",
}
_PROTECTED_DIRECTORIES = {
    ".git",
    ".agent",
    ".rag",
    ".worktrees",
    "build",
    "out",
    "runtime_data",
    "vector_store",
    "__pycache__",
}
_PROTECTED_FILES = {
    ".env",
    ".git-credentials",
    ".netrc",
    "_netrc",
    ".npmrc",
    ".pypirc",
}


def _validate_limits(limits: BuildLimits) -> None:
    if (
        type(limits.max_entries) is not int
        or limits.max_entries <= 0
        or type(limits.max_files) is not int
        or limits.max_files <= 0
        or type(limits.max_file_bytes) is not int
        or limits.max_file_bytes <= 0
        or type(limits.max_source_bytes) is not int
        or limits.max_source_bytes <= 0
        or type(limits.chunk_bytes) is not int
        or not 4 <= limits.chunk_bytes <= 65_536
        or type(limits.overlap_lines) is not int
        or not 0 <= limits.overlap_lines <= 32
    ):
        raise IndexBuildError("build limits are invalid")


def _is_link_or_reparse(path: Path) -> bool:
    try:
        status = os.lstat(path)
    except OSError as error:
        raise IndexBuildError("path inspection failed") from error
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


def _protected_directory(name: str) -> bool:
    lowered = name.casefold()
    return (
        lowered in _PROTECTED_DIRECTORIES
        or lowered.startswith("cmake-build-")
        or lowered.startswith("build-")
    )


def _supported_file(path: Path) -> bool:
    lowered = path.name.casefold()
    if lowered in _PROTECTED_FILES or (
        lowered.startswith(".env.") and lowered != ".env.example"
    ):
        return False
    return path.name == "CMakeLists.txt" or path.suffix.casefold() in _ALLOWED_SUFFIXES


def _walk_sources(
    root: Path, limits: BuildLimits
) -> tuple[list[Path], int, int]:
    accepted: list[Path] = []
    skipped = 0
    links = 0
    entries_seen = 0
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            entries = sorted(
                os.scandir(directory), key=lambda item: (item.name.casefold(), item.name)
            )
        except OSError as error:
            raise IndexBuildError("source traversal failed") from error
        child_directories: list[Path] = []
        for entry in entries:
            entries_seen += 1
            if entries_seen > limits.max_entries:
                raise IndexBuildError("source traversal exceeds entry budget")
            path = Path(entry.path)
            try:
                linked = _is_link_or_reparse(path)
                if linked:
                    skipped += 1
                    links += 1
                    continue
                if entry.is_dir(follow_symlinks=False):
                    if _protected_directory(entry.name):
                        skipped += 1
                    else:
                        child_directories.append(path)
                    continue
                if not entry.is_file(follow_symlinks=False) or not _supported_file(path):
                    skipped += 1
                    continue
                accepted.append(path)
            except OSError as error:
                raise IndexBuildError("source entry inspection failed") from error
        pending.extend(reversed(child_directories))
    accepted.sort(
        key=lambda path: (
            path.relative_to(root).as_posix().casefold(),
            path.relative_to(root).as_posix(),
        )
    )
    return accepted, skipped, links


def _split_utf8(text: str, budget: int) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for character in text:
        encoded_size = len(character.encode("utf-8"))
        if current and current_bytes + encoded_size > budget:
            parts.append("".join(current))
            current = []
            current_bytes = 0
        current.append(character)
        current_bytes += encoded_size
    if current:
        parts.append("".join(current))
    return parts


def _make_chunk(
    relative: str,
    start_line: int,
    end_line: int,
    content: str,
    part: int | None = None,
) -> _Chunk | None:
    if not content.strip():
        return None
    terms = tokenize(content)
    if not terms:
        return None
    suffix = f"#L{start_line}-L{end_line}"
    if part is not None:
        suffix += f"-P{part}"
    encoded = content.encode("utf-8")
    return _Chunk(
        relative + suffix,
        relative,
        start_line,
        end_line,
        content,
        hashlib.sha256(encoded).hexdigest(),
        len(terms),
        Counter(terms),
    )


def _chunks(relative: str, text: str, limits: BuildLimits) -> list[_Chunk]:
    lines = text.splitlines(keepends=True)
    if not lines and text:
        lines = [text]
    chunks: list[_Chunk] = []
    current: list[tuple[int, str]] = []

    def emit_current() -> None:
        if not current:
            return
        chunk = _make_chunk(
            relative,
            current[0][0],
            current[-1][0],
            "".join(line for _, line in current),
        )
        if chunk is not None:
            chunks.append(chunk)

    for line_number, line in enumerate(lines, 1):
        line_bytes = len(line.encode("utf-8"))
        if line_bytes > limits.chunk_bytes:
            emit_current()
            current = []
            for part_number, part in enumerate(
                _split_utf8(line, limits.chunk_bytes), 1
            ):
                chunk = _make_chunk(
                    relative, line_number, line_number, part, part_number
                )
                if chunk is not None:
                    chunks.append(chunk)
            continue

        current_size = len("".join(value for _, value in current).encode("utf-8"))
        if current and current_size + line_bytes > limits.chunk_bytes:
            previous = list(current)
            emit_current()
            overlap = previous[-limits.overlap_lines :] if limits.overlap_lines else []
            while overlap and len(
                "".join(value for _, value in overlap).encode("utf-8")
            ) + line_bytes > limits.chunk_bytes:
                overlap.pop(0)
            current = overlap
        current.append((line_number, line))
    emit_current()
    return chunks


def _write_database(path: Path, chunks: list[_Chunk]) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.executescript(
            """
            PRAGMA journal_mode=OFF;
            PRAGMA synchronous=FULL;
            CREATE TABLE metadata(
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE chunks(
                source_id TEXT PRIMARY KEY,
                path TEXT NOT NULL,
                start_line INTEGER NOT NULL,
                end_line INTEGER NOT NULL,
                content TEXT NOT NULL,
                sha256 TEXT NOT NULL,
                token_count INTEGER NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE postings(
                term TEXT NOT NULL,
                source_id TEXT NOT NULL,
                tf INTEGER NOT NULL,
                PRIMARY KEY(term, source_id),
                FOREIGN KEY(source_id) REFERENCES chunks(source_id)
            ) WITHOUT ROWID;
            CREATE INDEX postings_source ON postings(source_id);
            """
        )
        connection.executemany(
            "INSERT INTO metadata(key, value) VALUES(?, ?)",
            (("schema_version", "1"),),
        )
        for chunk in chunks:
            connection.execute(
                "INSERT INTO chunks VALUES(?, ?, ?, ?, ?, ?, ?)",
                (
                    chunk.source_id,
                    chunk.path,
                    chunk.start_line,
                    chunk.end_line,
                    chunk.content,
                    chunk.sha256,
                    chunk.token_count,
                ),
            )
            connection.executemany(
                "INSERT INTO postings(term, source_id, tf) VALUES(?, ?, ?)",
                (
                    (term, chunk.source_id, frequency)
                    for term, frequency in sorted(chunk.term_counts.items())
                ),
            )
        connection.commit()
        if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
            raise IndexBuildError("built database failed validation")
    except sqlite3.Error as error:
        raise IndexBuildError("database build failed") from error
    finally:
        connection.close()


def build_index(
    source_root: Path,
    index_path: Path,
    limits: BuildLimits = BuildLimits(),
) -> BuildSummary:
    _validate_limits(limits)
    source_root = Path(source_root).absolute()
    index_path = Path(index_path).absolute()
    if (
        not source_root.exists()
        or not source_root.is_dir()
        or _has_link_component(source_root)
        or (index_path.exists() and _has_link_component(index_path))
    ):
        raise IndexBuildError("source or index path is unavailable")

    candidates, skipped, links = _walk_sources(source_root, limits)
    accepted_files = 0
    source_bytes = 0
    all_chunks: list[_Chunk] = []
    for path in candidates:
        try:
            size = path.stat().st_size
            if size > limits.max_file_bytes:
                skipped += 1
                continue
            raw = path.read_bytes()
            if len(raw) != size or b"\x00" in raw:
                skipped += 1
                continue
            text = raw.decode("utf-8", errors="strict")
        except (OSError, UnicodeError):
            skipped += 1
            continue
        accepted_files += 1
        source_bytes += len(raw)
        if accepted_files > limits.max_files or source_bytes > limits.max_source_bytes:
            raise IndexBuildError("source corpus exceeds build budget")
        relative = path.relative_to(source_root).as_posix()
        all_chunks.extend(_chunks(relative, text, limits))

    index_path.parent.mkdir(parents=True, exist_ok=True)
    if _has_link_component(index_path.parent):
        raise IndexBuildError("index parent is linked")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".agent-rag-", suffix=".sqlite.tmp", dir=index_path.parent
    )
    os.close(descriptor)
    temporary_path = Path(temporary_name)
    try:
        _write_database(temporary_path, all_chunks)
        with temporary_path.open("r+b") as database:
            os.fsync(database.fileno())
        os.replace(temporary_path, index_path)
        if os.name != "nt":
            directory_fd = os.open(index_path.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    except Exception:
        try:
            temporary_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    return BuildSummary(1, accepted_files, len(all_chunks), skipped, links)
