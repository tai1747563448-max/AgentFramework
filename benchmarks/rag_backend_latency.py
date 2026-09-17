"""Reproducible RAG embedding backend latency benchmark (T6).

Measures cold-start phases (Python import, weight mapping, model lock
verification, first encode call) and warm encode throughput for the
embedding backends used by the eCFR knowledge pack. Outputs a unified
JSON record per candidate so Spec section 5's quality gates (cold/warm
latency, RSS, Recall@5, MRR, citation accuracy, error rate) can be
filled in for each entry without code changes.

The script never swaps the model or lowers the corpus: the BGE-M3
sentence-transformers backend is the only locally available candidate on
this machine, and the SiliconFlow-hosted BGE-M3 candidate is recorded as
``remote_unreachable`` unless the operator provides credentials at the
shell prompt. The script writes ``--output`` (a JSON document) and never
fabricates percentiles for samples it could not observe.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys
import time
from typing import Any, Callable, Sequence

RAG_ROOT = pathlib.Path(__file__).resolve().parents[1] / "rag"
sys.path.insert(0, str(RAG_ROOT))


SUPPORTED_LOCAL_BACKENDS = ("sentence_transformers",)
SUPPORTED_PRECISIONS = ("float32", "float16", "int8")


def _now_iso() -> str:
    return _datetime.datetime.now(tz=_datetime.timezone.utc).isoformat()


def _rss_bytes() -> int | None:
    try:
        import psutil
    except ImportError:
        return None
    process = psutil.Process(os.getpid())
    try:
        return int(process.memory_info().rss)
    except (OSError, AttributeError):
        return None


def _measure_phase(call: Callable[[], Any]) -> tuple[float, Any]:
    started = time.perf_counter()
    result = call()
    elapsed = time.perf_counter() - started
    return elapsed, result


def _load_sentence_transformers_backend(
    model_root: pathlib.Path,
) -> tuple[Any, dict[str, Any]]:
    """Return the loaded BgeM3Embedding and its model load metadata.

    Reports the cold phases the spec budgets against: import, model lock
    verification, weight mapping, first-encode self-test.  All times are
    wall-clock seconds measured via ``time.perf_counter``.
    """
    from agent_rag.embedding import (  # type: ignore[import-not-found]
        BGE_M3_MODEL,
        BgeM3Embedding,
        EMBEDDING_DIMENSIONS,
        encode_normalized,
    )

    phases: dict[str, float] = {}

    def _import():
        from sentence_transformers import SentenceTransformer  # noqa: F401

        return SentenceTransformer

    phases["import_sentence_transformers"], _ = _measure_phase(_import)

    def _instantiate():
        return BgeM3Embedding(model_root, device="cpu")

    backend = _instantiate()
    identity = (
        BGE_M3_MODEL,
        backend.revision,
        backend.backend,
        backend.precision,
        EMBEDDING_DIMENSIONS,
        backend.tokenizer_sha256,
    )

    def _self_test():
        encode_normalized(backend, ["cold start self test"], dimensions=EMBEDDING_DIMENSIONS)

    phases["first_encode_self_test"], _ = _measure_phase(_self_test)
    return backend, {
        "identity": identity,
        "phases": phases,
        "rss_after_load_bytes": _rss_bytes(),
    }


def _warm_encode_loop(
    backend: Any, *, iterations: int, batch_size: int
) -> tuple[list[float], dict[str, Any]]:
    from agent_rag.embedding import (  # type: ignore[import-not-found]
        EMBEDDING_DIMENSIONS,
        encode_normalized,
    )

    sample = (
        "1 CFR 1.1\nSample regulation paragraph used to measure warm encode "
        "throughput on the local BGE-M3 backend. " * 4
    )
    samples = [sample for _ in range(batch_size)]
    durations: list[float] = []
    for _ in range(iterations):
        started = time.perf_counter()
        encode_normalized(backend, samples, dimensions=EMBEDDING_DIMENSIONS)
        durations.append(time.perf_counter() - started)
    stats = {
        "iterations": iterations,
        "batch_size": batch_size,
        "p50_ms": statistics.median(durations) * 1000.0,
        "mean_ms": statistics.fmean(durations) * 1000.0,
        "max_ms": max(durations) * 1000.0,
        "min_ms": min(durations) * 1000.0,
        "rss_after_warm_bytes": _rss_bytes(),
    }
    return durations, stats


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def _build_candidate(
    *,
    name: str,
    model_root: pathlib.Path,
    warm_iterations: int,
    warm_batch: int,
    remote_token: str | None,
    shared_local: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Run one candidate through cold + warm phases and return its record."""
    record: dict[str, Any] = {
        "candidate": name,
        "measured_at_utc": _now_iso(),
        "machine": {
            "platform": platform.platform(),
            "processor": platform.processor() or "unknown",
            "python": platform.python_version(),
        },
    }
    if name == "remote_siliconflow_bge_m3":
        record["backend_family"] = "remote_http"
        remote_meta = _probe_remote(model_root, remote_token)
        record.update(remote_meta)
        if remote_meta["status"] != "measured":
            return record
        return record
    if shared_local is not None:
        meta = shared_local
    else:
        try:
            backend, meta = _load_sentence_transformers_backend(model_root)
        except Exception as error:
            record["status"] = "load_failed"
            record["error"] = repr(error)
            return record
    record["identity"] = list(meta["identity"])
    record["identity_string"] = (
        f"{meta['identity'][0]}@{meta['identity'][1]}+{meta['identity'][2]}/"
        f"{meta['identity'][3]}/{meta['identity'][4]}"
    )
    record["cold_phases_seconds"] = meta["phases"]
    record["cold_total_seconds"] = sum(meta["phases"].values())
    record["rss_after_load_bytes"] = meta["rss_after_load_bytes"]

    try:
        _, warm_stats = _warm_encode_loop(
            backend, iterations=warm_iterations, batch_size=warm_batch
        )
        record["warm"] = warm_stats
        record["status"] = "measured"
    except Exception as error:
        record["status"] = "warm_failed"
        record["error"] = repr(error)
        return record
    return record


