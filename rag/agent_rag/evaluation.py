from __future__ import annotations

from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import re
import sqlite3
import time
import tracemalloc
from typing import Any, Iterable, Mapping, Sequence


CASE_SCHEMA_VERSION = 2
REPORT_SCHEMA_VERSION = 2
NEGATIVE_KINDS = frozenset({"no_answer", "injection_damaged"})
CASE_KINDS = frozenset(
    {
        "citation",
        "paraphrase_en",
        "cross_language_zh",
        "multi_concept",
        "adjacent_context",
        *NEGATIVE_KINDS,
    }
)
_CASE_KEYS = {
    "case_id",
    "split",
    "language",
    "query",
    "relevant_document_ids",
    "relevant_chunk_ids",
    "kind",
    "rationale",
}
_CASE_ID = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
_DOCUMENT_ID = re.compile(r"doc-[0-9a-f]{32}\Z")
_CHUNK_ID = re.compile(r"doc-[0-9a-f]{32}-chunk-[0-9a-f]{16}\Z")


class EvaluationError(RuntimeError):
    pass


@dataclass(frozen=True)
class EvaluationCase:
    case_id: str
    split: str
    language: str
    query: str
    relevant_document_ids: tuple[str, ...]
    relevant_chunk_ids: tuple[str, ...]
    kind: str
    rationale: str

    @property
    def is_negative(self) -> bool:
        return self.kind in NEGATIVE_KINDS


@dataclass(frozen=True)
class CaseScore:
    recall_at_5: float
    recall_at_10: float
    reciprocal_rank: float
    ndcg_at_10: float
    no_answer_false_positive: bool
    positive: bool


@dataclass(frozen=True)
class AggregateScore:
    recall_at_5: float
    recall_at_10: float
    mean_reciprocal_rank: float
    ndcg_at_10: float
    no_answer_false_positive_rate: float
    positive_case_count: int
    negative_case_count: int


