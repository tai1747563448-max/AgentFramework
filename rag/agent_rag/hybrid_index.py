from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import tempfile
from typing import Iterable, Sequence
import uuid

import numpy as np

from .chunker import ChunkRecord, DocumentSource, chunk_documents
from .embedding import (
    BGE_M3_MODEL,
    BGE_M3_REVISION,
    EMBEDDING_DIMENSIONS,
    EmbeddingBackend,
    EmbeddingError,
    encode_normalized,
    tokenizer_fingerprint,
)
from .retriever import tokenize


class HybridIndexError(RuntimeError):
    pass


@dataclass(frozen=True)
class IndexBuildSummary:
    schema_version: int
    database: Path
    vectors: Path
    vector_metadata: Path
    rows: int
    reused_vector_rows: int
    encoded_vector_rows: int
    model: str
    revision: str
    dimensions: int
    tokenizer_sha256: str


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise HybridIndexError("index artifact could not be read") from error
    return digest.hexdigest()


def _embedding_identity(embedding: EmbeddingBackend) -> tuple[str, str, int, str]:
    model = getattr(embedding, "model", None)
    revision = getattr(embedding, "revision", None)
    dimensions = getattr(embedding, "dimensions", None)
    tokenizer = getattr(embedding, "tokenizer", None)
    if (
        type(model) is not str
        or not model
        or type(revision) is not str
        or not revision
        or type(dimensions) is not int
        or dimensions <= 0
        or tokenizer is None
    ):
        raise HybridIndexError("embedding backend identity is invalid")
    try:
        fingerprint = tokenizer_fingerprint(tokenizer)
    except EmbeddingError as error:
        raise HybridIndexError("embedding tokenizer is invalid") from error
    return model, revision, dimensions, fingerprint


def _validate_inputs(
    documents: Sequence[DocumentSource], chunks: Sequence[ChunkRecord]
) -> None:
    if not documents or not chunks:
        raise HybridIndexError("index input is empty")
    document_map: dict[str, DocumentSource] = {}
    for document in documents:
        if document.document_id in document_map:
            raise HybridIndexError("duplicate document id")
        document_map[document.document_id] = document
    seen_chunks: set[str] = set()
    chunk_map = {chunk.chunk_id: chunk for chunk in chunks}
    if len(chunk_map) != len(chunks):
        raise HybridIndexError("duplicate chunk id")
    for chunk in chunks:
        document = document_map.get(chunk.document_id)
        if (
            document is None
            or chunk.document_id in seen_chunks
            or chunk.document_sha256 != document.document_sha256
            or chunk.path != document.path
            or chunk.citation != document.citation
            or _sha256_bytes(chunk.content.encode("utf-8")) != chunk.sha256
            or not chunk.embedding_text
            or chunk.start_line <= 0
            or chunk.end_line < chunk.start_line
            or chunk.start_char < 0
            or chunk.end_char <= chunk.start_char
            or chunk.token_count <= 0
        ):
            raise HybridIndexError("chunk metadata is invalid")
        for neighbor in (chunk.previous_id, chunk.next_id):
            if neighbor is not None:
                linked = chunk_map.get(neighbor)
                if linked is None or linked.document_id != chunk.document_id:
                    raise HybridIndexError("chunk neighbor is invalid")
        seen_chunks.add(chunk.chunk_id)


def _metadata_value(connection: sqlite3.Connection, key: str) -> str | None:
    row = connection.execute("SELECT value FROM metadata WHERE key = ?", (key,)).fetchone()
    return None if row is None else str(row[0])


