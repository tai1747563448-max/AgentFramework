from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))


from agent_rag.evaluation import (  # type: ignore[import-not-found]
    EvaluationCase,
    EvaluationError,
    aggregate_scores,
    evaluate_cases,
    fit_dense_threshold,
    load_cases,
    percentile,
    score_case,
    stable_report_json,
    validate_hybrid_quality,
    validate_case_labels,
    validate_final_composition,
    validate_splits,
)
from agent_rag import cli as rag_cli  # type: ignore[import-not-found]


class Hit:
    def __init__(self, chunk_id: str, document_id: str, citation: str = "1 CFR 1.1"):
        self.chunk_id = chunk_id
        self.document_id = document_id
        self.citation = citation


class FakeRetriever:
    def __init__(self) -> None:
        self.calls: list[tuple[str, str]] = []

    def max_dense_score(self, query: str) -> float:
        return {"positive": 0.8, "negative": 0.2, "holdout": 0.85,
                "holdout-negative": 0.1}.get(query, 0.0)

    def query(
        self, query: str, *, top_k: int, max_total_bytes: int, mode: str
    ) -> list[Hit]:
        assert top_k == 10
        assert max_total_bytes == 32768
        self.calls.append((query, mode))
        if "negative" in query:
            return [Hit("doc-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb-chunk-bbbbbbbbbbbbbbbb",
                        "doc-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")]
        return [Hit("doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-chunk-aaaaaaaaaaaaaaaa",
                    "doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")]


def case(
    case_id: str,
    *,
    split: str = "development",
    kind: str = "paraphrase_en",
    language: str = "en",
    documents: tuple[str, ...] = ("doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",),
    chunks: tuple[str, ...] = (),
) -> EvaluationCase:
    return EvaluationCase(
        case_id=case_id,
        split=split,
        language=language,
        query="What rule applies?",
        relevant_document_ids=documents,
        relevant_chunk_ids=chunks,
        kind=kind,
        rationale="Audited against the cited source section.",
    )


def test_metrics_use_zero_for_missed_query() -> None:
    result = score_case(relevant={"d1"}, ranked=["d2", "d3"])

    assert result.recall_at_5 == 0.0
    assert result.recall_at_10 == 0.0
    assert result.reciprocal_rank == 0.0
    assert result.ndcg_at_10 == 0.0


def test_metrics_use_unique_hits_and_binary_ndcg() -> None:
    result = score_case(relevant={"d1", "d2"}, ranked=["x", "d2", "d2", "d1"])

    assert result.recall_at_5 == 1.0
    assert result.reciprocal_rank == 0.5
    assert result.ndcg_at_10 == pytest.approx(
        (1.0 / 1.584962500721156 + 1.0 / 2.0)
        / (1.0 + 1.0 / 1.584962500721156)
    )


def test_no_answer_scoring_and_aggregation_are_explicit() -> None:
    empty = score_case(relevant=set(), ranked=[])
    false_positive = score_case(relevant=set(), ranked=["d1"])
    metrics = aggregate_scores([empty, false_positive])

    assert not empty.no_answer_false_positive
    assert false_positive.no_answer_false_positive
    assert metrics.no_answer_false_positive_rate == 0.5
    assert metrics.positive_case_count == 0


def test_evaluation_rejects_case_overlap_between_development_and_holdout() -> None:
    with pytest.raises(EvaluationError, match="case split overlap"):
        validate_splits([case("same")], [case("same", split="holdout")])