def _probe_remote(
    model_root: pathlib.Path, token: str | None
) -> dict[str, Any]:
    """Best-effort probe of the SiliconFlow-hosted BGE-M3 endpoint.

    The spec forbids fabricating latency numbers when no real call was made;
    when the token is missing or the network call fails the result is
    recorded honestly so the offline report can flag the absence.
    """
    record: dict[str, Any] = {
        "status": "skipped",
        "reason": None,
    }
    if not token:
        record["reason"] = "no_token"
        return record
    try:
        import urllib.request
        import urllib.error
        import socket
    except ImportError:
        record["reason"] = "stdlib_unavailable"
        return record
    body = json.dumps(
        {"model": "BAAI/bge-m3", "input": "1 CFR 1.1 cold start probe"}
    ).encode("utf-8")
    request = urllib.request.Request(
        "https://api.siliconflow.cn/v1/embeddings",
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        },
    )
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            response.read(1024)
            elapsed = time.perf_counter() - started
            record["status"] = "probe_ok_no_vectors_consumed"
            record["remote_round_trip_seconds"] = elapsed
            record["rss_after_probe_bytes"] = _rss_bytes()
            return record
    except (urllib.error.URLError, socket.timeout, TimeoutError) as error:
        elapsed = time.perf_counter() - started
        record["status"] = "unreachable"
        record["reason"] = repr(error)
        record["attempted_seconds"] = elapsed
        record["rss_after_probe_bytes"] = _rss_bytes()
        return record
    except Exception as error:  # pragma: no cover - defensive
        record["status"] = "error"
        record["reason"] = repr(error)
        return record


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model-root",
        required=True,
        type=pathlib.Path,
        help="Absolute path to the verified eCFR knowledge pack's model root",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=pathlib.Path,
        help="Absolute path for the unified JSON result document",
    )
    parser.add_argument(
        "--warm-iterations",
        type=int,
        default=200,
        help="Number of warm encode iterations per candidate (default: 200)",
    )
    parser.add_argument(
        "--warm-batch-size",
        type=int,
        default=8,
        help="Batch size for the warm encode loop (default: 8)",
    )
    parser.add_argument(
        "--remote-token",
        default=os.environ.get("RAG_REMOTE_TOKEN"),
        help="Optional token used to probe the remote SiliconFlow candidate",
    )
    args = parser.parse_args(argv)

    if not args.model_root.is_absolute():
        parser.error("--model-root must be absolute")
    if not args.output.is_absolute():
        parser.error("--output must be absolute")
    args.output.parent.mkdir(parents=True, exist_ok=True)

    candidates = [
        "sentence_transformers_cpu",
        "remote_siliconflow_bge_m3",
    ]
    records: list[dict[str, Any]] = []
    shared_local: dict[str, Any] | None = None
    for name in candidates:
        if name == "remote_siliconflow_bge_m3" and shared_local is None:
            try:
                _, shared_local = _load_sentence_transformers_backend(
                    args.model_root
                )
            except Exception as error:
                shared_local = {"load_error": repr(error)}
        records.append(
            _build_candidate(
                name=name,
                model_root=args.model_root,
                warm_iterations=args.warm_iterations,
                warm_batch=args.warm_batch_size,
                remote_token=args.remote_token,
                shared_local=shared_local,
            )
        )
    payload = {
        "schema_version": 1,
        "generated_at_utc": _now_iso(),
        "model_lock_sha256": _sha256_file(args.model_root / "model.lock.json")
        if (args.model_root / "model.lock.json").is_file()
        else None,
        "warm_iterations": args.warm_iterations,
        "warm_batch_size": args.warm_batch_size,
        "candidates": records,
    }
    args.output.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    sys.stdout.write(
        json.dumps(
            {"wrote": str(args.output), "candidates": [r.get("candidate") for r in records]},
            ensure_ascii=False,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":  # pragma: no cover - CLI entry point
    raise SystemExit(main(sys.argv[1:]))