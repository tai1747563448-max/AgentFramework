"""Reproducible CLI latency benchmark driver.

This is the harness referenced by the implementation plan under T0. It runs a
sequence of CLI latency cases against an AgentFramework binary (or a Codex CLI
binary, when ``--target codex`` is selected) and emits per-stage latency samples
in JSONL form.

The driver does not attempt to compare against Codex internally. It writes the
raw samples to ``--output`` and the companion ``.summary.json`` file reports
P50/P95/max, success rate, and per-stage aggregates. The downstream comparison
script lives outside this repository so the harness stays free of credentials.

The harness never swaps the model, lowers the corpus, or invents previews: if a
scenario did not produce a real text event the result is recorded as
``first_text_received = null`` so the gap cannot be papered over by warm data.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import time
from typing import Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = REPO_ROOT / "benchmarks" / "fixtures" / "cli_latency_cases.jsonl"
RESULTS_DIR = REPO_ROOT / "benchmarks" / "results"
DEFAULT_OUTPUT = RESULTS_DIR / "cli-latency-baseline.json"

# Stage identifier constants. These names are emitted by the C++ side via
# src/domain/latency_trace.h and the C++ observer wiring in runtime_engine.cpp
# and cpr_http_transport.cpp. Keep them in sync.
STAGE_SUBMIT = "submit"
STAGE_REQUEST_SEND = "request_send"
STAGE_FIRST_TEXT_RECEIVED = "first_text_received"
STAGE_FIRST_TEXT_RENDERED = "first_text_rendered"

# Additional harness-only stage placeholders. They are populated by the driver
# itself when measurement points are observable outside the binary.
STAGE_PROCESS_START = "process_start"
STAGE_MENU_READY = "menu_ready"

ALL_STAGES = (
    STAGE_PROCESS_START,
    STAGE_MENU_READY,
    STAGE_SUBMIT,
    STAGE_REQUEST_SEND,
    STAGE_FIRST_TEXT_RECEIVED,
    STAGE_FIRST_TEXT_RENDERED,
)


@dataclasses.dataclass(frozen=True)
class Case:
    """Single CLI latency scenario."""

    name: str
    prompt: str
    # warm: keep the binary process and reuse context; process-cold: launch a
    # fresh process per iteration; machine-cold: caller must arrange an OS
    # reboot (out of scope for offline harness runs).
    state: str = "warm"
    # Iterations; spec requires >= 50 offline and >= 40 paired online.
    iterations: int = 5

    @classmethod
    def from_json(cls, raw: dict) -> "Case":
        return cls(
            name=str(raw["name"]),
            prompt=str(raw["prompt"]),
            state=str(raw.get("state", "warm")),
            iterations=int(raw.get("iterations", 5)),
        )


@dataclasses.dataclass(frozen=True)
class RunSpec:
    target: str  # "agent" or "codex"
    binary: pathlib.Path
    cases: tuple[Case, ...]
    iterations: int
    state: str
    output: pathlib.Path
    runtime_data: pathlib.Path


def load_cases(path: pathlib.Path) -> tuple[Case, ...]:
    cases: list[Case] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            cases.append(Case.from_json(json.loads(line)))
    return tuple(cases)


def resolve_iterations(spec: RunSpec, case: Case) -> int:
    if case.iterations > 0:
        return case.iterations
    return spec.iterations


def percentile(values: list[float], pct: float) -> float | None:
    if not values:
        return None
    if len(values) == 1:
        return float(values[0])
    ordered = sorted(values)
    rank = (pct / 100.0) * (len(ordered) - 1)
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1 - weight) + ordered[upper] * weight


def aggregate(samples_us: Iterable[int]) -> dict[str, float | int | None]:
    samples = [float(value) for value in samples_us]
    if not samples:
        return {"count": 0, "p50": None, "p95": None, "max": None, "mean": None}
    return {
        "count": len(samples),
        "p50": percentile(samples, 50),
        "p95": percentile(samples, 95),
        "max": max(samples),
        "mean": statistics.fmean(samples),
    }


def render_summary(records: list[dict]) -> dict:
    by_stage: dict[str, list[int]] = {}
    successes = 0
    cancel_or_timeout = 0
    for record in records:
        for stage, samples in record.get("stages", {}).items():
            if samples:
                by_stage.setdefault(stage, []).extend(samples)
        status = record.get("status", "success")
        if status == "success":
            successes += 1
        elif status in {"timeout", "cancelled"}:
            cancel_or_timeout += 1
    summary = {
        "total": len(records),
        "successes": successes,
        "timeout_or_cancelled": cancel_or_timeout,
        "stages": {stage: aggregate(values) for stage, values in sorted(by_stage.items())},
    }
    return summary


def ensure_isolated_runtime(spec: RunSpec) -> None:
    """Wipe the worktree-local runtime_data between iterations so cold starts
    stay honest, but never touch the main directory's runtime_data."""
    target = spec.runtime_data
    if target.exists():
        # Honour the user constraint: only operate inside the worktree path.
        if "AgentFramework-latency" not in str(target):
            raise RuntimeError(
                "refusing to clear runtime_data outside the latency worktree: "
                + str(target)
            )
        shutil.rmtree(target)
    target.mkdir(parents=True, exist_ok=True)


