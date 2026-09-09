from __future__ import annotations

import io
import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))


class Clock:
    def __init__(self, *values: float) -> None:
        self._values = iter(values)

    def __call__(self) -> float:
        return next(self._values)


def test_progress_reporter_emits_rate_limited_safe_jsonl() -> None:
    from agent_rag.progress import ProgressReporter

    stream = io.StringIO()
    reporter = ProgressReporter(
        stream,
        clock=Clock(100.0, 102.0, 106.0, 110.0),
        minimum_interval_seconds=5.0,
    )

    reporter.update("evaluate", 0, 10, mode="lexical")
    reporter.update("evaluate", 1, 10, mode="lexical")
    reporter.update("evaluate", 2, 10, mode="lexical")
    reporter.update("evaluate", 10, 10, mode="lexical")

    events = [json.loads(line) for line in stream.getvalue().splitlines()]
    assert [event["completed"] for event in events] == [0, 2, 10]
    assert events[0] == {
        "completed": 0,
        "elapsed_seconds": 0.0,
        "eta_seconds": None,
        "mode": "lexical",
        "percent": 0.0,
        "phase": "evaluate",
        "schema_version": 1,
        "status": "running",
        "throughput_items_per_second": 0.0,
        "total": 10,
        "type": "progress",
    }
    assert events[1]["elapsed_seconds"] == 6.0
    assert events[1]["throughput_items_per_second"] == 0.333
    assert events[1]["eta_seconds"] == 24.0
    assert events[-1]["status"] == "completed"
    assert events[-1]["eta_seconds"] is None
    assert set(events[-1]) == set(events[0])


def test_progress_reporter_rejects_identifiers_and_invalid_counts() -> None:
    from agent_rag.progress import ProgressReporter, ProgressError

    reporter = ProgressReporter(io.StringIO())

    with pytest.raises(ProgressError, match="progress event is invalid"):
        reporter.update("contains secret text", 0, 1)
    with pytest.raises(ProgressError, match="progress event is invalid"):
        reporter.update("evaluate", 2, 1)


def test_progress_reporter_emits_failure_without_free_text() -> None:
    from agent_rag.progress import ProgressReporter

    stream = io.StringIO()
    reporter = ProgressReporter(
        stream,
        clock=Clock(10.0, 11.0),
        minimum_interval_seconds=60.0,
    )

    reporter.update("evaluate", 0, 10, mode="lexical")
    reporter.fail("evaluate", mode="lexical")

    events = [json.loads(line) for line in stream.getvalue().splitlines()]
    assert [event["status"] for event in events] == ["running", "failed"]
    assert events[-1]["completed"] == 0
    assert events[-1]["total"] == 10
    assert "error" not in events[-1]
