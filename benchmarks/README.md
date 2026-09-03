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

Every operation creates fresh Runtime collaborators for the Runtime scenarios.
Each measured iteration times one complete batch. Per-operation latency for
that sample is the batch wall time divided by its operation count; this reduces
timer and operating-system scheduling noise for microsecond-scale operations.
Warm-up batches are excluded. `total_duration_us` retains the actual sum of
timed batch wall time rather than the divided per-operation samples.
`std::chrono::steady_clock` measures each complete batch.

## Metrics and regression policy

Schema-v2 JSON distinguishes the batch `sample_count` from operation-level
`operation_count`, `success_count`, and `error_count`. It also contains actual
total measured duration; per-operation minimum, mean, P50, P95, P99, and
maximum latency in microseconds; operations per second; and operation success
rate. Percentiles use linear interpolation over sorted batch-average samples.

When `--baseline` is supplied, it must have the supported schema and the same
warm-up, iteration, batch-size, clock, scripted/network method, operating
system, compiler, build configuration, and exact unique scenario set. Its
summary fields must also be internally consistent and their sample/operation
counts must match the declared workload. The baseline and output must be
distinct files, including through equivalent filesystem paths, so a comparison
cannot overwrite its evidence. An incompatible baseline is rejected with exit
code `2`. The comparison report records the baseline path and SHA-256 so the
comparison input can be audited. After validation, scenarios are matched by
exact name and comparison passes only when:

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
  --warmup 10 --iterations 100 --batch-size 1000 `
  --output benchmarks/results/2026-09-03-windows-msvc-release-baseline.json

& .\build\vs2022\Release\agent_benchmark.exe `
  --warmup 10 --iterations 100 --batch-size 1000 `
  --baseline benchmarks/results/2026-09-03-windows-msvc-release-baseline.json `
  --max-regression-percent 15 `
  --output benchmarks/results/2026-09-03-windows-msvc-release-comparison.json
```

The recorded comparison completed three scenarios with 100 timed batches of
1,000 operations each, 100% success, and no P95-latency or throughput
regression over the 15% policy. Batch size and threshold were calibrated after
repeated Windows runs showed that microsecond-scale single-operation samples
made a 10% gate noisy. The JSON files are the evidence; the numbers are
machine- and build-specific snapshots, not universal performance claims.

## Claim boundary

This work supports `Benchmark Testing` and controlled `Software Benchmarking`
for the Agent Runtime. It does **not** support claims of GPU/HPC benchmarking,
LLM inference benchmarking, live Provider performance, or concurrent production
load testing. Those require separate workloads and measurement systems.
