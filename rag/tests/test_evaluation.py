from __future__ import annotations

import json
from pathlib import Path
import sqlite3
import sys
from types import SimpleNamespace

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))


from agent_rag.evaluation import (  # type: ignore[import-not-found]
    EvaluationCase,
    EvaluationError,
    aggregate_scores,
    evaluate_cases,
    fit_dense_threshold,
    generate_evaluation_cases,
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
from agent_rag import evaluation as evaluation_module  # type: ignore[import-not-found]
from agent_rag import hybrid_retriever as hybrid_retriever_module  # type: ignore[import-not-found]
from agent_rag import pack as pack_module  # type: ignore[import-not-found]


class Hit:
    def __init__(self, chunk_id: str, document_id: str, citation: str = "1 CFR 1.1"):
        self.chunk_id = chunk_id
        self.document_id = document_id
        self.citation = citation


class FakeRetriever:
    def __init__(self) -> None:
        self.calls: list[tuple[str, str]] = []
        self.trace_states: list[bool] = []

    def max_dense_score(self, query: str) -> float:
        return {"positive": 0.8, "negative": 0.2, "holdout": 0.85,
                "holdout-negative": 0.1}.get(query, 0.0)

    def query(
        self, query: str, *, top_k: int, max_total_bytes: int, mode: str
    ) -> list[Hit]:
        assert top_k == 10
        assert max_total_bytes == 32768
        self.calls.append((query, mode))
        import tracemalloc

        self.trace_states.append(tracemalloc.is_tracing())
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
        (case("n2", kind="hostile_query", documents=()), 0.20),
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
        "hostile_query": (25, "en"),
    }
    index = 0
    for kind, (count, language) in requirements.items():
        for _ in range(count):
            index += 1
            negative = kind in {"no_answer", "hostile_query"}
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
    assert sum(item.kind == "hostile_query" for item in cases) == 25
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

    progress_events: list[tuple[str, int, int, str | None]] = []

    def progress(
        phase: str, completed: int, total: int, *, mode: str | None = None
    ) -> None:
        progress_events.append((phase, completed, total, mode))

    retriever = FakeRetriever()
    report = evaluate_cases(
        cases,
        retriever,
        mode="hybrid",
        identity={
            "pack_id": "pack-0123456789abcdef0123456789abcdef",
            "snapshot_date": "2026-09-03",
            "model": "BAAI/bge-m3",
            "revision": "r",
        },
        warmup=1,
        progress=progress,
    )

    assert report["threshold"]["dense_cosine_min"] == pytest.approx(0.8)
    assert report["metrics"]["holdout"]["recall_at_5"] == 1.0
    assert report["metrics"]["holdout"]["no_answer_false_positive_rate"] == 0.0
    assert report["latency_ms"]["sample_count"] == 4
    assert report["latency_ms"]["warmup_count"] == 1
    assert report["python_tracemalloc_peak_bytes"] >= 0
    assert report["memory_probe_count"] == 2
    assert retriever.trace_states[:5] == [False] * 5
    assert retriever.trace_states[5:] == [True, True]
    assert [event[1] for event in progress_events if event[0] == "dense-threshold"] == [
        0, 1, 2, 3, 4
    ]
    assert [event[1] for event in progress_events if event[0] == "evaluate"] == [
        0, 1, 2, 3, 4
    ]
    assert {event[3] for event in progress_events} == {"hybrid"}
    assert [event[1] for event in progress_events if event[0] == "memory-probe"] == [
        0, 1, 2
    ]