@dataclass(frozen=True)
class DenseThreshold:
    threshold: float
    recall: float
    false_positive_rate: float
    positive_case_count: int
    negative_case_count: int


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def _load_json_line(raw: bytes) -> Any:
    if not raw or len(raw) > 64 * 1024 or b"\x00" in raw:
        raise EvaluationError("case JSON is invalid")
    try:
        return json.loads(
            raw.decode("utf-8", errors="strict"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite")),
        )
    except (UnicodeError, ValueError, TypeError, json.JSONDecodeError) as error:
        raise EvaluationError("case JSON is invalid") from error


def _identifier_list(value: Any, pattern: re.Pattern[str]) -> tuple[str, ...]:
    if type(value) is not list or len(value) > 100:
        raise EvaluationError("case record is invalid")
    result: list[str] = []
    seen: set[str] = set()
    for item in value:
        if type(item) is not str or pattern.fullmatch(item) is None or item in seen:
            raise EvaluationError("case record is invalid")
        seen.add(item)
        result.append(item)
    return tuple(result)


def _parse_case(value: Any) -> EvaluationCase:
    if type(value) is not dict or set(value) != _CASE_KEYS:
        raise EvaluationError("case record is invalid")
    if (
        type(value["case_id"]) is not str
        or _CASE_ID.fullmatch(value["case_id"]) is None
        or value["split"] not in {"development", "holdout"}
        or type(value["split"]) is not str
        or value["language"] not in {"en", "zh"}
        or type(value["language"]) is not str
        or value["kind"] not in CASE_KINDS
        or type(value["kind"]) is not str
        or type(value["query"]) is not str
        or not value["query"].strip()
        or "\x00" in value["query"]
        or len(value["query"].encode("utf-8")) > 16_384
        or type(value["rationale"]) is not str
        or not value["rationale"].strip()
        or len(value["rationale"].encode("utf-8")) > 4_096
    ):
        raise EvaluationError("case record is invalid")
    documents = _identifier_list(value["relevant_document_ids"], _DOCUMENT_ID)
    chunks = _identifier_list(value["relevant_chunk_ids"], _CHUNK_ID)
    negative = value["kind"] in NEGATIVE_KINDS
    if negative and (documents or chunks):
        raise EvaluationError("negative case must not contain labels")
    if not negative and not documents and not chunks:
        raise EvaluationError("positive case requires labels")
    if value["kind"] == "cross_language_zh" and value["language"] != "zh":
        raise EvaluationError("case record is invalid")
    if value["kind"] == "paraphrase_en" and value["language"] != "en":
        raise EvaluationError("case record is invalid")
    return EvaluationCase(
        value["case_id"],
        value["split"],
        value["language"],
        value["query"],
        documents,
        chunks,
        value["kind"],
        value["rationale"],
    )


def load_cases(path: Path) -> list[EvaluationCase]:
    source = Path(path)
    try:
        if not source.is_file() or source.is_symlink() or source.stat().st_size > 64 * 1024 * 1024:
            raise EvaluationError("case file is invalid")
        lines = source.read_bytes().splitlines()
    except OSError as error:
        raise EvaluationError("case file is invalid") from error
    if not lines:
        raise EvaluationError("case file is invalid")
    cases: list[EvaluationCase] = []
    seen: set[str] = set()
    for raw in lines:
        item = _parse_case(_load_json_line(raw))
        if item.case_id in seen:
            raise EvaluationError("duplicate case id")
        seen.add(item.case_id)
        cases.append(item)
    development = [item for item in cases if item.split == "development"]
    holdout = [item for item in cases if item.split == "holdout"]
    validate_splits(development, holdout)
    return cases


def validate_splits(
    development: Sequence[EvaluationCase], holdout: Sequence[EvaluationCase]
) -> None:
    development_ids = {item.case_id for item in development}
    holdout_ids = {item.case_id for item in holdout}
    if len(development_ids) != len(development) or len(holdout_ids) != len(holdout):
        raise EvaluationError("duplicate case id")
    if development_ids & holdout_ids:
        raise EvaluationError("case split overlap")


def validate_case_labels(
    cases: Sequence[EvaluationCase],
    *,
    documents: set[str],
    chunks: Mapping[str, str],
) -> None:
    for item in cases:
        for document_id in item.relevant_document_ids:
            if document_id not in documents:
                raise EvaluationError("unknown document label")
        for chunk_id in item.relevant_chunk_ids:
            if chunk_id not in chunks:
                raise EvaluationError("unknown chunk label")
            if (
                item.relevant_document_ids
                and chunks[chunk_id] not in item.relevant_document_ids
            ):
                raise EvaluationError("chunk label does not match document label")


def validate_final_composition(cases: Sequence[EvaluationCase]) -> None:
    minimums = {
        "citation": 60,
        "paraphrase_en": 60,
        "cross_language_zh": 60,
        "multi_concept": 40,
        "adjacent_context": 30,
        "no_answer": 25,
        "injection_damaged": 25,
    }
    counts = {kind: 0 for kind in minimums}
    for item in cases:
        counts[item.kind] = counts.get(item.kind, 0) + 1
    development = [item for item in cases if item.split == "development"]
    holdout = [item for item in cases if item.split == "holdout"]
    validate_splits(development, holdout)
    if (
        len(cases) < 300
        or not development
        or not holdout
        or any(counts[kind] < count for kind, count in minimums.items())
        or any(item.language != "zh" for item in cases if item.kind == "cross_language_zh")
        or any(item.language != "en" for item in cases if item.kind == "paraphrase_en")
    ):
        raise EvaluationError("evaluation composition is invalid")


def score_case(*, relevant: set[str], ranked: Sequence[str]) -> CaseScore:
    ordered: list[str] = []
    seen: set[str] = set()
    for identifier in ranked:
        if identifier not in seen:
            seen.add(identifier)
            ordered.append(identifier)
    if not relevant:
        return CaseScore(0.0, 0.0, 0.0, 0.0, bool(ordered), False)
    recall_at_5 = len(relevant.intersection(ordered[:5])) / len(relevant)
    recall_at_10 = len(relevant.intersection(ordered[:10])) / len(relevant)
    reciprocal_rank = next(
        (1.0 / rank for rank, identifier in enumerate(ordered, 1) if identifier in relevant),
        0.0,
    )
    dcg = sum(
        1.0 / math.log2(rank + 1)
        for rank, identifier in enumerate(ordered[:10], 1)
        if identifier in relevant
    )
    ideal = sum(
        1.0 / math.log2(rank + 1)
        for rank in range(1, min(len(relevant), 10) + 1)
    )
    return CaseScore(
        recall_at_5,
        recall_at_10,
        reciprocal_rank,
        dcg / ideal,
        False,
        True,
    )


def aggregate_scores(scores: Sequence[CaseScore]) -> AggregateScore:
    positives = [item for item in scores if item.positive]
    negatives = [item for item in scores if not item.positive]

    def mean(values: Iterable[float]) -> float:
        collected = list(values)
        return sum(collected) / len(collected) if collected else 0.0

    return AggregateScore(
        mean(item.recall_at_5 for item in positives),
        mean(item.recall_at_10 for item in positives),
        mean(item.reciprocal_rank for item in positives),
        mean(item.ndcg_at_10 for item in positives),
        mean(float(item.no_answer_false_positive) for item in negatives),
        len(positives),
        len(negatives),
    )


def fit_dense_threshold(
    scored_cases: Sequence[tuple[EvaluationCase, float]],
    *,
    maximum_false_positive_rate: float,
) -> DenseThreshold:
    if (
        not scored_cases
        or type(maximum_false_positive_rate) not in {int, float}
        or not 0.0 <= float(maximum_false_positive_rate) <= 1.0
        or any(
            type(score) not in {int, float}
            or not math.isfinite(float(score))
            or not -1.0 <= float(score) <= 1.0
            for _, score in scored_cases
        )
    ):
        raise EvaluationError("dense threshold inputs are invalid")
    positives = [(item, float(score)) for item, score in scored_cases if not item.is_negative]
    negatives = [(item, float(score)) for item, score in scored_cases if item.is_negative]
    if not positives or not negatives:
        raise EvaluationError("dense threshold requires positive and negative cases")
    candidates = sorted({score for _, score in scored_cases}, reverse=True)
    eligible: list[DenseThreshold] = []
    for threshold in candidates:
        recall = sum(score >= threshold for _, score in positives) / len(positives)
        false_positive_rate = (
            sum(score >= threshold for _, score in negatives) / len(negatives)
        )
        if false_positive_rate <= maximum_false_positive_rate:
            eligible.append(
                DenseThreshold(
                    threshold,
                    recall,
                    false_positive_rate,
                    len(positives),
                    len(negatives),
                )
            )
    if not eligible:
        raise EvaluationError("no dense threshold satisfies false-positive limit")
    return sorted(eligible, key=lambda item: (-item.recall, -item.threshold))[0]


def percentile(values: Sequence[float], quantile: float, *, warmup: int = 0) -> float:
    if (
        type(warmup) is not int
        or warmup < 0
        or type(quantile) not in {int, float}
        or not 0.0 < float(quantile) <= 1.0
        or any(type(value) not in {int, float} or not math.isfinite(float(value)) for value in values)
    ):
        raise EvaluationError("percentile inputs are invalid")
    measured = sorted(float(value) for value in values[warmup:])
    if not measured:
        raise EvaluationError("warm-up removed every latency sample")
    index = max(0, math.ceil(float(quantile) * len(measured)) - 1)
    return measured[index]


def stable_report_json(value: Any) -> str:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ) + "\n"
    except (TypeError, ValueError, UnicodeError) as error:
        raise EvaluationError("report is invalid") from error