def _load_reusable_vectors(
    root: Path,
    *,
    model: str,
    revision: str,
    dimensions: int,
    tokenizer_sha256: str,
) -> dict[str, tuple[str, np.ndarray]]:
    database = root / "metadata.sqlite3"
    matrix_path = root / "vectors.f16"
    metadata_path = root / "vectors.json"
    if not (database.is_file() and matrix_path.is_file() and metadata_path.is_file()):
        return {}
    try:
        vector_metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if (
            type(vector_metadata) is not dict
            or vector_metadata.get("schema_version") != 2
            or vector_metadata.get("dtype") != "<f2"
            or vector_metadata.get("model") != model
            or vector_metadata.get("revision") != revision
            or vector_metadata.get("dimensions") != dimensions
            or vector_metadata.get("tokenizer_sha256") != tokenizer_sha256
            or vector_metadata.get("matrix_sha256") != _sha256_file(matrix_path)
            or vector_metadata.get("database_sha256") != _sha256_file(database)
        ):
            return {}
        rows = vector_metadata.get("rows")
        if type(rows) is not int or rows <= 0 or matrix_path.stat().st_size != rows * dimensions * 2:
            return {}
        connection = sqlite3.connect(f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True)
        try:
            connection.execute("PRAGMA query_only=ON")
            if (
                _metadata_value(connection, "schema_version") != "2"
                or _metadata_value(connection, "model") != model
                or _metadata_value(connection, "revision") != revision
                or _metadata_value(connection, "dimensions") != str(dimensions)
                or _metadata_value(connection, "tokenizer_sha256") != tokenizer_sha256
                or connection.execute("PRAGMA quick_check").fetchone() != ("ok",)
            ):
                return {}
            stored = connection.execute(
                "SELECT chunk_id, embedding_sha256, vector_row FROM chunks"
            ).fetchall()
        finally:
            connection.close()
        matrix = np.memmap(matrix_path, dtype="<f2", mode="r", shape=(rows, dimensions))
        reusable: dict[str, tuple[str, np.ndarray]] = {}
        for chunk_id, embedding_sha256, vector_row in stored:
            if type(vector_row) is not int or not 0 <= vector_row < rows:
                return {}
            reusable[str(chunk_id)] = (
                str(embedding_sha256),
                np.asarray(matrix[vector_row], dtype=np.float32).copy(),
            )
        return reusable
    except (OSError, UnicodeError, ValueError, TypeError, sqlite3.Error, HybridIndexError):
        return {}


def _write_database(
    path: Path,
    documents: Sequence[DocumentSource],
    chunks: Sequence[ChunkRecord],
    *,
    model: str,
    revision: str,
    dimensions: int,
    tokenizer_sha256: str,
) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.executescript(
            """
            PRAGMA journal_mode=OFF;
            PRAGMA synchronous=FULL;
            CREATE TABLE metadata(
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE documents(
                document_id TEXT PRIMARY KEY,
                citation TEXT NOT NULL,
                path TEXT NOT NULL UNIQUE,
                section_title TEXT NOT NULL,
                snapshot_date TEXT NOT NULL,
                official_url TEXT NOT NULL,
                sha256 TEXT NOT NULL
            ) WITHOUT ROWID;
            CREATE TABLE chunks(
                chunk_id TEXT PRIMARY KEY,
                document_id TEXT NOT NULL,
                citation TEXT NOT NULL,
                path TEXT NOT NULL,
                content TEXT NOT NULL,
                embedding_text TEXT NOT NULL,
                embedding_sha256 TEXT NOT NULL,
                start_line INTEGER NOT NULL,
                end_line INTEGER NOT NULL,
                start_char INTEGER NOT NULL,
                end_char INTEGER NOT NULL,
                token_count INTEGER NOT NULL,
                sha256 TEXT NOT NULL,
                snapshot_date TEXT NOT NULL,
                official_url TEXT NOT NULL,
                previous_id TEXT,
                next_id TEXT,
                vector_row INTEGER NOT NULL UNIQUE,
                FOREIGN KEY(document_id) REFERENCES documents(document_id),
                FOREIGN KEY(previous_id) REFERENCES chunks(chunk_id),
                FOREIGN KEY(next_id) REFERENCES chunks(chunk_id)
            );
            CREATE TABLE postings(
                term TEXT NOT NULL,
                chunk_id TEXT NOT NULL,
                tf INTEGER NOT NULL CHECK(tf > 0),
                PRIMARY KEY(term, chunk_id),
                FOREIGN KEY(chunk_id) REFERENCES chunks(chunk_id) ON DELETE CASCADE
            ) WITHOUT ROWID;
            CREATE INDEX chunks_document ON chunks(document_id);
            CREATE INDEX postings_chunk ON postings(chunk_id);
            """
        )
        connection.executemany(
            "INSERT INTO metadata VALUES(?, ?)",
            (
                ("schema_version", "2"),
                ("model", model),
                ("revision", revision),
                ("dimensions", str(dimensions)),
                ("tokenizer_sha256", tokenizer_sha256),
                ("rows", str(len(chunks))),
            ),
        )
        connection.executemany(
            "INSERT INTO documents VALUES(?, ?, ?, ?, ?, ?, ?)",
            (
                (
                    document.document_id,
                    document.citation,
                    document.path,
                    document.section_title,
                    document.snapshot_date,
                    document.official_url,
                    document.document_sha256,
                )
                for document in sorted(documents, key=lambda item: item.document_id)
            ),
        )
        for vector_row, chunk in enumerate(chunks):
            embedding_sha256 = _sha256_bytes(chunk.embedding_text.encode("utf-8"))
            connection.execute(
                "INSERT INTO chunks VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    chunk.chunk_id,
                    chunk.document_id,
                    chunk.citation,
                    chunk.path,
                    chunk.content,
                    chunk.embedding_text,
                    embedding_sha256,
                    chunk.start_line,
                    chunk.end_line,
                    chunk.start_char,
                    chunk.end_char,
                    chunk.token_count,
                    chunk.sha256,
                    chunk.snapshot_date,
                    chunk.official_url,
                    None,
                    None,
                    vector_row,
                ),
            )
            counts = Counter(tokenize(chunk.content))
            connection.executemany(
                "INSERT INTO postings VALUES(?, ?, ?)",
                ((term, chunk.chunk_id, count) for term, count in sorted(counts.items())),
            )
        connection.executemany(
            "UPDATE chunks SET previous_id = ?, next_id = ? WHERE chunk_id = ?",
            (
                (chunk.previous_id, chunk.next_id, chunk.chunk_id)
                for chunk in chunks
            ),
        )
        connection.commit()
        if connection.execute("PRAGMA foreign_key_check").fetchall():
            raise HybridIndexError("built database has broken references")
        if connection.execute("PRAGMA integrity_check").fetchone() != ("ok",):
            raise HybridIndexError("built database failed integrity check")
    except sqlite3.Error as error:
        raise HybridIndexError("database build failed") from error
    finally:
        connection.close()


