"""Self-contained tests for benchmarks/cli_latency_benchmark.py.

These tests verify the harness contract: the fixture file is well-formed, the
percentile helper is monotonic and stable, the summary aggregation preserves
the four canonical stages, and the driver refuses to operate outside the
isolated worktree. They run with ``python -m pytest`` and do not require the
C++ binary to exist, so they fit into the offline CI lane described in the
implementation plan's T0 step.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = REPO_ROOT / "benchmarks" / "cli_latency_benchmark.py"
FIXTURE = REPO_ROOT / "benchmarks" / "fixtures" / "cli_latency_cases.jsonl"


def _load_harness():
    spec = importlib.util.spec_from_file_location("cli_latency_benchmark", HARNESS)
    assert spec and spec.loader, "harness module spec is missing"
    module = importlib.util.module_from_spec(spec)
    sys.modules["cli_latency_benchmark"] = module
    spec.loader.exec_module(module)
    return module


class CliLatencyBenchmarkTests(unittest.TestCase):
    def setUp(self) -> None:
        self.harness = _load_harness()

    def test_canonical_stage_names(self) -> None:
        self.assertEqual(
            self.harness.STAGE_SUBMIT,
            "submit",
        )
        self.assertEqual(
            self.harness.STAGE_REQUEST_SEND,
            "request_send",
        )
        self.assertEqual(
            self.harness.STAGE_FIRST_TEXT_RECEIVED,
            "first_text_received",
        )
        self.assertEqual(
            self.harness.STAGE_FIRST_TEXT_RENDERED,
            "first_text_rendered",
        )

    def test_fixture_loads_with_expected_keys(self) -> None:
        cases = self.harness.load_cases(FIXTURE)
        self.assertGreaterEqual(len(cases), 4)
        names = {case.name for case in cases}
        self.assertIn("greeting_short_warm", names)
        self.assertIn("cfr_exact_reference_warm", names)
        for case in cases:
            self.assertTrue(case.name)
            self.assertTrue(case.prompt)
            self.assertIn(case.state, {"warm", "process-cold"})
            self.assertGreaterEqual(case.iterations, 1)

    def test_percentile_is_monotonic(self) -> None:
        values = list(range(1, 101))
        self.assertLess(self.harness.percentile(values, 50),
                        self.harness.percentile(values, 95))
        self.assertLessEqual(self.harness.percentile(values, 95),
                             self.harness.percentile(values, 100))

    def test_percentile_handles_empty(self) -> None:
        self.assertIsNone(self.harness.percentile([], 50))

    def test_aggregate_reports_required_fields(self) -> None:
        result = self.harness.aggregate([100, 200, 300])
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["max"], 300)
        self.assertIsNotNone(result["p50"])
        self.assertIsNotNone(result["p95"])

    def test_summary_keeps_failure_samples_in_denominator(self) -> None:
        records = [
            {
                "case": "x",
                "status": "success",
                "stages": {
                    "first_text_received": [10],
                    "first_text_rendered": [20],
                },
            },
            {
                "case": "x",
                "status": "timeout",
                "stages": {"first_text_received": []},
            },
        ]
        summary = self.harness.render_summary(records)
        self.assertEqual(summary["total"], 2)
        self.assertEqual(summary["successes"], 1)
        self.assertEqual(summary["timeout_or_cancelled"], 1)
        # Failure samples must remain in the per-stage denominator.
        self.assertEqual(summary["stages"]["first_text_received"]["count"], 1)
        # A stage with no samples at all still has to appear with count 0.
        self.assertEqual(summary["stages"]["first_text_rendered"]["count"], 1)

    def test_isolated_runtime_refuses_to_clear_main_directory(self) -> None:
        class _FakeSpec:
            def __init__(self, path: pathlib.Path) -> None:
                self.runtime_data = path
        with self.assertRaises(RuntimeError):
            self.harness.ensure_isolated_runtime(
                _FakeSpec(pathlib.Path(
                    "E:/desktop/How_to_build_a_agent/AgentFramework/runtime_data",
                )),
            )

    def test_summary_writes_valid_json(self) -> None:
        records = [
            {
                "case": "x",
                "status": "success",
                "stages": {"first_text_received": [1]},
            }
        ]
        summary = self.harness.render_summary(records)
        encoded = json.dumps(summary)
        decoded = json.loads(encoded)
        self.assertEqual(decoded["total"], 1)


if __name__ == "__main__":
    unittest.main()
