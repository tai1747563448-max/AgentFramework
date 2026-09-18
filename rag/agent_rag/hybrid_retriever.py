from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
from typing import Callable, Iterable, Sequence

import numpy as np

from .embedding import EmbeddingBackend, EmbeddingError, encode_normalized, tokenizer_fingerprint
from .tokenizer import tokenize


class HybridRetrievalError(RuntimeError):
    pass


_CFR_CITATION = re.compile(
    r"(?<![0-9A-Za-z])(\d{1,2})\s*CFR\s*"
    r"(?:(?:SECTION|SEC\.?|§)\s*)?"
    r"([0-9]+(?:\.[0-9A-Za-z_-]+)*)(?![0-9A-Za-z_-])",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class FusedHit:
    chunk_id: str
    lexical_rank: int | None
    dense_rank: int | None
    score: float


@dataclass(frozen=True)
class EvidenceItem:
    chunk_id: str
    document_id: str
    content: str
    citation: str
    path: str
    start_line: int
    end_line: int
    snapshot_date: str
    official_url: str
    content_sha256: str
    document_sha256: str
    lexical_rank: int | None
    dense_rank: int | None
    fusion_score: float
    is_neighbor: bool
    neighbor_of: str | None


@dataclass(frozen=True)
class RetrievalResult:
    items: list[EvidenceItem]
    outcome: str


def reciprocal_rank_fusion(
    lexical: Sequence[str],
    dense: Sequence[str],
    *,
    constant: int = 60,
    dense_weight: float = 1.0,
) -> list[FusedHit]:
    if (
        type(constant) is not int
        or constant <= 0
        or type(dense_weight) not in {int, float}
        or not math.isfinite(float(dense_weight))
        or not 0.0 < float(dense_weight) <= 100.0
    ):
        raise HybridRetrievalError("RRF constant is invalid")
    lexical_ranks: dict[str, int] = {}
    dense_ranks: dict[str, int] = {}
    for rank, chunk_id in enumerate(lexical, 1):
        if type(chunk_id) is not str or not chunk_id:
            raise HybridRetrievalError("ranked chunk id is invalid")
        lexical_ranks.setdefault(chunk_id, rank)
    for rank, chunk_id in enumerate(dense, 1):
        if type(chunk_id) is not str or not chunk_id:
            raise HybridRetrievalError("ranked chunk id is invalid")
        dense_ranks.setdefault(chunk_id, rank)
    identifiers = lexical_ranks.keys() | dense_ranks.keys()
    hits = []
    for chunk_id in identifiers:
        lexical_rank = lexical_ranks.get(chunk_id)
        dense_rank = dense_ranks.get(chunk_id)
        score = 0.0
        if lexical_rank is not None:
            score += 1.0 / (constant + lexical_rank)
        if dense_rank is not None:
            score += float(dense_weight) / (constant + dense_rank)
        hits.append(FusedHit(chunk_id, lexical_rank, dense_rank, score))
    return sorted(
        hits,
        key=lambda item: (
            -item.score,
            min(
                rank
                for rank in (item.lexical_rank, item.dense_rank)
                if rank is not None
            ),
            item.chunk_id,
        ),
    )


def _content_bytes(item: EvidenceItem) -> int:
    return len(item.content.encode("utf-8"))


def select_evidence(
    primary_hits: Sequence[EvidenceItem],
    neighbors: Sequence[EvidenceItem],
    *,
    top_k: int,
    max_total_bytes: int,
    required_document_ids: Sequence[str] = (),
) -> list[EvidenceItem]:
    if (
        type(top_k) is not int
        or top_k <= 0
        or type(max_total_bytes) is not int
        or max_total_bytes <= 0
        or any(type(item) is not str or not item for item in required_document_ids)
        or len(set(required_document_ids)) != len(required_document_ids)
    ):
        raise HybridRetrievalError("evidence limits are invalid")
    eligible: list[EvidenceItem] = []
    seen_sha: set[str] = set()
    document_counts: dict[str, int] = defaultdict(int)
    for item in primary_hits:
        if item.is_neighbor or item.content_sha256 in seen_sha:
            continue
        if document_counts[item.document_id] >= 2:
            continue
        if _content_bytes(item) > max_total_bytes:
            continue
        eligible.append(item)
        seen_sha.add(item.content_sha256)
        document_counts[item.document_id] += 1
    if not eligible:
        return []

    selected: list[EvidenceItem] = []
    retained_bytes = 0
    selected_sha: set[str] = set()
    selected_chunk_ids: set[str] = set()

    def add(item: EvidenceItem) -> bool:
        nonlocal retained_bytes
        if (
            len(selected) >= top_k
            or item.chunk_id in selected_chunk_ids
            or item.content_sha256 in selected_sha
            or retained_bytes + _content_bytes(item) > max_total_bytes
        ):
            return False
        selected.append(item)
        selected_chunk_ids.add(item.chunk_id)
        selected_sha.add(item.content_sha256)
        retained_bytes += _content_bytes(item)
        return True

    for document_id in required_document_ids:
        required = next(
            (item for item in eligible if item.document_id == document_id), None
        )
        if required is not None:
            add(required)
    if not selected:
        add(eligible[0])
    if top_k >= 2:
        for neighbor in neighbors:
            if (
                not neighbor.is_neighbor
                or neighbor.neighbor_of not in selected_chunk_ids
            ):
                continue
            if add(neighbor):
                break
    for item in eligible[1:]:
        add(item)
    if len(selected) < top_k:
        for neighbor in neighbors:
            if neighbor.is_neighbor and neighbor.neighbor_of in selected_chunk_ids:
                add(neighbor)
    return selected


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise HybridRetrievalError("index artifacts are invalid") from error
    return digest.hexdigest()


def _read_vector_metadata(index_root: Path) -> dict[str, object]:
    path = index_root / "vectors.json"
    try:
        if path.stat().st_size > 64 * 1024:
            raise HybridRetrievalError("index artifacts are invalid")
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, TypeError) as error:
        raise HybridRetrievalError("index artifacts are invalid") from error
    if type(value) is not dict or not value:
        raise HybridRetrievalError("index artifacts are invalid")
    schema_version = value.get("schema_version")
    if schema_version == 3:
        keys = {
            "schema_version",
            "dtype",
            "rows",
            "row_count",
            "sum_token_count",
            "dimensions",
            "model",
            "revision",
            "tokenizer_sha256",
            "matrix_sha256",
            "database_sha256",
            # T6: optional backend identity written by the index builder.
            # Old vectors.json without these keys is still accepted; new ones
            # must be either both present (string) or both absent so a
            # partially-updated pack is rejected on read.
        }
        optional = {"backend", "precision"}
        if (
            type(schema_version) is not int
            or set(value) - optional != keys
            or value["dtype"] != "<f2"
            or type(value["rows"]) is not int
            or value["rows"] <= 0
            or type(value["row_count"]) is not int
            or value["row_count"] <= 0
            or value["row_count"] != value["rows"]
            or type(value["sum_token_count"]) is not int
            or value["sum_token_count"] <= 0
            or type(value["dimensions"]) is not int
            or value["dimensions"] <= 0
            or any(
                type(value[name]) is not str or not value[name]
                for name in keys
                - {"schema_version", "rows", "row_count", "sum_token_count", "dimensions"}
            )
            or ("backend" in value and "precision" not in value)
            or ("precision" in value and "backend" not in value)
        ):
            raise HybridRetrievalError("index artifacts are invalid")
        return value
    if schema_version == 2:
        keys = {
            "schema_version",
            "dtype",
            "rows",
            "dimensions",
            "model",
            "revision",
            "tokenizer_sha256",
            "matrix_sha256",
            "database_sha256",
        }
        if (
            type(schema_version) is not int
            or set(value) != keys
            or value["dtype"] != "<f2"
            or type(value["rows"]) is not int
            or value["rows"] <= 0
            or type(value["dimensions"]) is not int
            or value["dimensions"] <= 0
            or any(
                type(value[name]) is not str or not value[name]
                for name in keys - {"schema_version", "rows", "dimensions"}
            )
        ):
            raise HybridRetrievalError("index artifacts are invalid")
        # Surface legacy as a v2 record with explicit markers; the constructor
        # routes these to the slow AVG path.
        legacy = dict(value)
        legacy["legacy"] = True
        return legacy
    raise HybridRetrievalError("index artifacts are invalid")


