from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, replace
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
from typing import Iterable, Sequence

import numpy as np

from .embedding import EmbeddingBackend, EmbeddingError, encode_normalized, tokenizer_fingerprint
from .retriever import tokenize


class HybridRetrievalError(RuntimeError):
    pass


_CFR_CITATION = re.compile(
    r"\s*(\d{1,2})\s*CFR\s*(?:(?:SECTION|SEC\.?|§)\s*)?"
    r"([0-9]+(?:\.[0-9A-Za-z_-]+)*)\s*\Z",
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


def reciprocal_rank_fusion(
    lexical: Sequence[str], dense: Sequence[str], *, constant: int = 60
) -> list[FusedHit]:
    if type(constant) is not int or constant <= 0:
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
            score += 1.0 / (constant + dense_rank)
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
) -> list[EvidenceItem]:
    if (
        type(top_k) is not int
        or top_k <= 0
        or type(max_total_bytes) is not int
        or max_total_bytes <= 0
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

    def add(item: EvidenceItem) -> bool:
        nonlocal retained_bytes
        if (
            len(selected) >= top_k
            or item.content_sha256 in selected_sha
            or retained_bytes + _content_bytes(item) > max_total_bytes
        ):
            return False
        selected.append(item)
        selected_sha.add(item.content_sha256)
        retained_bytes += _content_bytes(item)
        return True

    add(eligible[0])
    if top_k >= 2:
        primary_ids = {item.chunk_id for item in eligible}
        for neighbor in neighbors:
            if (
                not neighbor.is_neighbor
                or neighbor.neighbor_of not in primary_ids
                or neighbor.chunk_id in primary_ids
            ):
                continue
            if add(neighbor):
                break
    for item in eligible[1:]:
        add(item)
    if len(selected) < top_k:
        for neighbor in neighbors:
            if neighbor.is_neighbor:
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
        type(value) is not dict
        or set(value) != keys
        or value["schema_version"] != 2
        or type(value["schema_version"]) is not int
        or value["dtype"] != "<f2"
        or type(value["rows"]) is not int
        or value["rows"] <= 0
        or type(value["dimensions"]) is not int
        or value["dimensions"] <= 0
        or any(type(value[name]) is not str or not value[name] for name in keys - {"schema_version", "rows", "dimensions"})
    ):
        raise HybridRetrievalError("index artifacts are invalid")
    return value


def _readonly_connection(path: Path) -> sqlite3.Connection:
    try:
        connection = sqlite3.connect(
            f"file:{path.as_posix()}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
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
        ):
            raise HybridRetrievalError("retriever configuration is invalid")
        metadata = _read_vector_metadata(root)
        database = root / "metadata.sqlite3"
        vectors = root / "vectors.f16"
        try:
            if (
                metadata["matrix_sha256"] != _sha256_file(vectors)
                or metadata["database_sha256"] != _sha256_file(database)
                or vectors.stat().st_size
                != metadata["rows"] * metadata["dimensions"] * 2
            ):
                raise HybridRetrievalError("index artifacts are invalid")
        except OSError as error:
            raise HybridRetrievalError("index artifacts are invalid") from error
        connection = _readonly_connection(database)
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
            if actual != expected or len(vector_rows) != metadata["rows"]:
                raise HybridRetrievalError("index artifacts are invalid")
            if [row[1] for row in vector_rows] != list(range(metadata["rows"])):
                raise HybridRetrievalError("index artifacts are invalid")
        finally:
            connection.close()
        if embedding is not None:
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
        self._root = root
        self._database = database
        self._vectors_path = vectors
        self._metadata = metadata
        self._embedding = embedding
        self._dense_min = float(dense_min)
        self._scan_rows = scan_rows
        self._row_ids = tuple(str(row[0]) for row in vector_rows)
        try:
            self._vectors = np.memmap(
                vectors,
                dtype="<f2",
                mode="r",
                shape=(metadata["rows"], metadata["dimensions"]),
            )
        except (OSError, ValueError) as error:
            raise HybridRetrievalError("index artifacts are invalid") from error

    def _lexical(self, connection: sqlite3.Connection, query: str) -> list[str]:
        query_terms = list(dict.fromkeys(tokenize(query)))[:64]
        if not query_terms:
            return []
        chunk_count = int(self._metadata["rows"])
        average = connection.execute("SELECT AVG(token_count) FROM chunks").fetchone()[0]
        if average is None or average <= 0:
            return []
        placeholders = ",".join("?" for _ in query_terms)
        rows = connection.execute(
            "SELECT p.term, p.chunk_id, p.tf, c.token_count "
            "FROM postings AS p JOIN chunks AS c USING(chunk_id) "
            f"WHERE p.term IN ({placeholders})",
            query_terms,
        ).fetchall()
        frequencies = dict(
            connection.execute(
                "SELECT term, COUNT(*) FROM postings "
                f"WHERE term IN ({placeholders}) GROUP BY term",
                query_terms,
            )
        )
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
            denominator = frequency + k1 * (1.0 - b + b * length / average)
            scores[str(chunk_id)] += inverse * frequency * (k1 + 1.0) / denominator
        return sorted(scores, key=lambda chunk_id: (-scores[chunk_id], chunk_id))[:100]

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
    def _normalized_citation(query: str) -> str | None:
        matched = _CFR_CITATION.fullmatch(query)
        if matched is None:
            return None
        return f"{int(matched.group(1))} cfr {matched.group(2).casefold()}"

    @staticmethod
    def _has_exact_citation(
        connection: sqlite3.Connection, normalized: str | None
    ) -> bool:
        if normalized is None:
            return False
        rows = connection.execute("SELECT citation FROM documents").fetchall()
        return any(str(row[0]).strip().casefold() == normalized for row in rows)

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

    def query(
        self,
        query: str,
        *,
        top_k: int,
        max_total_bytes: int,
        mode: str,
    ) -> list[EvidenceItem]:
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
        connection = _readonly_connection(self._database)
        try:
            lexical = self._lexical(connection, query) if mode != "dense" else []
            dense_scores = self._dense_scored(query) if mode != "lexical" else []
            if dense_scores and dense_scores[0][0] < self._dense_min:
                if not self._has_exact_citation(
                    connection, self._normalized_citation(query)
                ):
                    return []
            dense = [chunk_id for _, chunk_id in dense_scores]
            hits = reciprocal_rank_fusion(lexical, dense, constant=60)
            primary, neighbors = self._evidence(connection, hits)
            return select_evidence(
                primary,
                neighbors,
                top_k=top_k,
                max_total_bytes=max_total_bytes,
            )
        except HybridRetrievalError:
            raise
        except (sqlite3.Error, UnicodeError, ValueError, TypeError) as error:
            raise HybridRetrievalError("retrieval query failed") from error
        finally:
            connection.close()
