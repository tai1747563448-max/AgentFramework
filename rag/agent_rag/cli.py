from __future__ import annotations

from dataclasses import asdict
import json
import os
from pathlib import Path
import sys

import numpy as np

from .embedding import (
    BgeM3Embedding,
    EmbeddingError,
    encode_normalized,
    select_embedding_device,
)
from .ecfr_document import build_corpus_from_downloads
from .ecfr_source import DownloadConfig, download_ecfr_snapshot
from .hybrid_index import build_hybrid_index
from .indexer import build_index
from .pack import verify_complete_pack
from .progress import ProgressReporter
from .protocol import parse_query_request, response_json
from .retriever import query_index
from .evaluation import (
    evaluate_pack,
    generate_evaluation_cases,
    stable_report_json,
    write_evaluation_cases,
)


def _build(arguments: list[str]) -> int:
    if len(arguments) != 5 or arguments[1] != "--source" or arguments[3] != "--index":
        raise ValueError("invalid build arguments")
    summary = build_index(Path(arguments[2]), Path(arguments[4]))
    sys.stdout.write(
        json.dumps(asdict(summary), ensure_ascii=False, separators=(",", ":"))
        + "\n"
    )
    return 0


def _query(arguments: list[str]) -> int:
    if len(arguments) != 3 or arguments[1] != "--index":
        raise ValueError("invalid query arguments")
    request = parse_query_request(sys.stdin.read(65_537))
    items = query_index(
        Path(arguments[2]),
        request.query,
        top_k=request.top_k,
        max_total_bytes=request.max_total_bytes,
    )
    sys.stdout.write(response_json(items) + "\n")
    return 0


def _download_ecfr(arguments: list[str]) -> int:
    if (
        len(arguments) != 5
        or arguments[1] != "--snapshot"
        or arguments[3] != "--staging-root"
    ):
        raise ValueError("invalid download-ecfr arguments")
    summary = download_ecfr_snapshot(
        DownloadConfig(arguments[2], Path(arguments[4]))
    )
    sys.stdout.write(
        json.dumps(
            {
                "schema_version": 2,
                "snapshot_date": summary.snapshot_date,
                "title_count": summary.title_count,
                "downloaded": summary.downloaded,
                "reused": summary.reused,
            },
            ensure_ascii=False,
            separators=(",", ":"),
        )
        + "\n"
    )
    return 0


def _build_corpus(arguments: list[str]) -> int:
    if (
        len(arguments) != 7
        or arguments[1] != "--snapshot"
        or arguments[3] != "--staging-root"
        or arguments[5] != "--count"
    ):
        raise ValueError("invalid build-corpus arguments")
    count = int(arguments[6])
    summary = build_corpus_from_downloads(
        Path(arguments[4]), snapshot_date=arguments[2], count=count
    )
    sys.stdout.write(
        json.dumps(asdict(summary), ensure_ascii=False, separators=(",", ":"))
        + "\n"
    )
    return 0


def _load_tested_embedding(
    model_root: Path, requested_device: str
) -> tuple[BgeM3Embedding, str]:
    selected = select_embedding_device(requested_device)
    try:
        embedding = BgeM3Embedding(model_root, device=selected)
        encode_normalized(embedding, ["embedding device self test"], dimensions=1024)
        return embedding, selected
    except EmbeddingError:
        if requested_device != "auto" or selected != "cuda":
            raise
        embedding = BgeM3Embedding(model_root, device="cpu")
        encode_normalized(embedding, ["embedding device self test"], dimensions=1024)
        return embedding, "cpu"