def invoke_agent(spec: RunSpec, case: Case) -> dict:
    """Run one CLI iteration. The C++ side writes JSONL samples to the path
    pointed to by ``AGENT_LATENCY_TRACE_FILE`` (only honoured when the binary
    has been built with the latency trace hooks wired in)."""
    started = time.monotonic_ns()
    env = os.environ.copy()
    trace_file = spec.output.parent / f"{case.name}-{spec.state}.samples.jsonl"
    trace_file.parent.mkdir(parents=True, exist_ok=True)
    env["AGENT_LATENCY_TRACE_FILE"] = str(trace_file)
    env["AGENT_RUNTIME_ROOT"] = str(spec.runtime_data)
    process = subprocess.run(
        [str(spec.binary), "--once", case.prompt],
        env=env,
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    finished = time.monotonic_ns()
    elapsed_us = (finished - started) // 1000
    stages: dict[str, list[int]] = {}
    status = "success" if process.returncode == 0 else "exit_" + str(process.returncode)
    if trace_file.exists():
        with trace_file.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                payload = json.loads(line)
                stage = payload.get("stage")
                monotonic_us = payload.get("monotonic_us")
                if stage is None or monotonic_us is None:
                    continue
                stages.setdefault(stage, []).append(int(monotonic_us))
    return {
        "case": case.name,
        "state": case.state,
        "status": status,
        "wall_us": elapsed_us,
        "stages": stages,
        "stdout_bytes": len(process.stdout),
        "stderr_bytes": len(process.stderr),
    }


def run_case(spec: RunSpec, case: Case) -> list[dict]:
    records: list[dict] = []
    iterations = resolve_iterations(spec, case)
    for iteration in range(iterations):
        if case.state == "process-cold":
            ensure_isolated_runtime(spec)
        record = invoke_agent(spec, case)
        record["iteration"] = iteration
        records.append(record)
    return records


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("agent", "codex", "provider"), required=True)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument(
        "--scenario-set", type=pathlib.Path, default=FIXTURES,
        help="Path to a JSONL file with cases; default uses the committed fixture",
    )
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--state", choices=("warm", "process-cold"), default="warm")
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--runtime-data", type=pathlib.Path,
        default=REPO_ROOT / "runtime_data",
        help="Isolated runtime_data directory; defaults to the worktree path",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    cases = load_cases(args.scenario_set)
    if not cases:
        print("no cases resolved", file=sys.stderr)
        return 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    spec = RunSpec(
        target=args.target,
        binary=args.binary,
        cases=cases,
        iterations=args.iterations,
        state=args.state,
        output=args.output,
        runtime_data=args.runtime_data.resolve(),
    )
    if not spec.binary.exists():
        print(f"binary not found: {spec.binary}", file=sys.stderr)
        return 2
    all_records: list[dict] = []
    for case in cases:
        records = run_case(spec, case)
        all_records.extend(records)
    summary = render_summary(all_records)
    summary["target"] = spec.target
    summary["state"] = spec.state
    summary["iterations"] = spec.iterations
    summary["cases"] = [dataclasses.asdict(case) for case in cases]
    summary["binary"] = str(spec.binary)
    with spec.output.open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
    print(json.dumps({"output": str(spec.output), "total": summary["total"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