def _fsync_file(path: Path) -> None:
    with path.open("r+b") as stream:
        stream.flush()
        os.fsync(stream.fileno())


def _publish_directory(staging: Path, destination: Path) -> None:
    backup = destination.with_name(destination.name + f".backup-{uuid.uuid4().hex}")
    moved_old = False
    try:
        if destination.exists():
            if not destination.is_dir():
                raise HybridIndexError("index destination is not a directory")
            os.replace(destination, backup)
            moved_old = True
        os.replace(staging, destination)
    except Exception as error:
        if moved_old and backup.exists() and not destination.exists():
            os.replace(backup, destination)
        if isinstance(error, HybridIndexError):
            raise
        raise HybridIndexError("index publication failed") from error
    if moved_old:
        shutil.rmtree(backup)


def build_index_from_chunks(
    index_root: Path,
    documents: Sequence[DocumentSource],
    chunks: Sequence[ChunkRecord],
    embedding: EmbeddingBackend,
    *,
    batch_size: int = 16,
) -> IndexBuildSummary:
    root = Path(index_root)
    if not root.is_absolute() or type(batch_size) is not int or batch_size <= 0:
        raise HybridIndexError("index build configuration is invalid")
    _validate_inputs(documents, chunks)
    model, revision, dimensions, tokenizer_sha256 = _embedding_identity(embedding)
    ordered = sorted(chunks, key=lambda item: item.chunk_id)
    reusable = _load_reusable_vectors(
        root,
        model=model,
        revision=revision,
        dimensions=dimensions,
        tokenizer_sha256=tokenizer_sha256,
    )
    vectors = np.empty((len(ordered), dimensions), dtype=np.float32)
    pending_positions: list[int] = []
    pending_texts: list[str] = []
    reused = 0
    for position, chunk in enumerate(ordered):
        embedding_sha256 = _sha256_bytes(chunk.embedding_text.encode("utf-8"))
        previous = reusable.get(chunk.chunk_id)
        if previous is not None and previous[0] == embedding_sha256:
            vectors[position] = previous[1]
            reused += 1
        else:
            pending_positions.append(position)
            pending_texts.append(chunk.embedding_text)
    if pending_texts:
        try:
            encoded = encode_normalized(
                embedding,
                pending_texts,
                dimensions=dimensions,
                batch_size=batch_size,
            )
        except EmbeddingError as error:
            raise HybridIndexError("embedding failed") from error
        for position, row in zip(pending_positions, encoded, strict=True):
            vectors[position] = row
    parent = root.parent
    parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{root.name}-build-", dir=parent))
    try:
        database = staging / "metadata.sqlite3"
        matrix_path = staging / "vectors.f16"
        metadata_path = staging / "vectors.json"
        _write_database(
            database,
            documents,
            ordered,
            model=model,
            revision=revision,
            dimensions=dimensions,
            tokenizer_sha256=tokenizer_sha256,
        )
        little_endian = np.asarray(vectors, dtype="<f2")
        with matrix_path.open("xb") as stream:
            stream.write(little_endian.tobytes(order="C"))
            stream.flush()
            os.fsync(stream.fileno())
        _fsync_file(database)
        expected_bytes = len(ordered) * dimensions * 2
        if matrix_path.stat().st_size != expected_bytes:
            raise HybridIndexError("vector matrix byte size is invalid")
        metadata = {
            "schema_version": 2,
            "dtype": "<f2",
            "rows": len(ordered),
            "dimensions": dimensions,
            "model": model,
            "revision": revision,
            "tokenizer_sha256": tokenizer_sha256,
            "matrix_sha256": _sha256_file(matrix_path),
            "database_sha256": _sha256_file(database),
        }
        with metadata_path.open("x", encoding="utf-8", newline="") as stream:
            json.dump(metadata, stream, sort_keys=True, separators=(",", ":"))
            stream.flush()
            os.fsync(stream.fileno())
        _publish_directory(staging, root)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return IndexBuildSummary(
        2,
        root / "metadata.sqlite3",
        root / "vectors.f16",
        root / "vectors.json",
        len(ordered),
        reused,
        len(pending_positions),
        model,
        revision,
        dimensions,
        tokenizer_sha256,
    )