def _build_hybrid(arguments: list[str]) -> int:
    if (
        len(arguments) != 7
        or arguments[1] != "--pack-root"
        or arguments[3] != "--device"
        or arguments[5] != "--batch-size"
    ):
        raise ValueError("invalid build-index arguments")
    root = Path(arguments[2])
    batch_size = int(arguments[6])
    reporter = ProgressReporter(sys.stderr)
    reporter.update("build-index-command", 0, 1)
    try:
        embedding, device = _load_tested_embedding(
            root / "model" / "bge-m3", arguments[4]
        )
        summary = build_hybrid_index(
            root,
            embedding,
            device=device,
            batch_size=batch_size,
            progress=reporter.update,
        )
    except (Exception, KeyboardInterrupt):
        reporter.fail("build-index-command")
        raise
    reporter.update("build-index-command", 1, 1)
    result = asdict(summary)
    result.update(
        {
            "database": "index/metadata.sqlite3",
            "vectors": "index/vectors.f16",
            "vector_metadata": "index/vectors.json",
            "device": device,
        }
    )
    sys.stdout.write(json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n")
    return 0


def _verify_model(arguments: list[str]) -> int:
    if len(arguments) != 3 or arguments[1] != "--model-root":
        raise ValueError("invalid verify-model arguments")
    embedding, device = _load_tested_embedding(Path(arguments[2]), "auto")
    values = encode_normalized(embedding, ["model verification probe"], dimensions=1024)
    result = {
        "schema_version": 2,
        "model": embedding.model,
        "revision": embedding.revision,
        "dimensions": embedding.dimensions,
        "device": device,
        "finite": bool(np.isfinite(values).all()),
        "unit_norm": bool(np.allclose(np.linalg.norm(values, axis=1), [1.0], atol=1e-4)),
    }
    sys.stdout.write(json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n")
    return 0


def _verify_pack(arguments: list[str]) -> int:
    if len(arguments) != 3 or arguments[1] != "--pack-root":
        raise ValueError("invalid verify-pack arguments")
    reporter = ProgressReporter(sys.stderr)
    reporter.update("verify-pack-command", 0, 1)
    try:
        manifest = verify_complete_pack(
            Path(arguments[2]), progress=reporter.update
        )
    except (Exception, KeyboardInterrupt):
        reporter.fail("verify-pack-command")
        raise
    reporter.update("verify-pack-command", 1, 1)
    result = {
        "schema_version": manifest.schema_version,
        "pack_id": manifest.pack_id,
        "snapshot_date": manifest.snapshot_date,
        "document_count": manifest.document_count,
        "chunk_count": manifest.chunk_count,
        "embedding_dimensions": manifest.embedding_dimensions,
    }
    sys.stdout.write(json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n")
    return 0


def _evaluate(arguments: list[str]) -> int:
    if (
        len(arguments) != 9
        or arguments[1] != "--pack-root"
        or arguments[3] != "--cases"
        or arguments[5] != "--mode"
        or arguments[7] != "--output"
    ):
        raise ValueError("invalid evaluate arguments")
    pack_root = Path(arguments[2])
    cases = Path(arguments[4])
    mode = arguments[6]
    output = Path(arguments[8])
    if (
        not pack_root.is_absolute()
        or not cases.is_absolute()
        or not output.is_absolute()
        or mode not in {"lexical", "dense", "hybrid"}
        or not output.parent.is_dir()
        or output.is_symlink()
    ):
        raise ValueError("invalid evaluate arguments")
    reporter = ProgressReporter(sys.stderr)
    reporter.update("evaluate-command", 0, 1, mode=mode)
    try:
        report = evaluate_pack(
            pack_root, cases, mode=mode, progress=reporter.update
        )
        temporary = output.with_name(output.name + f".partial-{os.getpid()}")
        if temporary.exists() or temporary.is_symlink():
            raise ValueError("invalid evaluate output")
        try:
            with temporary.open("x", encoding="utf-8", newline="") as stream:
                stream.write(stable_report_json(report))
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, output)
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
    except (Exception, KeyboardInterrupt):
        reporter.fail("evaluate-command", mode=mode)
        raise
    reporter.update("evaluate-command", 1, 1, mode=mode)
    return 0


def _generate_eval(arguments: list[str]) -> int:
    if (
        len(arguments) not in {7, 9}
        or arguments[1] != "--index-root"
        or arguments[3] != "--negative-cases"
        or (
            len(arguments) == 7
            and arguments[5] != "--output"
        )
        or (
            len(arguments) == 9
            and (arguments[5] != "--curated-cases" or arguments[7] != "--output")
        )
    ):
        raise ValueError("invalid generate-eval arguments")
    index_root = Path(arguments[2])
    negatives = Path(arguments[4])
    curated = Path(arguments[6]) if len(arguments) == 9 else None
    output = Path(arguments[8] if len(arguments) == 9 else arguments[6])
    if (
        not index_root.is_absolute()
        or not negatives.is_absolute()
        or (curated is not None and not curated.is_absolute())
        or not output.is_absolute()
    ):
        raise ValueError("invalid generate-eval arguments")
    reporter = ProgressReporter(sys.stderr)
    reporter.update("generate-eval-command", 0, 1, mode="generation")
    try:
        cases = generate_evaluation_cases(
            index_root, negatives, curated, progress=reporter.update
        )
        write_evaluation_cases(output, cases)
    except (Exception, KeyboardInterrupt):
        reporter.fail("generate-eval-command", mode="generation")
        raise
    reporter.update("generate-eval-command", 1, 1, mode="generation")
    return 0


def _serve(arguments: list[str]) -> int:
    if (
        len(arguments) not in {3, 5}
        or arguments[1] != "--pack-root"
        or (len(arguments) == 5 and arguments[3] != "--device")
    ):
        raise ValueError("invalid serve arguments")
    from .sidecar import run_sidecar

    device = arguments[4] if len(arguments) == 5 else "auto"
    return run_sidecar(
        Path(arguments[2]), sys.stdin.buffer, sys.stdout.buffer, sys.stderr,
        device=device,
    )


def main(arguments: list[str] | None = None) -> int:
    values = list(sys.argv[1:] if arguments is None else arguments)
    command = values[0] if values else ""
    try:
        if command == "build":
            return _build(values)
        if command == "query":
            return _query(values)
        if command == "download-ecfr":
            return _download_ecfr(values)
        if command == "build-corpus":
            return _build_corpus(values)
        if command == "build-index":
            return _build_hybrid(values)
        if command == "verify-model":
            return _verify_model(values)
        if command == "verify-pack":
            return _verify_pack(values)
        if command == "evaluate":
            return _evaluate(values)
        if command == "generate-eval":
            return _generate_eval(values)
        if command == "serve":
            return _serve(values)
        raise ValueError("unknown command")
    except Exception:
        if command == "build":
            sys.stderr.write("rag build failed\n")
        elif command == "query":
            sys.stderr.write("rag query failed\n")
        elif command == "download-ecfr":
            sys.stderr.write("eCFR download failed\n")
        elif command == "build-corpus":
            sys.stderr.write("eCFR corpus build failed\n")
        elif command == "build-index":
            sys.stderr.write("hybrid index build failed\n")
        elif command == "verify-model":
            sys.stderr.write("embedding model verification failed\n")
        elif command == "verify-pack":
            sys.stderr.write("Knowledge Pack verification failed\n")
        elif command == "evaluate":
            sys.stderr.write("retrieval evaluation failed\n")
        elif command == "generate-eval":
            sys.stderr.write("retrieval evaluation case generation failed\n")
        elif command == "serve":
            sys.stderr.write("rag sidecar failed\n")
        else:
            sys.stderr.write("rag command failed\n")
        return 2