def _case_ranked_ids(item: EvaluationCase, hits: Sequence[Any]) -> list[str]:
    if item.relevant_chunk_ids:
        return [str(hit.chunk_id) for hit in hits]
    return [str(hit.document_id) for hit in hits]


def _case_relevant_ids(item: EvaluationCase) -> set[str]:
    return set(
        item.relevant_chunk_ids
        if item.relevant_chunk_ids
        else item.relevant_document_ids
    )


def _exact_citation_bypass(query: str, hits: Sequence[Any]) -> bool:
    matched = re.fullmatch(
        r"\s*(\d{1,2})\s*CFR\s*(?:(?:SECTION|SEC\.?|§)\s*)?"
        r"([0-9]+(?:\.[0-9A-Za-z_-]+)*)\s*",
        query,
        flags=re.IGNORECASE,
    )
    if matched is None:
        return False
    normalized = f"{int(matched.group(1))} cfr {matched.group(2).casefold()}"
    return any(str(hit.citation).strip().casefold() == normalized for hit in hits)


def _aggregate_dict(scores: Sequence[CaseScore]) -> dict[str, Any]:
    return asdict(aggregate_scores(scores))


def evaluate_cases(
    cases: Sequence[EvaluationCase],
    retriever: Any,
    *,
    mode: str,
    identity: Mapping[str, Any],
    warmup: int = 1,
) -> dict[str, Any]:
    if mode not in {"lexical", "dense", "hybrid"} or not cases:
        raise EvaluationError("evaluation configuration is invalid")
    development = [item for item in cases if item.split == "development"]
    holdout = [item for item in cases if item.split == "holdout"]
    validate_splits(development, holdout)
    if not development or not holdout:
        raise EvaluationError("evaluation requires development and holdout cases")

    dense_scores: dict[str, float] = {}
    fitted: DenseThreshold | None = None
    if mode in {"dense", "hybrid"}:
        for item in cases:
            score = retriever.max_dense_score(item.query)
            if type(score) not in {int, float} or not math.isfinite(float(score)):
                raise EvaluationError("retriever returned an invalid dense score")
            dense_scores[item.case_id] = float(score)
        fitted = fit_dense_threshold(
            [(item, dense_scores[item.case_id]) for item in development],
            maximum_false_positive_rate=0.05,
        )

    for index in range(warmup):
        item = cases[index % len(cases)]
        retriever.query(
            item.query,
            top_k=10,
            max_total_bytes=32_768,
            mode=mode,
        )

    rows: list[dict[str, Any]] = []
    scores_by_split: dict[str, list[CaseScore]] = {
        "development": [],
        "holdout": [],
    }
    latencies: list[float] = []
    tracemalloc.start()
    try:
        for item in cases:
            started = time.perf_counter_ns()
            hits = retriever.query(
                item.query,
                top_k=10,
                max_total_bytes=32_768,
                mode=mode,
            )
            latency_ms = (time.perf_counter_ns() - started) / 1_000_000.0
            latencies.append(latency_ms)
            if (
                fitted is not None
                and dense_scores[item.case_id] < fitted.threshold
                and not _exact_citation_bypass(item.query, hits)
            ):
                hits = []
            ranked = _case_ranked_ids(item, hits)
            score = score_case(relevant=_case_relevant_ids(item), ranked=ranked)
            scores_by_split[item.split].append(score)
            rows.append(
                {
                    "case_id": item.case_id,
                    "split": item.split,
                    "kind": item.kind,
                    "language": item.language,
                    "ranked_chunk_ids": [str(hit.chunk_id) for hit in hits],
                    "ranked_document_ids": [str(hit.document_id) for hit in hits],
                    "dense_max_score": dense_scores.get(item.case_id),
                    "latency_ms": latency_ms,
                    "score": asdict(score),
                }
            )
        _, peak_bytes = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()

    all_scores = scores_by_split["development"] + scores_by_split["holdout"]
    threshold = None
    if fitted is not None:
        threshold = {
            "dense_cosine_min": fitted.threshold,
            "development_recall": fitted.recall,
            "development_false_positive_rate": fitted.false_positive_rate,
            "maximum_false_positive_rate": 0.05,
            "positive_case_count": fitted.positive_case_count,
            "negative_case_count": fitted.negative_case_count,
        }
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "identity": dict(identity),
        "mode": mode,
        "case_count": len(cases),
        "threshold": threshold,
        "metrics": {
            "development": _aggregate_dict(scores_by_split["development"]),
            "holdout": _aggregate_dict(scores_by_split["holdout"]),
            "all": _aggregate_dict(all_scores),
        },
        "latency_ms": {
            "sample_count": len(latencies),
            "warmup_count": warmup,
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
        },
        "python_tracemalloc_peak_bytes": peak_bytes,
        "cases": rows,
    }


