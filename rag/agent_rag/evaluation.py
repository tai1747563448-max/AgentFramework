from __future__ import annotations

from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import re
import sqlite3
import threading
import time
import tracemalloc
from typing import Any, Callable, Iterable, Mapping, Sequence


CASE_SCHEMA_VERSION = 2
REPORT_SCHEMA_VERSION = 2
NEGATIVE_KINDS = frozenset({"no_answer", "hostile_query"})
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
_CURATED_CASE_KEYS = {
    "case_id",
    "language",
    "query",
    "expected_citations",
    "kind",
    "rationale",
    "source_snapshot",
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
        "hostile_query": 25,
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
    progress: Callable[..., None] | None = None,
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
        if progress is not None:
            progress("dense-threshold", 0, len(cases), mode=mode)
        for completed, item in enumerate(cases, start=1):
            score = retriever.max_dense_score(item.query)
            if type(score) not in {int, float} or not math.isfinite(float(score)):
                raise EvaluationError("retriever returned an invalid dense score")
            dense_scores[item.case_id] = float(score)
            if progress is not None:
                progress("dense-threshold", completed, len(cases), mode=mode)
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
    if progress is not None:
        progress("evaluate", 0, len(cases), mode=mode)
    for completed, item in enumerate(cases, start=1):
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
        if progress is not None:
            progress("evaluate", completed, len(cases), mode=mode)

    # tracemalloc materially distorts SQLite-heavy latency. Keep latency runs
    # uninstrumented and use one representative query per case kind as a
    # separate Python-allocation probe.
    probe_cases = tuple({item.kind: item for item in cases}.values())
    if progress is not None:
        progress("memory-probe", 0, len(probe_cases), mode=mode)
    tracemalloc.start()
    try:
        for completed, item in enumerate(probe_cases, start=1):
            retriever.query(
                item.query,
                top_k=10,
                max_total_bytes=32_768,
                mode=mode,
            )
            if progress is not None:
                progress("memory-probe", completed, len(probe_cases), mode=mode)
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
        "memory_probe_count": len(probe_cases),
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
        hybrid_false_positive_rate = float(
            hybrid_metrics["no_answer_false_positive_rate"]
        )
        values = {
            lexical_recall,
            dense_recall,
            hybrid_recall,
            lexical_mrr,
            dense_mrr,
            hybrid_mrr,
            hybrid_false_positive_rate,
        }
        if any(not math.isfinite(value) or not 0.0 <= value <= 1.0 for value in values):
            raise ValueError("invalid metric")
    except (KeyError, TypeError, ValueError) as error:
        raise EvaluationError("hybrid quality gate input is invalid") from error
    best_recall = max(lexical_recall, dense_recall)
    best_mrr = max(lexical_mrr, dense_mrr)
    if (
        hybrid_recall < 0.50
        or hybrid_mrr < 0.45
        or hybrid_false_positive_rate > 0.05
        or hybrid_recall < best_recall
        or hybrid_mrr < best_mrr
        or not (hybrid_recall > best_recall or hybrid_mrr > best_mrr)
    ):
        raise EvaluationError("hybrid quality gate failed")


def _mode_metrics(
    report: Mapping[str, Any], *, expected_mode: str, split: str
) -> tuple[float, float, float]:
    try:
        if report["mode"] != expected_mode:
            raise ValueError("mode mismatch")
        metrics = report["metrics"][split]
        values = (
            float(metrics["recall_at_5"]),
            float(metrics["mean_reciprocal_rank"]),
            float(metrics["no_answer_false_positive_rate"]),
        )
        if any(
            not math.isfinite(value) or not 0.0 <= value <= 1.0
            for value in values
        ):
            raise ValueError("invalid metric")
        return values
    except (KeyError, TypeError, ValueError) as error:
        raise EvaluationError("retrieval mode selection input is invalid") from error


def select_retrieval_mode(
    *,
    lexical: Mapping[str, Any],
    dense: Mapping[str, Any],
    hybrid: Mapping[str, Any],
) -> str:
    """Select on development metrics, then independently gate holdout safety."""

    reports = {"lexical": lexical, "dense": dense, "hybrid": hybrid}
    development = {
        mode: _mode_metrics(report, expected_mode=mode, split="development")
        for mode, report in reports.items()
    }
    candidates = {
        mode: metrics
        for mode, metrics in development.items()
        if mode in {"dense", "hybrid"}
        and metrics[0] >= 0.50
        and metrics[1] >= 0.45
        and metrics[2] <= 0.05
    }
    if not candidates:
        raise EvaluationError("selected retrieval quality gate failed")
    selected = max(
        candidates,
        key=lambda mode: (
            candidates[mode][0] + candidates[mode][1],
            candidates[mode][0],
            candidates[mode][1],
            -candidates[mode][2],
            mode == "dense",
        ),
    )

    lexical_holdout = _mode_metrics(
        lexical, expected_mode="lexical", split="holdout"
    )
    selected_holdout = _mode_metrics(
        reports[selected], expected_mode=selected, split="holdout"
    )
    if (
        selected_holdout[0] < 0.50
        or selected_holdout[1] < 0.45
        or selected_holdout[2] > 0.05
        or selected_holdout[0] < lexical_holdout[0]
        or selected_holdout[1] < lexical_holdout[1]
        or not (
            selected_holdout[0] > lexical_holdout[0]
            or selected_holdout[1] > lexical_holdout[1]
        )
    ):
        raise EvaluationError("selected retrieval quality gate failed")
    return selected


def _load_label_inventory(
    index_root: Path,
    *,
    progress: Callable[..., None] | None = None,
    mode: str | None = None,
) -> tuple[set[str], dict[str, str]]:
    database = index_root / "metadata.sqlite3"
    try:
        connection = sqlite3.connect(
            f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        heartbeat_stop = threading.Event()
        heartbeat = None
        if progress is not None:
            progress("evaluation-index-integrity", 0, 1, mode=mode)

            def emit_heartbeat() -> None:
                while not heartbeat_stop.wait(5.0):
                    try:
                        progress("evaluation-index-integrity", 0, 1, mode=mode)
                    except Exception:
                        return

            heartbeat = threading.Thread(target=emit_heartbeat, daemon=True)
            heartbeat.start()
        try:
            if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
                raise EvaluationError("evaluation index is invalid")
        finally:
            heartbeat_stop.set()
            if heartbeat is not None:
                heartbeat.join()
        if progress is not None:
            progress("evaluation-index-integrity", 1, 1, mode=mode)

        document_count = int(
            connection.execute("SELECT COUNT(*) FROM documents").fetchone()[0]
        )
        chunk_count = int(
            connection.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
        )
        total = document_count + chunk_count
        completed = 0
        if progress is not None:
            progress("evaluation-label-inventory", completed, total, mode=mode)

        documents: set[str] = set()
        cursor = connection.execute("SELECT document_id FROM documents")
        while rows := cursor.fetchmany(4096):
            documents.update(str(row[0]) for row in rows)
            completed += len(rows)
            if progress is not None:
                progress("evaluation-label-inventory", completed, total, mode=mode)

        chunks: dict[str, str] = {}
        cursor = connection.execute("SELECT chunk_id, document_id FROM chunks")
        while rows := cursor.fetchmany(4096):
            chunks.update((str(row[0]), str(row[1])) for row in rows)
            completed += len(rows)
            if progress is not None:
                progress("evaluation-label-inventory", completed, total, mode=mode)
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
    progress: Callable[..., None] | None = None,
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
        from .pack import verify_runtime_pack

        # The pack build and publish gates perform the exhaustive whole-pack
        # verification. Evaluation only consumes the immutable retrieval
        # assets, so repeating every corpus hash for each retrieval mode is
        # redundant and makes a three-mode comparison needlessly O(3 * pack).
        manifest = verify_runtime_pack(root, progress=progress)
        cases = load_cases(cases_file)
        validate_final_composition(cases)
        documents, chunks = _load_label_inventory(
            root / "index", progress=progress, mode=mode
        )
        validate_case_labels(cases, documents=documents, chunks=chunks)
        embedding = None
        device = "none"
        if mode in {"dense", "hybrid"}:
            if progress is not None:
                progress("embedding-load", 0, 1, mode=mode)
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
            if progress is not None:
                progress("embedding-load", 1, 1, mode=mode)
        retriever = HybridRetriever(
            root / "index",
            embedding=embedding,
            dense_min=-1.0,
            progress=progress,
            verify_database_integrity=False,
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
            progress=progress,
        )
    except EvaluationError:
        raise
    except Exception as error:
        raise EvaluationError("pack evaluation failed") from error


_ZH_LEGAL_TERMS = {
    "access": "访问",
    "administration": "管理",
    "application": "适用",
    "approval": "批准",
    "certificate": "证书",
    "compliance": "合规",
    "construction": "建设",
    "definitions": "定义",
    "disclosure": "披露",
    "emergency": "紧急情况",
    "emission": "排放",
    "emissions": "排放",
    "employee": "雇员",
    "enforcement": "执法",
    "exemption": "豁免",
    "filing": "申报",
    "general": "一般规定",
    "health": "健康",
    "inspection": "检查",
    "labeling": "标签",
    "license": "许可",
    "licensing": "许可",
    "maintenance": "维护",
    "notice": "通知",
    "operation": "运行",
    "payment": "支付",
    "permit": "许可证",
    "privacy": "隐私",
    "procedure": "程序",
    "procedures": "程序",
    "prohibition": "禁止",
    "prohibited": "禁止",
    "record": "记录",
    "records": "记录",
    "report": "报告",
    "reporting": "报告",
    "requirement": "要求",
    "requirements": "要求",
    "safety": "安全",
    "security": "安全保障",
    "standard": "标准",
    "standards": "标准",
    "testing": "测试",
    "training": "培训",
}


def _deterministic_order(identifier: str) -> str:
    import hashlib

    return hashlib.sha256(("ecfr-eval-v2:" + identifier).encode("utf-8")).hexdigest()


def _zh_concepts(title: str) -> tuple[str, ...]:
    words = re.findall(r"[A-Za-z]+", title.casefold())
    return tuple(dict.fromkeys(_ZH_LEGAL_TERMS[word] for word in words if word in _ZH_LEGAL_TERMS))


def _case_record(item: EvaluationCase) -> dict[str, Any]:
    return {
        "case_id": item.case_id,
        "split": item.split,
        "language": item.language,
        "query": item.query,
        "relevant_document_ids": list(item.relevant_document_ids),
        "relevant_chunk_ids": list(item.relevant_chunk_ids),
        "kind": item.kind,
        "rationale": item.rationale,
    }


def _load_curated_cases(
    path: Path, citation_documents: Mapping[str, str]
) -> list[EvaluationCase]:
    source = Path(path)
    try:
        if (
            not source.is_file()
            or source.is_symlink()
            or source.stat().st_size > 4 * 1024 * 1024
        ):
            raise EvaluationError("curated case file is invalid")
        lines = source.read_bytes().splitlines()
    except OSError as error:
        raise EvaluationError("curated case file is invalid") from error
    if not lines:
        raise EvaluationError("curated case file is invalid")
    cases: list[EvaluationCase] = []
    seen: set[str] = set()
    for raw in lines:
        value = _load_json_line(raw)
        if type(value) is not dict or set(value) != _CURATED_CASE_KEYS:
            raise EvaluationError("curated case record is invalid")
        citations = value["expected_citations"]
        if (
            type(value["case_id"]) is not str
            or _CASE_ID.fullmatch(value["case_id"]) is None
            or value["case_id"] in seen
            or type(value["language"]) is not str
            or value["language"] not in {"en", "zh"}
            or type(value["query"]) is not str
            or not value["query"].strip()
            or "\x00" in value["query"]
            or len(value["query"].encode("utf-8")) > 16_384
            or type(value["kind"]) is not str
            or value["kind"] not in CASE_KINDS - NEGATIVE_KINDS
            or type(value["rationale"]) is not str
            or not value["rationale"].strip()
            or type(value["source_snapshot"]) is not str
            or value["source_snapshot"] != "2026-09-03"
            or type(citations) is not list
            or not 1 <= len(citations) <= 4
            or any(type(item) is not str or not item.strip() for item in citations)
            or len(set(citations)) != len(citations)
            or (value["kind"] == "cross_language_zh" and value["language"] != "zh")
            or (value["kind"] == "paraphrase_en" and value["language"] != "en")
        ):
            raise EvaluationError("curated case record is invalid")
        try:
            document_ids = tuple(citation_documents[item] for item in citations)
        except KeyError as error:
            raise EvaluationError("unknown curated citation label") from error
        seen.add(value["case_id"])
        cases.append(
            EvaluationCase(
                value["case_id"],
                "holdout",
                value["language"],
                value["query"],
                document_ids,
                (),
                value["kind"],
                "Frozen source-checked case: " + value["rationale"],
            )
        )
    return cases


def generate_evaluation_cases(
    index_root: Path,
    negative_case_path: Path,
    curated_case_path: Path | None = None,
    progress: Callable[..., None] | None = None,
) -> list[EvaluationCase]:
    root = Path(index_root)
    negatives = load_cases(Path(negative_case_path))
    if (
        sum(item.kind == "no_answer" for item in negatives) != 25
        or sum(item.kind == "hostile_query" for item in negatives) != 25
        or any(not item.is_negative for item in negatives)
    ):
        raise EvaluationError("static negative evaluation cases are invalid")
    database = root / "metadata.sqlite3"
    try:
        connection = sqlite3.connect(
            f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
        )
        connection.execute("PRAGMA query_only=ON")
        if progress is not None:
            progress("evaluation-case-index", 0, 3, mode="generation")
        heartbeat_stop = threading.Event()
        heartbeat = None
        if progress is not None:
            def emit_heartbeat() -> None:
                while not heartbeat_stop.wait(5.0):
                    try:
                        progress(
                            "evaluation-case-index", 0, 3, mode="generation"
                        )
                    except Exception:
                        return

            heartbeat = threading.Thread(target=emit_heartbeat, daemon=True)
            heartbeat.start()
        try:
            if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
                raise EvaluationError("evaluation index is invalid")
        finally:
            heartbeat_stop.set()
            if heartbeat is not None:
                heartbeat.join()
        if progress is not None:
            progress("evaluation-case-index", 1, 3, mode="generation")
        document_rows = connection.execute(
            "SELECT d.document_id, d.citation, d.path, d.section_title, "
            "c.chunk_id, c.content FROM documents AS d JOIN chunks AS c "
            "ON c.document_id=d.document_id WHERE c.vector_row=("
            "SELECT MIN(c2.vector_row) FROM chunks AS c2 "
            "WHERE c2.document_id=d.document_id)"
        ).fetchall()
        adjacent_rows = connection.execute(
            "SELECT c.document_id, c.citation, c.path, d.section_title, "
            "c.chunk_id, c.next_id, c.content FROM chunks AS c "
            "JOIN documents AS d ON d.document_id=c.document_id "
            "WHERE c.next_id IS NOT NULL"
        ).fetchall()
        citation_documents = {
            str(citation): str(document_id)
            for document_id, citation in connection.execute(
                "SELECT document_id, citation FROM documents"
            )
        }
        chunk_documents = {
            str(chunk_id): str(document_id)
            for chunk_id, document_id in connection.execute(
                "SELECT chunk_id, document_id FROM chunks"
            )
        }
        if progress is not None:
            progress("evaluation-case-index", 2, 3, mode="generation")
    except sqlite3.Error as error:
        raise EvaluationError("evaluation index is invalid") from error
    finally:
        if "connection" in locals():
            connection.close()
    if len(document_rows) < 250 or len(adjacent_rows) < 30:
        raise EvaluationError("evaluation index has insufficient source diversity")
    documents = sorted(document_rows, key=lambda row: _deterministic_order(str(row[0])))
    cross_language = [row for row in documents if _zh_concepts(str(row[3]))]
    if len(cross_language) < 60:
        raise EvaluationError("evaluation index has insufficient multilingual concepts")

    cases: list[EvaluationCase] = []

    def add_positive(
        kind: str,
        index: int,
        query: str,
        document_ids: tuple[str, ...],
        chunk_ids: tuple[str, ...],
        rationale: str,
        *,
        language: str,
    ) -> None:
        serial = len(cases) + 1
        cases.append(
            EvaluationCase(
                f"pos-{kind.replace('_', '-')}-{index + 1:03d}",
                "development" if serial % 2 else "holdout",
                language,
                query,
                document_ids,
                chunk_ids,
                kind,
                rationale,
            )
        )

    for index, row in enumerate(documents[:60]):
        document_id, citation, path, _, _, _ = map(str, row)
        add_positive(
            "citation",
            index,
            citation,
            (document_id,),
            (),
            f"Exact citation audited against {path} in the locked document manifest.",
            language="en",
        )
    for index, row in enumerate(documents[60:120]):
        document_id, citation, path, title, _, content = map(str, row)
        excerpt_words = re.findall(r"[A-Za-z][A-Za-z0-9'-]+", content)[:8]
        phrase = " ".join(excerpt_words)
        query = f"What federal requirements apply to {title}? {phrase}"
        add_positive(
            "paraphrase_en",
            index,
            query[:16_000],
            (document_id,),
            (),
            f"English paraphrase audited against {citation} at {path}.",
            language="en",
        )
    for index, row in enumerate(cross_language[:60]):
        document_id, citation, path, title, _, _ = map(str, row)
        concepts = "、".join(_zh_concepts(title)[:4])
        query = f"关于{concepts}，这条美国联邦法规规定了什么？"
        add_positive(
            "cross_language_zh",
            index,
            query,
            (document_id,),
            (),
            f"Chinese cross-language query audited against {citation} at {path}; source title is {title}.",
            language="zh",
        )
    multi_source = documents[120:200]
    for index in range(40):
        left = multi_source[index * 2]
        right = multi_source[index * 2 + 1]
        left_id, left_citation, left_path, left_title, _, _ = map(str, left)
        right_id, right_citation, right_path, right_title, _, _ = map(str, right)
        add_positive(
            "multi_concept",
            index,
            f"Compare the federal requirements for {left_title} and {right_title}.",
            (left_id, right_id),
            (),
            f"Two-source case audited against {left_citation} ({left_path}) and {right_citation} ({right_path}).",
            language="en",
        )
    adjacent = sorted(adjacent_rows, key=lambda row: _deterministic_order(str(row[4])))[:30]
    for index, row in enumerate(adjacent):
        document_id, citation, path, title, chunk_id, next_id, content = map(str, row)
        words = " ".join(re.findall(r"[A-Za-z][A-Za-z0-9'-]+", content)[:10])
        add_positive(
            "adjacent_context",
            index,
            f"What context immediately follows the rule about {title}? {words}"[:16_000],
            (document_id,),
            (next_id,),
            f"Adjacent-chunk label {next_id} follows primary {chunk_id} in {citation} at {path}.",
            language="en",
        )
    if curated_case_path is not None:
        cases.extend(
            _load_curated_cases(Path(curated_case_path), citation_documents)
        )
    cases.extend(negatives)
    validate_final_composition(cases)
    validate_case_labels(
        cases,
        documents=set(citation_documents.values()),
        chunks=chunk_documents,
    )
    if progress is not None:
        progress("evaluation-case-index", 3, 3, mode="generation")
    return cases


def write_evaluation_cases(path: Path, cases: Sequence[EvaluationCase]) -> None:
    destination = Path(path)
    if not destination.is_absolute() or not destination.parent.is_dir() or destination.is_symlink():
        raise EvaluationError("evaluation case output is invalid")
    temporary = destination.with_name(destination.name + ".partial")
    if temporary.exists() or temporary.is_symlink():
        raise EvaluationError("evaluation case output is invalid")
    try:
        with temporary.open("x", encoding="utf-8", newline="") as stream:
            for item in cases:
                stream.write(stable_report_json(_case_record(item)))
            stream.flush()
            import os

            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    except OSError as error:
        raise EvaluationError("evaluation case output failed") from error
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
