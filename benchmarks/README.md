# Agent Runtime Benchmark

This benchmark makes the performance claim narrow, repeatable, and auditable:
it measures deterministic C++ Agent Runtime paths with no credentials, external
network, or model inference.

## Scenarios

| Scenario | Controlled work |
| --- | --- |
| `text_completion_runtime` | One context preparation and one scripted text response through `RuntimeEngine` |
| `tool_round_trip_runtime` | Scripted tool request, deterministic tool result, and final model response |
| `event_log_evaluation` | Strict replay and fixed `TaskEvaluation` metrics over a completed event trace |

Every measured iteration creates fresh Runtime collaborators for the Runtime
scenarios. Warm-up iterations are executed but excluded from the report.
`std::chrono::steady_clock` measures wall-clock latency around each complete
operation.

## Metrics and regression policy

The JSON report contains sample, success, and error counts; total, minimum,
mean, P50, P95, P99, and maximum latency in microseconds; operations per second;
and success rate. Percentiles use linear interpolation over sorted samples.

When `--baseline` is supplied, scenarios are matched by exact name. Comparison
passes only when:

- current P95 latency does not exceed the baseline by more than
  `--max-regression-percent`;
- current throughput does not fall by more than the same percentage; and
- every measured operation succeeds.

Exit code `0` means the run and requested comparison passed, `2` means invalid
arguments/baseline/report output, and `3` means an execution failure or detected
regression.

## Reproduce the checked-in run

Build and run the Release target from the repository root:

```powershell
cmake --build build/vs2022 --config Release --target agent_benchmark

& .\build\vs2022\Release\agent_benchmark.exe `
  --warmup 100 --iterations 10000 `
  --output benchmarks/results/2026-09-03-windows-msvc-release-baseline.json

& .\build\vs2022\Release\agent_benchmark.exe `
  --warmup 100 --iterations 10000 `
  --baseline benchmarks/results/2026-09-03-windows-msvc-release-baseline.json `
  --max-regression-percent 10 `
  --output benchmarks/results/2026-09-03-windows-msvc-release-comparison.json
```

The recorded comparison completed three scenarios with 10,000 measured
iterations each, 100% success, and no P95-latency or throughput regression over
the 10% policy. The JSON files are the evidence; the numbers are machine- and
build-specific snapshots, not universal performance claims.

## Claim boundary

This work supports `Benchmark Testing` and controlled `Software Benchmarking`
for the Agent Runtime. It does **not** support claims of GPU/HPC benchmarking,
LLM inference benchmarking, live Provider performance, or concurrent production
load testing. Those require separate workloads and measurement systems.