def validate_hybrid_quality(
    *,
    lexical: Mapping[str, Any],
    dense: Mapping[str, Any],
    hybrid: Mapping[str, Any],
) -> None:
    try:
        lexical_metrics = lexical["metrics"]["holdout"]
        dense_metrics = dense["metrics"]["holdout"]
        hybrid_metrics = hybrid["metrics"]["holdout"]
        lexical_recall = float(lexical_metrics["recall_at_5"])
        dense_recall = float(dense_metrics["recall_at_5"])
        hybrid_recall = float(hybrid_metrics["recall_at_5"])
        lexical_mrr = float(lexical_metrics["mean_reciprocal_rank"])
        dense_mrr = float(dense_metrics["mean_reciprocal_rank"])
        hybrid_mrr = float(hybrid_metrics["mean_reciprocal_rank"])
        values = {
            lexical_recall,
            dense_recall,
            hybrid_recall,
            lexical_mrr,
            dense_mrr,
            hybrid_mrr,
        }
        if any(not math.isfinite(value) or not 0.0 <= value <= 1.0 for value in values):
            raise ValueError("invalid metric")
    except (KeyError, TypeError, ValueError) as error:
        raise EvaluationError("hybrid quality gate input is invalid") from error
    best_recall = max(lexical_recall, dense_recall)
    best_mrr = max(lexical_mrr, dense_mrr)
    if (
        hybrid_recall < best_recall
        or hybrid_mrr < best_mrr
        or not (hybrid_recall > best_recall or hybrid_mrr > best_mrr)
    ):
        raise EvaluationError("hybrid quality gate failed")