def _load_document_sources(pack_root: Path) -> list[DocumentSource]:
    manifest_path = pack_root / "manifest" / "documents.jsonl"
    try:
        lines = manifest_path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        raise HybridIndexError("document manifest could not be read") from error
    if not lines:
        raise HybridIndexError("document manifest is empty")
    sources: list[DocumentSource] = []
    for line in lines:
        try:
            record = json.loads(line)
            relative = record["path"]
            markdown = (pack_root / Path(*relative.split("/"))).read_text(encoding="utf-8")
            front_end = markdown.index("\n---\n\n") + len("\n---\n\n")
            body_start = markdown.index("\n\n", front_end) + 2
            body = markdown[body_start:]
            if body.endswith("\n"):
                body = body[:-1]
            body_bytes = body.encode("utf-8")
            if (
                _sha256_bytes(body_bytes) != record["body_sha256"]
                or len(body_bytes) != record["body_bytes"]
            ):
                raise HybridIndexError("document body digest is invalid")
            sources.append(
                DocumentSource(
                    document_id=record["document_id"],
                    citation=record["citation"],
                    path=relative,
                    section_title=record["section_title"],
                    snapshot_date=record["snapshot_date"],
                    official_url=record["official_url"],
                    document_sha256=record["body_sha256"],
                    body=body,
                    body_start_line=markdown.count("\n", 0, body_start) + 1,
                )
            )
        except HybridIndexError:
            raise
        except (OSError, UnicodeError, ValueError, TypeError, KeyError) as error:
            raise HybridIndexError("document manifest is invalid") from error
    return sources


def build_hybrid_index(
    pack_root: Path,
    embedding: EmbeddingBackend,
    *,
    device: str,
    batch_size: int = 16,
) -> IndexBuildSummary:
    root = Path(pack_root)
    if (
        not root.is_absolute()
        or not root.is_dir()
        or device not in {"cpu", "cuda"}
        or getattr(embedding, "model", None) != BGE_M3_MODEL
        or getattr(embedding, "revision", None) != BGE_M3_REVISION
        or getattr(embedding, "dimensions", None) != EMBEDDING_DIMENSIONS
    ):
        raise HybridIndexError("hybrid index configuration is invalid")
    sources = _load_document_sources(root)
    chunks = chunk_documents(sources, getattr(embedding, "tokenizer"))
    return build_index_from_chunks(
        root / "index", sources, chunks, embedding, batch_size=batch_size
    )