def test_pack_evaluation_uses_the_bounded_runtime_check_after_publish_validation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    manifest = SimpleNamespace(
        pack_id="pack-0123456789abcdef0123456789abcdef",
        snapshot_date="2026-09-03",
        document_count=30_000,
        chunk_count=146_189,
        embedding_model="BAAI/bge-m3",
        embedding_revision="r",
        embedding_dimensions=1024,
    )
    calls: dict[str, object] = {}

    def verify_runtime(root: Path, *, progress: object) -> object:
        calls["verified"] = root
        calls["progress"] = progress
        return manifest

    def forbidden_complete(*_: object, **__: object) -> object:
        raise AssertionError("evaluation repeated the full pack verification")

    class Retriever:
        def __init__(self, root: Path, **kwargs: object) -> None:
            calls["retriever_root"] = root
            calls.update(kwargs)

    monkeypatch.setattr(pack_module, "verify_runtime_pack", verify_runtime)
    monkeypatch.setattr(pack_module, "verify_complete_pack", forbidden_complete)
    monkeypatch.setattr(hybrid_retriever_module, "HybridRetriever", Retriever)
    monkeypatch.setattr(evaluation_module, "load_cases", lambda _: [case("only")])
    monkeypatch.setattr(evaluation_module, "validate_final_composition", lambda _: None)
    monkeypatch.setattr(
        evaluation_module,
        "_load_label_inventory",
        lambda _, **__: ({"doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {}),
    )
    monkeypatch.setattr(evaluation_module, "validate_case_labels", lambda *_, **__: None)
    monkeypatch.setattr(
        evaluation_module,
        "evaluate_cases",
        lambda *_, identity, **__: {"identity": identity},
    )
    progress = lambda *_, **__: None

    report = evaluation_module.evaluate_pack(
        tmp_path.resolve(),
        (tmp_path / "cases.jsonl").resolve(),
        mode="lexical",
        progress=progress,
    )

    assert calls["verified"] == tmp_path.resolve()
    assert calls["progress"] is progress
    assert calls["verify_database_integrity"] is False
    assert report["identity"]["pack_id"] == manifest.pack_id


def test_label_inventory_reports_integrity_heartbeat_and_row_progress(
    tmp_path: Path,
) -> None:
    index = tmp_path / "index"
    index.mkdir()
    with sqlite3.connect(index / "metadata.sqlite3") as connection:
        connection.execute("CREATE TABLE documents(document_id TEXT PRIMARY KEY)")
        connection.execute(
            "CREATE TABLE chunks(chunk_id TEXT PRIMARY KEY, document_id TEXT NOT NULL)"
        )
        connection.executemany(
            "INSERT INTO documents(document_id) VALUES (?)", [("doc-a",), ("doc-b",)]
        )
        connection.executemany(
            "INSERT INTO chunks(chunk_id, document_id) VALUES (?, ?)",
            [("chunk-a", "doc-a"), ("chunk-b", "doc-b"), ("chunk-c", "doc-b")],
        )
    events: list[tuple[str, int, int, str | None]] = []

    documents, chunks = evaluation_module._load_label_inventory(
        index,
        progress=lambda phase, completed, total, *, mode=None: events.append(
            (phase, completed, total, mode)
        ),
        mode="hybrid",
    )

    assert documents == {"doc-a", "doc-b"}
    assert chunks == {"chunk-a": "doc-a", "chunk-b": "doc-b", "chunk-c": "doc-b"}
    assert [event[1] for event in events if event[0] == "evaluation-index-integrity"] == [
        0,
        1,
    ]
    inventory = [event for event in events if event[0] == "evaluation-label-inventory"]
    assert inventory[0][1:] == (0, 5, "hybrid")
    assert inventory[-1][1:] == (5, 5, "hybrid")


def test_hybrid_quality_gate_requires_nonregression_and_strict_improvement() -> None:
    lexical = {"metrics": {"holdout": {"recall_at_5": 0.6,
                                         "mean_reciprocal_rank": 0.5,
                                         "no_answer_false_positive_rate": 0.0}}}
    dense = {"metrics": {"holdout": {"recall_at_5": 0.7,
                                       "mean_reciprocal_rank": 0.6,
                                       "no_answer_false_positive_rate": 0.0}}}
    hybrid = {"metrics": {"holdout": {"recall_at_5": 0.8,
                                        "mean_reciprocal_rank": 0.6,
                                        "no_answer_false_positive_rate": 0.0}}}

    validate_hybrid_quality(lexical=lexical, dense=dense, hybrid=hybrid)

    with pytest.raises(EvaluationError, match="hybrid quality gate"):
        validate_hybrid_quality(lexical=dense, dense=dense, hybrid=dense)


def test_hybrid_quality_gate_enforces_absolute_quality_and_no_answer_safety() -> None:
    weak_lexical = {"metrics": {"holdout": {
        "recall_at_5": 0.1,
        "mean_reciprocal_rank": 0.1,
        "no_answer_false_positive_rate": 0.0,
    }}}
    weak_dense = {"metrics": {"holdout": {
        "recall_at_5": 0.2,
        "mean_reciprocal_rank": 0.2,
        "no_answer_false_positive_rate": 0.0,
    }}}
    weak_hybrid = {"metrics": {"holdout": {
        "recall_at_5": 0.3,
        "mean_reciprocal_rank": 0.3,
        "no_answer_false_positive_rate": 0.0,
    }}}
    with pytest.raises(EvaluationError, match="hybrid quality gate"):
        validate_hybrid_quality(
            lexical=weak_lexical, dense=weak_dense, hybrid=weak_hybrid
        )

    unsafe_hybrid = {"metrics": {"holdout": {
        "recall_at_5": 0.8,
        "mean_reciprocal_rank": 0.7,
        "no_answer_false_positive_rate": 0.08,
    }}}
    with pytest.raises(EvaluationError, match="hybrid quality gate"):
        validate_hybrid_quality(
            lexical=weak_lexical, dense=weak_dense, hybrid=unsafe_hybrid
        )


def test_retrieval_mode_selection_uses_development_and_gates_holdout() -> None:
    def report(mode: str, development: tuple[float, float], holdout: tuple[float, float]):
        return {
            "mode": mode,
            "metrics": {
                "development": {
                    "recall_at_5": development[0],
                    "mean_reciprocal_rank": development[1],
                    "no_answer_false_positive_rate": 0.04,
                },
                "holdout": {
                    "recall_at_5": holdout[0],
                    "mean_reciprocal_rank": holdout[1],
                    "no_answer_false_positive_rate": 0.04,
                },
            },
        }

    lexical = report("lexical", (0.48, 0.46), (0.47, 0.45))
    dense = report("dense", (0.57, 0.57), (0.55, 0.51))
    hybrid = report("hybrid", (0.57, 0.56), (0.90, 0.90))

    assert evaluation_module.select_retrieval_mode(
        lexical=lexical, dense=dense, hybrid=hybrid
    ) == "dense"

    unsafe_dense = report("dense", (0.57, 0.57), (0.49, 0.51))
    with pytest.raises(EvaluationError, match="selected retrieval quality gate"):
        evaluation_module.select_retrieval_mode(
            lexical=lexical, dense=unsafe_dense, hybrid=hybrid
        )


def test_evaluate_cli_requires_absolute_ordered_paths_and_writes_atomically(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    pack = tmp_path / "pack"
    pack.mkdir()
    cases = tmp_path / "cases.jsonl"
    cases.write_text("fixture", encoding="utf-8")
    output = tmp_path / "report.json"
    def fake_evaluate_pack(pack_root, case_path, *, mode, progress):
        progress("evaluate", 0, 1, mode=mode)
        progress("evaluate", 1, 1, mode=mode)
        return {
            "schema_version": 2,
            "mode": mode,
            "pack": str(pack_root),
            "cases": str(case_path),
        }

    monkeypatch.setattr(
        rag_cli,
        "evaluate_pack",
        fake_evaluate_pack,
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
    success = capsys.readouterr()
    assert success.out == ""
    events = [json.loads(line) for line in success.err.splitlines()]
    assert [event["phase"] for event in events] == [
        "evaluate-command", "evaluate", "evaluate", "evaluate-command"
    ]
    assert [event["status"] for event in events] == [
        "running", "running", "completed", "completed"
    ]
    assert rag_cli.main(
        [
            "evaluate", "--pack-root", "relative", "--cases", str(cases),
            "--mode", "hybrid", "--output", str(output),
        ]
    ) == 2
    assert capsys.readouterr().err == "retrieval evaluation failed\n"


def test_evaluate_cli_emits_failed_progress_before_keyboard_interrupt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    pack = tmp_path / "pack"
    pack.mkdir()
    cases = tmp_path / "cases.jsonl"
    cases.write_text("fixture", encoding="utf-8")
    output = tmp_path / "report.json"

    def interrupted(*args, **kwargs):
        del args, kwargs
        raise KeyboardInterrupt

    monkeypatch.setattr(rag_cli, "evaluate_pack", interrupted)
    with pytest.raises(KeyboardInterrupt):
        rag_cli._evaluate(
            [
                "evaluate", "--pack-root", str(pack), "--cases", str(cases),
                "--mode", "lexical", "--output", str(output),
            ]
        )

    events = [json.loads(line) for line in capsys.readouterr().err.splitlines()]
    assert events[-1]["phase"] == "evaluate-command"
    assert events[-1]["status"] == "failed"


def test_generated_300_case_suite_is_deterministic_and_all_labels_resolve(
    tmp_path: Path,
) -> None:
    index = tmp_path / "index"
    index.mkdir()
    with sqlite3.connect(index / "metadata.sqlite3") as connection:
        connection.executescript(
            "CREATE TABLE documents(document_id TEXT PRIMARY KEY,citation TEXT,path TEXT,section_title TEXT);"
            "CREATE TABLE chunks(chunk_id TEXT PRIMARY KEY,document_id TEXT,citation TEXT,path TEXT,"
            "next_id TEXT,content TEXT,vector_row INTEGER);"
        )
        vector_row = 0
        for number in range(250):
            document_id = f"doc-{number:032x}"
            chunk_id = f"{document_id}-chunk-{number:016x}"
            next_id = (
                f"{document_id}-chunk-{number + 1000:016x}" if number < 30 else None
            )
            citation = f"1 CFR {number + 1}.1"
            path = f"corpus/section-{number:03d}.md"
            title = f"Safety reporting requirements {number}"
            connection.execute(
                "INSERT INTO documents VALUES(?,?,?,?)",
                (document_id, citation, path, title),
            )
            connection.execute(
                "INSERT INTO chunks VALUES(?,?,?,?,?,?,?)",
                (chunk_id, document_id, citation, path, next_id,
                 f"Federal safety reporting rule number {number}", vector_row),
            )
            vector_row += 1
            if next_id is not None:
                connection.execute(
                    "INSERT INTO chunks VALUES(?,?,?,?,?,?,?)",
                    (next_id, document_id, citation, path, None,
                     f"Adjacent context number {number}", vector_row),
                )
                vector_row += 1

    negatives = RAG_ROOT / "eval" / "negative_cases.jsonl"
    first = generate_evaluation_cases(index, negatives)
    second = generate_evaluation_cases(index, negatives)

    assert first == second
    assert len(first) == 300
    validate_final_composition(first)

    curated = tmp_path / "curated.jsonl"
    curated.write_text(
        json.dumps(
            {
                "case_id": "curated-holdout-fixture",
                "language": "en",
                "query": "Which rule covers the first synthetic safety requirement?",
                "expected_citations": ["1 CFR 1.1"],
                "kind": "paraphrase_en",
                "rationale": "Independent frozen fixture query.",
                "source_snapshot": "2026-09-03",
            },
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    with_curated = generate_evaluation_cases(index, negatives, curated)
    assert len(with_curated) == 301
    resolved = next(
        item for item in with_curated if item.case_id == "curated-holdout-fixture"
    )
    assert resolved.split == "holdout"
    assert resolved.relevant_document_ids == ("doc-00000000000000000000000000000000",)