def _load_label_inventory(index_root: Path) -> tuple[set[str], dict[str, str]]:
    database = index_root / "metadata.sqlite3"
    try:
        connection = sqlite3.connect(
            f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
            raise EvaluationError("evaluation index is invalid")
        documents = {
            str(row[0]) for row in connection.execute("SELECT document_id FROM documents")
        }
        chunks = {
            str(row[0]): str(row[1])
            for row in connection.execute(
                "SELECT chunk_id, document_id FROM chunks"
            )
        }
    except sqlite3.Error as error:
        raise EvaluationError("evaluation index is invalid") from error
    finally:
        if "connection" in locals():
            connection.close()
    if not documents or not chunks:
        raise EvaluationError("evaluation index is invalid")
    return documents, chunks


def evaluate_pack(
    pack_root: Path,
    case_path: Path,
    *,
    mode: str,
    requested_device: str = "auto",
) -> dict[str, Any]:
    root = Path(pack_root)
    cases_file = Path(case_path)
    if (
        not root.is_absolute()
        or not cases_file.is_absolute()
        or mode not in {"lexical", "dense", "hybrid"}
        or requested_device not in {"auto", "cpu", "cuda"}
    ):
        raise EvaluationError("evaluation configuration is invalid")
    try:
        from .embedding import BgeM3Embedding, EmbeddingError, select_embedding_device
        from .hybrid_retriever import HybridRetriever
        from .pack import verify_complete_pack

        manifest = verify_complete_pack(root)
        cases = load_cases(cases_file)
        validate_final_composition(cases)
        documents, chunks = _load_label_inventory(root / "index")
        validate_case_labels(cases, documents=documents, chunks=chunks)
        embedding = None
        device = "none"
        if mode in {"dense", "hybrid"}:
            device = select_embedding_device(requested_device)
            try:
                embedding = BgeM3Embedding(
                    root / "model" / "bge-m3", device=device
                )
            except EmbeddingError:
                if requested_device != "auto" or device != "cuda":
                    raise
                device = "cpu"
                embedding = BgeM3Embedding(
                    root / "model" / "bge-m3", device=device
                )
        retriever = HybridRetriever(
            root / "index", embedding=embedding, dense_min=-1.0
        )
        return evaluate_cases(
            cases,
            retriever,
            mode=mode,
            identity={
                "pack_id": manifest.pack_id,
                "snapshot_date": manifest.snapshot_date,
                "document_count": manifest.document_count,
                "chunk_count": manifest.chunk_count,
                "model": manifest.embedding_model,
                "revision": manifest.embedding_revision,
                "dimensions": manifest.embedding_dimensions,
                "device": device,
            },
        )
    except EvaluationError:
        raise
    except Exception as error:
        raise EvaluationError("pack evaluation failed") from error