def test_case_loader_rejects_duplicate_ids_extra_keys_and_missing_labels(
    tmp_path: Path,
) -> None:
    valid = {
        "case_id": "dev-001",
        "split": "development",
        "language": "en",
        "query": "question",
        "relevant_document_ids": ["doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"],
        "relevant_chunk_ids": [],
        "kind": "paraphrase_en",
        "rationale": "audited",
    }
    path = tmp_path / "cases.jsonl"
    path.write_text(
        json.dumps(valid) + "\n" + json.dumps(valid) + "\n",
        encoding="utf-8",
    )
    with pytest.raises(EvaluationError, match="duplicate case id"):
        load_cases(path)

    extra = dict(valid, unexpected=True)
    path.write_text(json.dumps(extra) + "\n", encoding="utf-8")
    with pytest.raises(EvaluationError, match="case record is invalid"):
        load_cases(path)

    valid["relevant_document_ids"] = []
    path.write_text(json.dumps(valid) + "\n", encoding="utf-8")
    with pytest.raises(EvaluationError, match="positive case requires labels"):
        load_cases(path)


def test_case_loader_rejects_duplicate_json_keys_and_unknown_labels(
    tmp_path: Path,
) -> None:
    path = tmp_path / "cases.jsonl"
    path.write_text(
        '{"case_id":"a","case_id":"b","split":"development"}\n',
        encoding="utf-8",
    )
    with pytest.raises(EvaluationError, match="case JSON is invalid"):
        load_cases(path)

    positive = case(
        "dev-positive",
        chunks=("chunk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",),
    )
    with pytest.raises(EvaluationError, match="unknown document label"):
        validate_case_labels(
            [positive],
            documents={"doc-cccccccccccccccccccccccccccccccc"},
            chunks={"chunk-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":
                    "doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
        )
    with pytest.raises(EvaluationError, match="unknown chunk label"):
        validate_case_labels(
            [positive],
            documents={"doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
            chunks={},
        )


def test_dense_threshold_maximizes_recall_under_five_percent_false_positives() -> None:
    development = [
        (case("p1"), 0.90),
        (case("p2"), 0.70),
        (case("p3"), 0.40),
        (case("n1", kind="no_answer", documents=()), 0.65),
        (case("n2", kind="injection_damaged", documents=()), 0.20),
    ]

    fitted = fit_dense_threshold(development, maximum_false_positive_rate=0.05)

    assert fitted.threshold == pytest.approx(0.70)
    assert fitted.recall == pytest.approx(2 / 3)
    assert fitted.false_positive_rate == 0.0


def test_percentiles_exclude_warmup_and_use_nearest_rank() -> None:
    values = [999.0, 1.0, 2.0, 3.0, 100.0]

    assert percentile(values, 0.50, warmup=1) == 2.0
    assert percentile(values, 0.95, warmup=1) == 100.0
    with pytest.raises(EvaluationError, match="warm-up"):
        percentile([1.0], 0.50, warmup=1)


def test_final_composition_requires_all_300_audited_cases() -> None:
    cases: list[EvaluationCase] = []
    requirements = {
        "citation": (60, "en"),
        "paraphrase_en": (60, "en"),
        "cross_language_zh": (60, "zh"),
        "multi_concept": (40, "en"),
        "adjacent_context": (30, "en"),
        "no_answer": (25, "en"),
        "injection_damaged": (25, "en"),
    }
    index = 0
    for kind, (count, language) in requirements.items():
        for _ in range(count):
            index += 1
            negative = kind in {"no_answer", "injection_damaged"}
            cases.append(
                case(
                    f"case-{index:03d}",
                    split="development" if index % 2 else "holdout",
                    kind=kind,
                    language=language,
                    documents=() if negative else
                        ("doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",),
                )
            )

    validate_final_composition(cases)

    with pytest.raises(EvaluationError, match="evaluation composition"):
        validate_final_composition(cases[:-1])


def test_report_serialization_is_stable_and_rejects_nonfinite_numbers() -> None:
    first = stable_report_json({"z": 1, "a": {"b": 2.0}})
    second = stable_report_json({"a": {"b": 2.0}, "z": 1})

    assert first == second == '{"a":{"b":2.0},"z":1}\n'
    with pytest.raises(EvaluationError, match="report is invalid"):
        stable_report_json({"bad": float("nan")})


def test_static_negative_assets_have_25_no_answer_and_25_injection_cases() -> None:
    cases = load_cases(RAG_ROOT / "eval" / "negative_cases.jsonl")

    assert len(cases) == 50
    assert sum(item.kind == "no_answer" for item in cases) == 25
    assert sum(item.kind == "injection_damaged" for item in cases) == 25
    assert {item.split for item in cases} == {"development", "holdout"}
    assert all(item.is_negative for item in cases)
    schema = json.loads((RAG_ROOT / "eval" / "ecfr_eval_schema.json").read_text("utf-8"))
    assert schema["$id"] == "https://agentframework.local/schema/ecfr-eval-v2.json"


def test_evaluate_cases_fits_development_gate_and_applies_it_to_holdout() -> None:
    cases = [
        case("dev-p", split="development"),
        case("dev-n", split="development", kind="no_answer", documents=()),
        case("hold-p", split="holdout"),
        case("hold-n", split="holdout", kind="no_answer", documents=()),
    ]
    cases[0] = EvaluationCase(**{**cases[0].__dict__, "query": "positive"})
    cases[1] = EvaluationCase(**{**cases[1].__dict__, "query": "negative"})
    cases[2] = EvaluationCase(**{**cases[2].__dict__, "query": "holdout"})
    cases[3] = EvaluationCase(**{**cases[3].__dict__, "query": "holdout-negative"})

    report = evaluate_cases(
        cases,
        FakeRetriever(),
        mode="hybrid",
        identity={
            "pack_id": "pack-0123456789abcdef0123456789abcdef",
            "snapshot_date": "2026-09-03",
            "model": "BAAI/bge-m3",
            "revision": "r",
        },
        warmup=1,
    )

    assert report["threshold"]["dense_cosine_min"] == pytest.approx(0.8)
    assert report["metrics"]["holdout"]["recall_at_5"] == 1.0
    assert report["metrics"]["holdout"]["no_answer_false_positive_rate"] == 0.0
    assert report["latency_ms"]["sample_count"] == 4
    assert report["latency_ms"]["warmup_count"] == 1
    assert report["python_tracemalloc_peak_bytes"] >= 0


def test_hybrid_quality_gate_requires_nonregression_and_strict_improvement() -> None:
    lexical = {"metrics": {"holdout": {"recall_at_5": 0.6,
                                         "mean_reciprocal_rank": 0.5}}}
    dense = {"metrics": {"holdout": {"recall_at_5": 0.7,
                                       "mean_reciprocal_rank": 0.6}}}
    hybrid = {"metrics": {"holdout": {"recall_at_5": 0.8,
                                        "mean_reciprocal_rank": 0.6}}}

    validate_hybrid_quality(lexical=lexical, dense=dense, hybrid=hybrid)

    with pytest.raises(EvaluationError, match="hybrid quality gate"):
        validate_hybrid_quality(lexical=dense, dense=dense, hybrid=dense)


def test_evaluate_cli_requires_absolute_ordered_paths_and_writes_atomically(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    pack = tmp_path / "pack"
    pack.mkdir()
    cases = tmp_path / "cases.jsonl"
    cases.write_text("fixture", encoding="utf-8")
    output = tmp_path / "report.json"
    monkeypatch.setattr(
        rag_cli,
        "evaluate_pack",
        lambda pack_root, case_path, *, mode: {
            "schema_version": 2,
            "mode": mode,
            "pack": str(pack_root),
            "cases": str(case_path),
        },
    )

    result = rag_cli.main(
        [
            "evaluate",
            "--pack-root",
            str(pack),
            "--cases",
            str(cases),
            "--mode",
            "hybrid",
            "--output",
            str(output),
        ]
    )

    assert result == 0
    assert json.loads(output.read_text("utf-8"))["mode"] == "hybrid"
    assert list(tmp_path.glob("report.json.partial-*")) == []
    assert rag_cli.main(
        [
            "evaluate", "--pack-root", "relative", "--cases", str(cases),
            "--mode", "hybrid", "--output", str(output),
        ]
    ) == 2
    assert capsys.readouterr().err == "retrieval evaluation failed\n"
