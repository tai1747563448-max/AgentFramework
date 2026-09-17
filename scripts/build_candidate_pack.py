"""Build, verify, and document a candidate knowledge pack for T10.

The source pack at ``--source-pack-root`` is treated as read-only.  This
script creates an independent candidate pack under ``--candidate-pack-root``
by:

1. Copying the small control files (``pack.json``, ``model.lock.json``,
   ``runtime.lock.json``, ``manifest/documents.jsonl``,
   ``sidecar/agent_rag_cli.py``, ``build.intent.json``,
   ``reports/retrieval-eval.json``, ``eval/ecfr-cases.jsonl``) into the
   candidate root.
2. Copying the index directory and applying the schema 2 → 3 upgrade so
   the candidate ships ``row_count`` and ``sum_token_count`` without
   re-encoding any vector.
3. Injecting the T6 backend identity (``backend=sentence_transformers``,
   ``precision=float32``) into both the SQLite metadata table and the
   ``vectors.json`` document so ``retrieval_revision`` can bind to it.
4. Re-writing the candidate's ``pack.json`` so the index file digests
   match the upgraded bytes, pruning any record whose payload has not
   yet been staged (corpus markdowns, runtime interpreter, stdlib,
   wheels, model payload).  The remaining payload is staged later by
   ``cmake/stage_ready_package.cmake``.
5. Running a focused verification on the data surface — every present
   file's size and SHA-256 matches ``pack.json``; the upgraded SQLite
   metadata carries schema_version=3, ``row_count``, ``sum_token_count``,
   ``backend``, ``precision``, and the ``chunks_vector_row_cover``
   covering index; ``vectors.f16`` is byte-identical to the source so
   the upgrade is a metadata-only operation.

The candidate pack never shares files with the source through hard
links, junctions, or symbolic links.  ``shutil.copyfile`` is used so the
candidate owns its bytes.  The script refuses to operate if the
candidate root already exists or if the source root is not absolute.

Usage::

    python scripts/build_candidate_pack.py \\
        --source-pack-root E:/.../ecfr-2026-09-03 \\
        --candidate-pack-root \\
            E:/desktop/How_to_build_a_agent/AgentFramework-latency/out/candidate-pack/ecfr-2026-09-17-latency \\
        --report-out \\
            E:/desktop/How_to_build_a_agent/AgentFramework-latency/out/validation/2026-09-17-t10-candidate-pack.json
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import sys


_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))


from rag.agent_rag.hybrid_index import (  # type: ignore[import-not-found]
    CHUNKS_VECTOR_ROW_COVER,
    INDEX_SCHEMA_VERSION,
    INDEX_SCHEMA_VERSION_LEGACY,
    _sha256_file,
)
from rag.agent_rag.pack import (  # type: ignore[import-not-found]
    DEFAULT_EMBEDDING_BACKEND,
    DEFAULT_EMBEDDING_PRECISION,
    verify_complete_pack,
)


_SMALL_CONTROL_FILES = (
    "pack.json",
    "model.lock.json",
    "runtime.lock.json",
    "manifest/documents.jsonl",
    "sidecar/agent_rag_cli.py",
    "build.intent.json",
    "reports/retrieval-eval.json",
    "eval/ecfr-cases.jsonl",
)


class BuildError(RuntimeError):
    pass


def _now_iso() -> str:
    return _datetime.datetime.now(tz=_datetime.timezone.utc).isoformat()


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _copy_small_controls(source: Path, candidate: Path) -> list[str]:
    copied: list[str] = []
    for relative in _SMALL_CONTROL_FILES:
        source_path = source / relative
        if not source_path.is_file():
            raise BuildError(f"required file is missing in source pack: {relative}")
        candidate_path = candidate / relative
        candidate_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_path, candidate_path)
        copied.append(relative)
    return copied


def _copy_index(source: Path, candidate: Path) -> None:
    source_index = source / "index"
    candidate_index = candidate / "index"
    if not source_index.is_dir():
        raise BuildError("source pack index directory is missing")
    candidate_index.mkdir(parents=True, exist_ok=True)
    for name in ("metadata.sqlite3", "vectors.f16", "vectors.json"):
        shutil.copyfile(source_index / name, candidate_index / name)


def _read_legacy_vector_metadata(index_root: Path) -> dict[str, object]:
    path = index_root / "vectors.json"
    value = json.loads(path.read_text(encoding="utf-8"))
    if (
        type(value) is not dict
        or value.get("schema_version") != INDEX_SCHEMA_VERSION_LEGACY
    ):
        raise BuildError("source vectors.json is not on the legacy schema")
    return value


def _compute_statistics(database: Path) -> tuple[int, int]:
    connection = sqlite3.connect(
        f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
    )
    try:
        connection.execute("PRAGMA query_only=ON")
        rows = connection.execute("SELECT token_count FROM chunks").fetchall()
        sum_token_count = 0
        for (token_count,) in rows:
            if type(token_count) is not int or token_count <= 0:
                raise BuildError("token_count is invalid")
            sum_token_count += token_count
        return len(rows), sum_token_count
    finally:
        connection.close()


def _verify_contiguous_mapping(database: Path, expected_row_count: int) -> None:
    connection = sqlite3.connect(
        f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
    )
    try:
        connection.execute("PRAGMA query_only=ON")
        vector_map = [
            int(vector_row)
            for (vector_row,) in connection.execute(
                "SELECT vector_row FROM chunks ORDER BY vector_row"
            )
        ]
        if vector_map != list(range(expected_row_count)):
            raise BuildError("vector_row mapping is not contiguous")
    finally:
        connection.close()


def _upgrade_index(
    candidate_index: Path,
    *,
    backend: str,
    precision: str,
) -> dict[str, object]:
    database = candidate_index / "metadata.sqlite3"
    matrix = candidate_index / "vectors.f16"
    metadata_path = candidate_index / "vectors.json"
    legacy = _read_legacy_vector_metadata(candidate_index)
    matrix_sha = _sha256_file(matrix)
    database_sha = _sha256_file(database)
    if legacy.get("matrix_sha256") != matrix_sha:
        raise BuildError("matrix sha256 is invalid")
    if legacy.get("database_sha256") != database_sha:
        raise BuildError("database sha256 is invalid")
    row_count, sum_token_count = _compute_statistics(database)
    _verify_contiguous_mapping(database, row_count)
    if legacy.get("rows") != row_count:
        raise BuildError("legacy rows metadata does not match database")

    staging = candidate_index.parent / ".staging-upgrade"
    if staging.exists():
        shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True, exist_ok=True)
    try:
        new_database = staging / "metadata.sqlite3"
        new_metadata_path = staging / "vectors.json"
        shutil.copyfile(database, new_database)
        connection = sqlite3.connect(new_database)
        try:
            connection.execute("PRAGMA foreign_keys=ON")
            connection.execute(
                "UPDATE metadata SET value = ? WHERE key = 'schema_version'",
                (str(INDEX_SCHEMA_VERSION),),
            )
            for key, value in (
                ("row_count", str(row_count)),
                ("sum_token_count", str(sum_token_count)),
                ("backend", backend),
                ("precision", precision),
            ):
                existing = connection.execute(
                    "SELECT 1 FROM metadata WHERE key = ?", (key,)
                ).fetchone()
                if existing is None:
                    connection.execute(
                        "INSERT INTO metadata VALUES(?, ?)", (key, value)
                    )
                else:
                    connection.execute(
                        "UPDATE metadata SET value = ? WHERE key = ?",
                        (value, key),
                    )
            connection.execute(
                "CREATE INDEX IF NOT EXISTS chunks_vector_row_cover "
                "ON chunks(vector_row, chunk_id)"
            )
            connection.commit()
            if connection.execute("PRAGMA foreign_key_check").fetchall():
                raise BuildError("upgraded database has broken references")
            if connection.execute("PRAGMA integrity_check").fetchone() != ("ok",):
                raise BuildError("upgraded database failed integrity check")
        finally:
            connection.close()
        new_metadata = dict(legacy)
        new_metadata["schema_version"] = INDEX_SCHEMA_VERSION
        new_metadata["row_count"] = row_count
        new_metadata["sum_token_count"] = sum_token_count
        new_metadata["backend"] = backend
        new_metadata["precision"] = precision
        new_metadata["database_sha256"] = _sha256_file(new_database)
        new_metadata_path.write_text(
            json.dumps(new_metadata, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
            newline="",
        )
        backup_database = database.with_name(database.name + ".legacy-backup")
        backup_metadata = metadata_path.with_name(
            metadata_path.name + ".legacy-backup"
        )
        shutil.move(str(database), str(backup_database))
        shutil.move(str(metadata_path), str(backup_metadata))
        try:
            shutil.move(str(new_database), str(database))
            shutil.move(str(new_metadata_path), str(metadata_path))
        except Exception:
            shutil.move(str(backup_database), str(database))
            shutil.move(str(backup_metadata), str(metadata_path))
            raise
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return {
        "schema_version": INDEX_SCHEMA_VERSION,
        "row_count": row_count,
        "sum_token_count": sum_token_count,
        "backend": backend,
        "precision": precision,
        "covering_index": CHUNKS_VECTOR_ROW_COVER,
        "vectors_unchanged": True,
        "database_sha256": _sha256_file(database),
        "matrix_sha256": matrix_sha,
    }


def _sync_pack_manifest_index_records(
    candidate: Path,
    *,
    matrix_sha: str,
    database_sha: str,
    matrix_bytes: int,
    database_bytes: int,
    vectors_json_bytes: int,
    vectors_json_sha: str,
) -> dict[str, object]:
    """Update the candidate's pack.json for the upgraded index files.

    The source ``pack.json`` records digests for the legacy index; the
    candidate's index is rewritten during the schema 2 → 3 upgrade with
    new ``index/metadata.sqlite3`` and ``index/vectors.json`` content.
    Without this rewrite the runtime verifier rejects the candidate with
    ``pack file digest is invalid`` even though the bytes are correct.

    The candidate only ships the data surface (small controls + index),
    so ``manifest.files`` is pruned to entries whose paths exist on disk.
    The runtime payload (interpreter, stdlib, wheels, model files,
    corpus markdowns) is staged later by ``stage_ready_package.cmake``
    and is intentionally absent here.
    """
    pack_json_path = candidate / "pack.json"
    manifest = json.loads(pack_json_path.read_text(encoding="utf-8"))
    replaced: dict[str, dict[str, object]] = {}
    for record in manifest["files"]:
        path = record["path"]
        if path == "index/metadata.sqlite3":
            record["bytes"] = database_bytes
            record["sha256"] = database_sha
            replaced[path] = {"bytes": database_bytes, "sha256": database_sha}
        elif path == "index/vectors.json":
            record["bytes"] = vectors_json_bytes
            record["sha256"] = vectors_json_sha
            replaced[path] = {
                "bytes": vectors_json_bytes,
                "sha256": vectors_json_sha,
            }
        elif path == "index/vectors.f16":
            record["bytes"] = matrix_bytes
            record["sha256"] = matrix_sha
            replaced[path] = {"bytes": matrix_bytes, "sha256": matrix_sha}
    original_count = len(manifest["files"])
    kept: list[dict[str, object]] = []
    pruned: list[str] = []
    for record in manifest["files"]:
        path = record["path"]
        if (candidate / path).is_file():
            kept.append(record)
        else:
            pruned.append(path)
    manifest["files"] = kept
    pack_json_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True),
        encoding="utf-8",
        newline="",
    )
    return {
        "rewritten_files": sorted(replaced),
        "rewritten_records": replaced,
        "schema_version": manifest.get("schema_version"),
        "pack_id": manifest.get("pack_id"),
        "complete": manifest.get("complete"),
        "files_before_prune": original_count,
        "files_after_prune": len(kept),
        "pruned_file_count": len(pruned),
        "pruned_sample": sorted(pruned)[:5],
    }


def _focused_verification(
    candidate: Path,
    *,
    backend: str,
    precision: str,
    matrix_sha: str,
    source_pack_root: Path,
) -> dict[str, object]:
    """Verify the candidate's data surface without requiring the runtime payload.

    The candidate intentionally omits ``runtime/python.exe``,
    ``runtime/stdlib``, and ``runtime/wheels`` because the embedded
    interpreter and wheel payload are staged by ``stage_ready_package.cmake``
    after the data-side upgrade. ``verify_runtime_pack`` therefore cannot
    be run end-to-end here; instead this routine confirms the subset of
    invariants that the data-side upgrade must hold on its own.
    """
    issues: list[str] = []
    pack_json = candidate / "pack.json"
    manifest = json.loads(pack_json.read_text(encoding="utf-8"))
    records = {record["path"]: record for record in manifest["files"]}

    def _check(label: str, condition: bool, detail: str = "") -> None:
        issues.append(
            f"{label}: ok" if condition else f"{label}: FAIL {detail}".rstrip()
        )

    for required in (
        "index/metadata.sqlite3",
        "index/vectors.f16",
        "index/vectors.json",
        "manifest/documents.jsonl",
        "model.lock.json",
    ):
        _check(
            f"pack.json references {required}",
            required in records,
            f"(missing in records)",
        )

    for path, record in records.items():
        actual_path = candidate / path
        size = actual_path.stat().st_size
        sha = _sha256_file(actual_path)
        _check(
            f"{path} size matches pack.json",
            size == record["bytes"],
            f"(actual={size}, expected={record['bytes']})",
        )
        _check(
            f"{path} sha256 matches pack.json",
            sha == record["sha256"],
            f"(actual={sha[:12]}…, expected={record['sha256'][:12]}…)",
        )

    database = candidate / "index" / "metadata.sqlite3"
    connection = sqlite3.connect(
        f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True
    )
    try:
        connection.execute("PRAGMA query_only=ON")
        schema_version_row = connection.execute(
            "SELECT value FROM metadata WHERE key='schema_version'"
        ).fetchone()
        backend_row = connection.execute(
            "SELECT value FROM metadata WHERE key='backend'"
        ).fetchone()
        precision_row = connection.execute(
            "SELECT value FROM metadata WHERE key='precision'"
        ).fetchone()
        row_count_row = connection.execute(
            "SELECT value FROM metadata WHERE key='row_count'"
        ).fetchone()
        sum_token_row = connection.execute(
            "SELECT value FROM metadata WHERE key='sum_token_count'"
        ).fetchone()
        covering = connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='index' AND name='chunks_vector_row_cover'"
        ).fetchone()
    finally:
        connection.close()
    _check(
        "metadata.schema_version == 3",
        schema_version_row is not None and schema_version_row[0] == "3",
        f"(actual={schema_version_row[0] if schema_version_row else None})",
    )
    _check(
        "metadata.backend == requested",
        backend_row is not None and backend_row[0] == backend,
        f"(actual={backend_row[0] if backend_row else None})",
    )
    _check(
        "metadata.precision == requested",
        precision_row is not None and precision_row[0] == precision,
        f"(actual={precision_row[0] if precision_row else None})",
    )
    _check(
        "metadata.row_count present",
        row_count_row is not None and int(row_count_row[0]) > 0,
    )
    _check(
        "metadata.sum_token_count present",
        sum_token_row is not None and int(sum_token_row[0]) > 0,
    )
    _check(
        "covering index chunks_vector_row_cover present",
        covering is not None,
    )

    vectors_json = json.loads(
        (candidate / "index" / "vectors.json").read_text(encoding="utf-8")
    )
    _check(
        "vectors.json schema_version == 3",
        vectors_json.get("schema_version") == 3,
    )
    _check(
        "vectors.json backend matches",
        vectors_json.get("backend") == backend,
    )
    _check(
        "vectors.json precision matches",
        vectors_json.get("precision") == precision,
    )
    _check(
        "vectors.json matrix_sha256 unchanged",
        vectors_json.get("matrix_sha256") == matrix_sha,
    )

    source_vectors_f16 = source_pack_root / "index" / "vectors.f16"
    if source_vectors_f16.is_file():
        candidate_vectors_f16 = candidate / "index" / "vectors.f16"
        source_sha = _sha256_file(source_vectors_f16)
        candidate_sha = _sha256_file(candidate_vectors_f16)
        _check(
            "candidate vectors.f16 sha256 == source vectors.f16 sha256",
            source_sha == candidate_sha,
            "(vectors must be byte-identical to source)",
        )
        _check(
            "candidate vectors.f16 size == source vectors.f16 size",
            source_vectors_f16.stat().st_size
            == candidate_vectors_f16.stat().st_size,
        )

    failures = [item for item in issues if "FAIL" in item]
    return {
        "status": "verified" if not failures else "failed",
        "issues": issues,
        "failures": failures,
        "expected_missing": [
            "runtime/python.exe",
            "runtime/stdlib",
            "runtime/wheels",
            "runtime.lock.json hash mismatch is allowed when the "
            "embedded interpreter is not yet staged",
        ],
    }


def _candidate_files_report(candidate: Path) -> dict[str, object]:
    inventory: dict[str, int] = {}
    total_bytes = 0
    for path in candidate.rglob("*"):
        if path.is_file():
            try:
                size = path.stat().st_size
            except OSError:
                continue
            inventory[path.relative_to(candidate).as_posix()] = size
            total_bytes += size
    return {
        "total_files": len(inventory),
        "total_bytes": total_bytes,
        "small_controls": sorted(
            relative for relative in inventory if relative in _SMALL_CONTROL_FILES
        ),
        "index_files": sorted(
            relative for relative in inventory if relative.startswith("index/")
        ),
        "missing_runtime_assets": [
            "runtime/python.exe",
            "runtime/stdlib",
            "runtime/wheels",
        ],
    }


def _rollback_plan(source: Path, candidate: Path) -> dict[str, str]:
    return {
        "step_1": (
            f"Stop any process reading {candidate} before the rollback so the "
            "destination can be replaced."
        ),
        "step_2": (
            f"Delete the candidate root at {candidate} after confirming no "
            "process holds it open; the source pack remains untouched."
        ),
        "step_3": (
            f"Re-point AGENT_RAG_PACK_ROOT in the worktree .env back to "
            f"{source} (or to whichever active pack the operator chooses) "
            "and update knowledge/active-pack.json to the same path."
        ),
        "step_4": (
            "Restart the CLI; the runtime pack verifier reads the new "
            "AGENT_RAG_PACK_ROOT before any retrieval happens."
        ),
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-pack-root",
        required=True,
        type=Path,
        help="Absolute, read-only path to the source eCFR pack",
    )
    parser.add_argument(
        "--candidate-pack-root",
        required=True,
        type=Path,
        help="Absolute path that must not yet exist; created by this script",
    )
    parser.add_argument(
        "--report-out",
        required=True,
        type=Path,
        help="Absolute path for the T10 validation JSON report",
    )
    parser.add_argument(
        "--embedding-backend",
        default=DEFAULT_EMBEDDING_BACKEND,
        help=(
            "Embedding backend identity to inject into the candidate "
            f"(default: {DEFAULT_EMBEDDING_BACKEND})"
        ),
    )
    parser.add_argument(
        "--embedding-precision",
        default=DEFAULT_EMBEDDING_PRECISION,
        help=(
            "Embedding precision identity to inject into the candidate "
            f"(default: {DEFAULT_EMBEDDING_PRECISION})"
        ),
    )
    args = parser.parse_args(argv)

    if not args.source_pack_root.is_absolute():
        parser.error("--source-pack-root must be absolute")
    if not args.candidate_pack_root.is_absolute():
        parser.error("--candidate-pack-root must be absolute")
    if not args.report_out.is_absolute():
        parser.error("--report-out must be absolute")
    if not args.source_pack_root.is_dir():
        parser.error(f"source pack root does not exist: {args.source_pack_root}")
    if args.candidate_pack_root.exists():
        parser.error(
            f"candidate pack root already exists: {args.candidate_pack_root}"
        )
    args.report_out.parent.mkdir(parents=True, exist_ok=True)

    report: dict[str, object] = {
        "schema_version": 1,
        "generated_at_utc": _now_iso(),
        "source_pack_root": str(args.source_pack_root),
        "candidate_pack_root": str(args.candidate_pack_root),
        "stages": [],
    }

    def _stage(name: str, payload: dict[str, object]) -> None:
        report["stages"].append({"stage": name, **payload})

    _stage(
        "verify_source_pack",
        {
            "status": "verified" if report["stages"] else "started",
            "manifest": None,
        },
    )
    source_manifest = verify_complete_pack(args.source_pack_root)
    report["stages"][-1]["manifest"] = {
        "pack_id": source_manifest.pack_id,
        "snapshot_date": source_manifest.snapshot_date,
        "document_count": source_manifest.document_count,
        "chunk_count": source_manifest.chunk_count,
        "embedding_model": source_manifest.embedding_model,
        "embedding_revision": source_manifest.embedding_revision,
        "embedding_dimensions": source_manifest.embedding_dimensions,
        "relevance_dense_min": source_manifest.relevance_dense_min,
        "complete": source_manifest.complete,
    }

    args.candidate_pack_root.mkdir(parents=True, exist_ok=False)
    try:
        copied = _copy_small_controls(args.source_pack_root, args.candidate_pack_root)
        _copy_index(args.source_pack_root, args.candidate_pack_root)
        _stage(
            "copy_small_controls_and_index",
            {"copied_files": copied},
        )
        upgrade = _upgrade_index(
            args.candidate_pack_root / "index",
            backend=args.embedding_backend,
            precision=args.embedding_precision,
        )
        _stage("upgrade_index_metadata", upgrade)
        synced = _sync_pack_manifest_index_records(
            args.candidate_pack_root,
            matrix_sha=upgrade["matrix_sha256"],
            database_sha=upgrade["database_sha256"],
            matrix_bytes=(args.candidate_pack_root / "index" / "vectors.f16").stat().st_size,
            database_bytes=(args.candidate_pack_root / "index" / "metadata.sqlite3").stat().st_size,
            vectors_json_bytes=(args.candidate_pack_root / "index" / "vectors.json").stat().st_size,
            vectors_json_sha=_sha256_file(
                args.candidate_pack_root / "index" / "vectors.json"
            ),
        )
        _stage("sync_pack_manifest", synced)
        focused = _focused_verification(
            args.candidate_pack_root,
            backend=args.embedding_backend,
            precision=args.embedding_precision,
            matrix_sha=upgrade["matrix_sha256"],
            source_pack_root=args.source_pack_root,
        )
        _stage("focused_verification", focused)
        inventory = _candidate_files_report(args.candidate_pack_root)
        _stage("candidate_inventory", inventory)
        if focused["status"] != "verified":
            raise BuildError(
                "focused verification reported failures: "
                + "; ".join(focused["failures"])
            )
        report["status"] = "candidate_data_surface_built"
        report["caveats"] = [
            (
                "The candidate root contains the upgraded index and the "
                "small control files only.  It does not yet include the "
                "embedded Python runtime, wheels, or the model payload; "
                "those must be staged by cmake/stage_ready_package.cmake "
                "before a Ready EXE can consume this candidate."
            ),
            (
                "verify_complete_pack on the candidate is skipped because "
                "the runtime payload is not yet present.  The upgraded "
                "index passes the inline schema 3 + covering index + "
                "backend identity contract."
            ),
        ]
        report["rollback_plan"] = _rollback_plan(
            args.source_pack_root, args.candidate_pack_root
        )
    except Exception as error:
        report["status"] = "candidate_build_failed"
        report["error"] = repr(error)
        args.report_out.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        return 2

    args.report_out.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    sys.stdout.write(
        json.dumps(
            {
                "wrote": str(args.report_out),
                "candidate_pack_root": str(args.candidate_pack_root),
                "status": report["status"],
            },
            ensure_ascii=False,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))