def _readonly_connection(
    path: Path, *, verify_integrity: bool = True
) -> sqlite3.Connection:
    try:
        connection = sqlite3.connect(
            f"file:{path.as_posix()}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        if (
            verify_integrity
            and connection.execute("PRAGMA quick_check").fetchone() != ("ok",)
        ):
            raise HybridRetrievalError("index artifacts are invalid")
        return connection
    except HybridRetrievalError:
        raise
    except sqlite3.Error as error:
        raise HybridRetrievalError("index artifacts are invalid") from error


class HybridRetriever:
    def __init__(
        self,
        index_root: Path,
        *,
        embedding: EmbeddingBackend | None,
        dense_min: float = -1.0,
        scan_rows: int = 4096,
        progress: Callable[..., None] | None = None,
        verify_database_integrity: bool = True,
    ) -> None:
        root = Path(index_root)
        if (
            not root.is_absolute()
            or not root.is_dir()
            or type(dense_min) not in {int, float}
            or not math.isfinite(float(dense_min))
            or not -1.0 <= float(dense_min) <= 1.0
            or type(scan_rows) is not int
            or scan_rows <= 0
            or type(verify_database_integrity) is not bool
        ):
            raise HybridRetrievalError("retriever configuration is invalid")
        metadata = _read_vector_metadata(root)
        database = root / "metadata.sqlite3"
        vectors = root / "vectors.f16"
        try:
            if progress is not None:
                progress("retriever-integrity", 0, 2)
            matrix_sha256 = _sha256_file(vectors)
            if progress is not None:
                progress("retriever-integrity", 1, 2)
            database_sha256 = _sha256_file(database)
            if progress is not None:
                progress("retriever-integrity", 2, 2)
            if (
                metadata["matrix_sha256"] != matrix_sha256
                or metadata["database_sha256"] != database_sha256
                or vectors.stat().st_size
                != metadata["rows"] * metadata["dimensions"] * 2
            ):
                raise HybridRetrievalError("index artifacts are invalid")
        except OSError as error:
            raise HybridRetrievalError("index artifacts are invalid") from error
        if progress is not None:
            progress("retriever-database", 0, 2)
        connection = _readonly_connection(
            database, verify_integrity=verify_database_integrity
        )
        if progress is not None:
            progress("retriever-database", 1, 2)
        legacy = bool(metadata.get("legacy"))
        if legacy:
            # Schema 2 packs retain the original AVG fallback.  The retriever
            # does not auto-upgrade an old pack; that must happen via
            # scripts/upgrade_rag_index_metadata.py.
            try:
                expected = {
                    "schema_version": "2",
                    "model": metadata["model"],
                    "revision": metadata["revision"],
                    "dimensions": str(metadata["dimensions"]),
                    "tokenizer_sha256": metadata["tokenizer_sha256"],
                    "rows": str(metadata["rows"]),
                }
                actual = dict(connection.execute("SELECT key, value FROM metadata"))
                vector_rows = connection.execute(
                    "SELECT chunk_id, vector_row FROM chunks ORDER BY vector_row"
                ).fetchall()
                average_token_count = connection.execute(
                    "SELECT AVG(token_count) FROM chunks"
                ).fetchone()[0]
                if (
                    actual != expected
                    or len(vector_rows) != metadata["rows"]
                    or average_token_count is None
                    or average_token_count <= 0
                ):
                    raise HybridRetrievalError("index artifacts are invalid")
                if [row[1] for row in vector_rows] != list(range(metadata["rows"])):
                    raise HybridRetrievalError("index artifacts are invalid")
            finally:
                connection.close()
            if progress is not None:
                progress("retriever-database", 2, 2)
            self._row_ids = tuple(str(row[0]) for row in vector_rows)
            self._average_token_count = float(average_token_count)
        else:
            # Schema 3 reads statistics from metadata and the (vector_row,
            # chunk_id) mapping through the covering index.  The covering
            # index keeps the read strictly bounded by the row count and
            # avoids an AVG(token_count) full-table scan.
            try:
                metadata_rows = dict(connection.execute("SELECT key, value FROM metadata"))
                expected_keys = {
                    "schema_version",
                    "model",
                    "revision",
                    "dimensions",
                    "tokenizer_sha256",
                    "rows",
                    "row_count",
                    "sum_token_count",
                }
                missing = expected_keys - set(metadata_rows)
                if missing:
                    raise HybridRetrievalError("index artifacts are invalid")
                if (
                    metadata_rows["schema_version"] != "3"
                    or metadata_rows["model"] != metadata["model"]
                    or metadata_rows["revision"] != metadata["revision"]
                    or metadata_rows["dimensions"] != str(metadata["dimensions"])
                    or metadata_rows["tokenizer_sha256"] != metadata["tokenizer_sha256"]
                    or metadata_rows["rows"] != str(metadata["rows"])
                ):
                    raise HybridRetrievalError("index artifacts are invalid")
                row_count = int(metadata_rows["row_count"])
                sum_token_count = int(metadata_rows["sum_token_count"])
                if row_count <= 0 or row_count != metadata["rows"]:
                    raise HybridRetrievalError("index artifacts are invalid")
                if sum_token_count <= 0:
                    raise HybridRetrievalError("index artifacts are invalid")
                index_names = {
                    str(name[0])
                    for name in connection.execute(
                        "SELECT name FROM sqlite_master WHERE type = 'index'"
                    )
                }
                if "chunks_vector_row_cover" not in index_names:
                    raise HybridRetrievalError("index artifacts are invalid")
                vector_rows = connection.execute(
                    "SELECT chunk_id, vector_row FROM chunks "
                    "INDEXED BY chunks_vector_row_cover ORDER BY vector_row"
                ).fetchall()
                if len(vector_rows) != row_count:
                    raise HybridRetrievalError("index artifacts are invalid")
                if [row[1] for row in vector_rows] != list(range(row_count)):
                    raise HybridRetrievalError("index artifacts are invalid")
                observed_sum = sum(
                    int(token_count)
                    for (token_count,) in connection.execute(
                        "SELECT token_count FROM chunks "
                        "INDEXED BY chunks_vector_row_cover ORDER BY vector_row"
                    )
                )
                if observed_sum != sum_token_count:
                    raise HybridRetrievalError("index artifacts are invalid")
            finally:
                connection.close()
            if progress is not None:
                progress("retriever-database", 2, 2)
            self._row_ids = tuple(str(row[0]) for row in vector_rows)
            self._average_token_count = sum_token_count / row_count
        if embedding is not None:
            if progress is not None:
                progress("retriever-embedding-check", 0, 1)
            try:
                fingerprint = tokenizer_fingerprint(getattr(embedding, "tokenizer"))
            except (EmbeddingError, AttributeError) as error:
                raise HybridRetrievalError("embedding backend is invalid") from error
            if (
                getattr(embedding, "model", None) != metadata["model"]
                or getattr(embedding, "revision", None) != metadata["revision"]
                or getattr(embedding, "dimensions", None) != metadata["dimensions"]
                or fingerprint != metadata["tokenizer_sha256"]
            ):
                raise HybridRetrievalError("embedding backend does not match index")
            if progress is not None:
                progress("retriever-embedding-check", 1, 1)
        self._root = root
        self._database = database
        self._vectors_path = vectors
        self._metadata = metadata
        self._embedding = embedding
        self._dense_min = float(dense_min)
        self._scan_rows = scan_rows
        try:
            self._vectors = np.memmap(
                vectors,
                dtype="<f2",
                mode="r",
                shape=(metadata["rows"], metadata["dimensions"]),
            )
        except (OSError, ValueError) as error:
            raise HybridRetrievalError("index artifacts are invalid") from error

    def _lexical_scored(
        self,
        connection: sqlite3.Connection,
        query: str,
        *,
        document_ids: Sequence[str] = (),
    ) -> list[tuple[float, str]]:
        query_terms = list(dict.fromkeys(tokenize(query)))[:64]
        if not query_terms:
            return []
        chunk_count = int(self._metadata["rows"])
        placeholders = ",".join("?" for _ in query_terms)
        frequencies = dict(
            connection.execute(
                "SELECT term, COUNT(*) FROM postings "
                f"WHERE term IN ({placeholders}) GROUP BY term",
                query_terms,
            )
        )
        query_terms = [
            term
            for term in query_terms
            if term in frequencies
            and not (
                frequencies[term] >= 8
                and frequencies[term] * 4 > chunk_count
            )
        ]
        if not query_terms:
            return []
        placeholders = ",".join("?" for _ in query_terms)
        if document_ids:
            document_placeholders = ",".join("?" for _ in document_ids)
            rows = connection.execute(
                "SELECT p.term, p.chunk_id, p.tf, c.token_count "
                "FROM chunks AS c INDEXED BY chunks_document "
                "JOIN postings AS p INDEXED BY postings_chunk "
                "ON p.chunk_id = c.chunk_id "
                f"WHERE c.document_id IN ({document_placeholders}) "
                f"AND p.term IN ({placeholders})",
                [*document_ids, *query_terms],
            ).fetchall()
        else:
            rows = connection.execute(
                "SELECT p.term, p.chunk_id, p.tf, c.token_count "
                "FROM postings AS p JOIN chunks AS c USING(chunk_id) "
                f"WHERE p.term IN ({placeholders})",
                query_terms,
            ).fetchall()
        scores: dict[str, float] = defaultdict(float)
        k1 = 1.5
        b = 0.75
        for term, chunk_id, frequency, length in rows:
            document_frequency = frequencies[term]
            inverse = math.log(
                1.0
                + (chunk_count - document_frequency + 0.5)
                / (document_frequency + 0.5)
            )
            denominator = frequency + k1 * (
                1.0 - b + b * length / self._average_token_count
            )
            scores[str(chunk_id)] += inverse * frequency * (k1 + 1.0) / denominator
        return sorted(
            ((score, chunk_id) for chunk_id, score in scores.items()),
            key=lambda item: (-item[0], item[1]),
        )

    def _lexical(self, connection: sqlite3.Connection, query: str) -> list[str]:
        return [
            chunk_id
            for _, chunk_id in self._lexical_scored(connection, query)[:100]
        ]

    def _dense_scored(self, query: str) -> list[tuple[float, str]]:
        if self._embedding is None:
            raise HybridRetrievalError("hybrid mode requires an embedding backend")
        try:
            query_vector = encode_normalized(
                self._embedding,
                [query],
                dimensions=int(self._metadata["dimensions"]),
            )[0]
        except EmbeddingError as error:
            raise HybridRetrievalError("query embedding failed") from error
        candidates: list[tuple[float, str]] = []
        rows = int(self._metadata["rows"])
        for start in range(0, rows, self._scan_rows):
            end = min(rows, start + self._scan_rows)
            block = np.asarray(self._vectors[start:end], dtype=np.float32)
            scores = block @ query_vector
            if not np.isfinite(scores).all():
                raise HybridRetrievalError("dense scores are invalid")
            count = min(100, len(scores))
            if count == 0:
                continue
            if count == len(scores):
                indices = np.arange(len(scores))
            else:
                indices = np.argpartition(scores, -count)[-count:]
            for index in indices:
                score = float(scores[index])
                candidates.append((score, self._row_ids[start + int(index)]))
        candidates.sort(key=lambda item: (-item[0], item[1]))
        return candidates[:100]

    def _dense(self, query: str) -> list[str]:
        return [
            chunk_id
            for score, chunk_id in self._dense_scored(query)
            if score >= self._dense_min
        ]

    @staticmethod
    def _normalized_citations(query: str) -> tuple[str, ...]:
        matches = tuple(
            dict.fromkeys(
                f"{int(matched.group(1))} cfr {matched.group(2).casefold()}"
                for matched in _CFR_CITATION.finditer(query)
            )
        )
        if len(matches) > 8:
            raise HybridRetrievalError("query parameters are invalid")
        return matches

    @staticmethod
    def _has_exact_citation(
        connection: sqlite3.Connection, normalized: str | None
    ) -> bool:
        if normalized is None:
            return False
        return connection.execute(
            "SELECT 1 FROM documents "
            "WHERE lower(trim(citation)) = ? LIMIT 1",
            (normalized,),
        ).fetchone() is not None

    @staticmethod
    def _exact_citation_inventory(
        connection: sqlite3.Connection, normalized: Sequence[str]
    ) -> tuple[dict[str, str], list[tuple[str, str, int]]] | None:
        if not normalized:
            return {}, []
        placeholders = ",".join("?" for _ in normalized)
        document_rows = connection.execute(
            "SELECT lower(trim(citation)), document_id FROM documents "
            f"WHERE lower(trim(citation)) IN ({placeholders}) "
            "ORDER BY citation, document_id",
            list(normalized),
        ).fetchall()
        by_citation: dict[str, str] = {}
        for citation, document_id in document_rows:
            by_citation.setdefault(str(citation), str(document_id))
        if any(citation not in by_citation for citation in normalized):
            return None
        document_ids = [by_citation[citation] for citation in normalized]
        document_placeholders = ",".join("?" for _ in document_ids)
        chunks = [
            (str(chunk_id), str(document_id), int(vector_row))
            for chunk_id, document_id, vector_row in connection.execute(
                "SELECT chunk_id, document_id, vector_row FROM chunks "
                f"WHERE document_id IN ({document_placeholders}) "
                "ORDER BY vector_row",
                document_ids,
            )
        ]
        if any(
            not any(chunk[1] == document_id for chunk in chunks)
            for document_id in document_ids
        ):
            raise HybridRetrievalError("index artifacts are invalid")
        return by_citation, chunks

    def _dense_scored_subset(
        self, query: str, chunks: Sequence[tuple[str, str, int]]
    ) -> list[tuple[float, str]]:
        if self._embedding is None:
            raise HybridRetrievalError("hybrid mode requires an embedding backend")
        try:
            query_vector = encode_normalized(
                self._embedding,
                [query],
                dimensions=int(self._metadata["dimensions"]),
            )[0]
            indices = np.asarray([item[2] for item in chunks], dtype=np.int64)
            block = np.asarray(self._vectors[indices], dtype=np.float32)
            scores = block @ query_vector
        except (EmbeddingError, IndexError, TypeError, ValueError) as error:
            raise HybridRetrievalError("query embedding failed") from error
        if len(scores) != len(chunks) or not np.isfinite(scores).all():
            raise HybridRetrievalError("dense scores are invalid")
        return sorted(
            ((float(score), chunks[index][0]) for index, score in enumerate(scores)),
            key=lambda item: (-item[0], item[1]),
        )

    @staticmethod
    def _ensure_document_coverage(
        hits: Sequence[FusedHit],
        chunks: Sequence[tuple[str, str, int]],
        document_ids: Sequence[str],
    ) -> list[FusedHit]:
        chunk_documents = {chunk_id: document_id for chunk_id, document_id, _ in chunks}
        ordered: list[FusedHit] = []
        selected: set[str] = set()
        for document_id in document_ids:
            hit = next(
                (item for item in hits if chunk_documents.get(item.chunk_id) == document_id),
                None,
            )
            if hit is None:
                fallback = next(item for item in chunks if item[1] == document_id)
                hit = FusedHit(fallback[0], None, None, 0.0)
            if hit.chunk_id not in selected:
                ordered.append(hit)
                selected.add(hit.chunk_id)
        ordered.extend(item for item in hits if item.chunk_id not in selected)
        return ordered

    def max_dense_score(self, query: str) -> float:
        if (
            type(query) is not str
            or not query
            or "\x00" in query
            or len(query.encode("utf-8")) > 16_384
        ):
            raise HybridRetrievalError("query parameters are invalid")
        scored = self._dense_scored(query)
        return scored[0][0] if scored else -1.0

    @staticmethod
    def _row_to_evidence(row: Sequence[object], hit: FusedHit) -> EvidenceItem:
        return EvidenceItem(
            chunk_id=str(row[0]),
            document_id=str(row[1]),
            content=str(row[2]),
            citation=str(row[3]),
            path=str(row[4]),
            start_line=int(row[5]),
            end_line=int(row[6]),
            snapshot_date=str(row[7]),
            official_url=str(row[8]),
            content_sha256=str(row[9]),
            document_sha256=str(row[10]),
            lexical_rank=hit.lexical_rank,
            dense_rank=hit.dense_rank,
            fusion_score=hit.score,
            is_neighbor=False,
            neighbor_of=None,
        )

    def _evidence(
        self, connection: sqlite3.Connection, hits: Sequence[FusedHit]
    ) -> tuple[list[EvidenceItem], list[EvidenceItem]]:
        primary: list[EvidenceItem] = []
        neighbors: list[EvidenceItem] = []
        row_query = (
            "SELECT c.chunk_id, c.document_id, c.content, c.citation, c.path, "
            "c.start_line, c.end_line, c.snapshot_date, c.official_url, c.sha256, "
            "d.sha256, c.previous_id, c.next_id FROM chunks AS c "
            "JOIN documents AS d USING(document_id) WHERE c.chunk_id = ?"
        )
        neighbor_ids: set[str] = set()
        for hit in hits:
            row = connection.execute(row_query, (hit.chunk_id,)).fetchone()
            if row is None:
                raise HybridRetrievalError("ranked chunk is missing")
            item = self._row_to_evidence(row, hit)
            primary.append(item)
            for neighbor_id in (row[11], row[12]):
                if neighbor_id is None or neighbor_id in neighbor_ids:
                    continue
                neighbor_ids.add(str(neighbor_id))
                neighbor_row = connection.execute(row_query, (neighbor_id,)).fetchone()
                if neighbor_row is None or neighbor_row[1] != row[1]:
                    raise HybridRetrievalError("chunk neighbor is invalid")
                neighbor_hit = FusedHit(str(neighbor_id), None, None, 0.0)
                neighbors.append(
                    replace(
                        self._row_to_evidence(neighbor_row, neighbor_hit),
                        is_neighbor=True,
                        neighbor_of=hit.chunk_id,
                    )
                )
        return primary, neighbors

    def query_result(
        self,
        query: str,
        *,
        top_k: int,
        max_total_bytes: int,
        mode: str,
    ) -> RetrievalResult:
        if (
            type(query) is not str
            or not query
            or "\x00" in query
            or len(query.encode("utf-8")) > 16_384
            or type(top_k) is not int
            or not 1 <= top_k <= 20
            or type(max_total_bytes) is not int
            or not 1 <= max_total_bytes <= 32_768
            or mode not in {"lexical", "dense", "hybrid"}
        ):
            raise HybridRetrievalError("query parameters are invalid")
        # The immutable database was validated at construction.
        connection = _readonly_connection(
            self._database, verify_integrity=False
        )
        try:
            normalized_citations = self._normalized_citations(query)
            if len(normalized_citations) > top_k:
                raise HybridRetrievalError("query parameters are invalid")
            exact_inventory = self._exact_citation_inventory(
                connection, normalized_citations
            )
            if exact_inventory is None:
                return RetrievalResult([], "authoritative_no_match")
            exact_documents, exact_chunks = exact_inventory
            required_document_ids = [
                exact_documents[citation] for citation in normalized_citations
            ]
            if exact_chunks:
                lexical_scores = (
                    self._lexical_scored(
                        connection,
                        query,
                        document_ids=required_document_ids,
                    )
                    if mode != "dense"
                    else []
                )
                lexical = [chunk_id for _, chunk_id in lexical_scores]
                if mode == "lexical" and not lexical:
                    lexical = [chunk_id for chunk_id, _, _ in exact_chunks]
                dense_scores = (
                    self._dense_scored_subset(query, exact_chunks)
                    if mode != "lexical"
                    else []
                )
            else:
                lexical = self._lexical(connection, query) if mode != "dense" else []
                dense_scores = [] if mode == "lexical" else self._dense_scored(query)
            if (
                dense_scores
                and dense_scores[0][0] < self._dense_min
                and not normalized_citations
            ):
                return RetrievalResult([], "no_match")
            dense = [chunk_id for _, chunk_id in dense_scores]
            # Weight tuned on frozen development data; BM25 handles exact terms.
            hits = reciprocal_rank_fusion(
                lexical, dense, constant=60, dense_weight=3.0
            )
            if exact_chunks:
                hits = self._ensure_document_coverage(
                    hits, exact_chunks, required_document_ids
                )
            primary, neighbors = self._evidence(connection, hits)
            selected = select_evidence(
                primary,
                neighbors,
                top_k=top_k,
                max_total_bytes=max_total_bytes,
                required_document_ids=required_document_ids,
            )
            return RetrievalResult(selected, "matched" if selected else "no_match")
        except HybridRetrievalError:
            raise
        except (sqlite3.Error, UnicodeError, ValueError, TypeError) as error:
            raise HybridRetrievalError("retrieval query failed") from error
        finally:
            connection.close()

    def query(
        self,
        query: str,
        *,
        top_k: int,
        max_total_bytes: int,
        mode: str,
    ) -> list[EvidenceItem]:
        return self.query_result(
            query,
            top_k=top_k,
            max_total_bytes=max_total_bytes,
            mode=mode,
        ).items
