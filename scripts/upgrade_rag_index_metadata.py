"""Upgrade a published schema 2 RAG index to schema 3 in place.

The schema 3 metadata publishes the exact ``row_count`` and
``sum_token_count`` of the chunk table and creates the covering index
``chunks_vector_row_cover`` so the retriever does not need an
``AVG(token_count)`` full-table scan at startup.  This script reads the
existing schema 2 artifacts, verifies that the vectors are unchanged, and
rewrites only the SQLite metadata keys plus the covering index, then bumps
``vectors.json`` to schema 3.  It does not re-encode vectors and does not
mutate document/chunk content hashes.  Changing the embedding backend must
be done by T6 and is intentionally out of scope here.

Usage:

    python scripts/upgrade_rag_index_metadata.py <pack-root>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile

# Ensure the in-repo agent_rag package is importable when this script is
# run as ``python scripts/upgrade_rag_index_metadata.py`` from the repo root.
_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from rag.agent_rag.hybrid_index import (  # type: ignore[import-not-found]
    CHUNKS_VECTOR_ROW_COVER,
    HybridIndexError,
    INDEX_SCHEMA_VERSION,
    INDEX_SCHEMA_VERSION_LEGACY,
    _sha256_file,
)


class UpgradeError(RuntimeError):
    pass


def _read_legacy_vector_metadata(index_root: Path) -> dict[str, object]:
    path = index_root / "vectors.json"
    if not path.is_file():
        raise UpgradeError("vectors.json is unavailable")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise UpgradeError("vectors.json could not be read") from error
    if (
        type(value) is not dict
        or value.get("schema_version") != INDEX_SCHEMA_VERSION_LEGACY
    ):
        raise UpgradeError("vectors.json is not on the legacy schema")
    return value


def _compute_statistics(database: Path) -> tuple[int, int]:
    """Return (row_count, sum_token_count) by streaming the chunk table."""
    connection = sqlite3.connect(f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True)
    try:
        connection.execute("PRAGMA query_only=ON")
        if connection.execute("PRAGMA quick_check").fetchone() != ("ok",):
            raise UpgradeError("source database failed quick_check")
        schema_version = connection.execute(
            "SELECT value FROM metadata WHERE key = 'schema_version'"
        ).fetchone()
        if schema_version is None or schema_version[0] != str(INDEX_SCHEMA_VERSION_LEGACY):
            raise UpgradeError("source database is not on the legacy schema")
        rows = connection.execute("SELECT token_count FROM chunks").fetchall()
        if not rows:
            raise UpgradeError("source database has no chunks")
        sum_token_count = 0
        for (token_count,) in rows:
            if type(token_count) is not int or token_count <= 0:
                raise UpgradeError("token_count is invalid")
            sum_token_count += token_count
        row_count = len(rows)
    finally:
        connection.close()
    return row_count, sum_token_count


def _verify_contiguous_mapping(database: Path, expected_row_count: int) -> None:
    connection = sqlite3.connect(f"file:{database.as_posix()}?mode=ro&immutable=1", uri=True)
    try:
        connection.execute("PRAGMA query_only=ON")
        vector_map = [
            int(vector_row)
            for (vector_row,) in connection.execute(
                "SELECT vector_row FROM chunks ORDER BY vector_row"
            )
        ]
        if len(vector_map) != expected_row_count:
            raise UpgradeError("vector_row mapping row count is invalid")
        if vector_map != list(range(expected_row_count)):
            raise UpgradeError("vector_row mapping is not contiguous")
    finally:
        connection.close()


def _copy_with_schema_upgrade(
    source_database: Path,
    destination_database: Path,
    *,
    row_count: int,
    sum_token_count: int,
) -> None:
    """Copy the legacy database, bump schema_version, and add covering index."""
    shutil.copyfile(source_database, destination_database)
    connection = sqlite3.connect(destination_database)
    try:
        connection.execute("PRAGMA foreign_keys=ON")
        connection.execute(
            "UPDATE metadata SET value = ? WHERE key = 'schema_version'",
            (str(INDEX_SCHEMA_VERSION),),
        )
        existing = connection.execute(
            "SELECT 1 FROM metadata WHERE key = 'row_count'"
        ).fetchone()
        if existing is None:
            connection.execute(
                "INSERT INTO metadata VALUES(?, ?)",
                ("row_count", str(row_count)),
            )
        else:
            connection.execute(
                "UPDATE metadata SET value = ? WHERE key = 'row_count'",
                (str(row_count),),
            )
        existing = connection.execute(
            "SELECT 1 FROM metadata WHERE key = 'sum_token_count'"
        ).fetchone()
        if existing is None:
            connection.execute(
                "INSERT INTO metadata VALUES(?, ?)",
                ("sum_token_count", str(sum_token_count)),
            )
        else:
            connection.execute(
                "UPDATE metadata SET value = ? WHERE key = 'sum_token_count'",
                (str(sum_token_count),),
            )
        connection.execute(
            "CREATE INDEX IF NOT EXISTS chunks_vector_row_cover "
            "ON chunks(vector_row, chunk_id)"
        )
        connection.commit()
        if connection.execute("PRAGMA foreign_key_check").fetchall():
            raise UpgradeError("upgraded database has broken references")
        if connection.execute("PRAGMA integrity_check").fetchone() != ("ok",):
            raise UpgradeError("upgraded database failed integrity check")
    finally:
        connection.close()


def upgrade_pack(pack_root: Path) -> dict[str, object]:
    if not pack_root.is_absolute() or not pack_root.is_dir():
        raise UpgradeError("pack root must be an absolute directory")
    index_root = pack_root / "index"
    database = index_root / "metadata.sqlite3"
    vectors_metadata = index_root / "vectors.json"
    matrix_path = index_root / "vectors.f16"
    if not (database.is_file() and matrix_path.is_file() and vectors_metadata.is_file()):
        raise UpgradeError("index artifacts are missing")
    legacy = _read_legacy_vector_metadata(index_root)
    matrix_sha256 = _sha256_file(matrix_path)
    database_sha256 = _sha256_file(database)
    if legacy.get("matrix_sha256") != matrix_sha256:
        raise UpgradeError("matrix sha256 is invalid")
    if legacy.get("database_sha256") != database_sha256:
        raise UpgradeError("database sha256 is invalid")
    row_count, sum_token_count = _compute_statistics(database)
    _verify_contiguous_mapping(database, row_count)
    legacy_rows = legacy.get("rows")
    if type(legacy_rows) is not int or legacy_rows != row_count:
        raise UpgradeError("legacy rows metadata does not match database")

    staging_dir = Path(tempfile.mkdtemp(prefix=".upgrade-", dir=index_root.parent))
    try:
        staging_database = staging_dir / "metadata.sqlite3"
        staging_vectors_metadata = staging_dir / "vectors.json"
        _copy_with_schema_upgrade(
            database,
            staging_database,
            row_count=row_count,
            sum_token_count=sum_token_count,
        )
        if _sha256_file(staging_database) == database_sha256:
            raise UpgradeError("database content did not change during upgrade")
        new_database_sha256 = _sha256_file(staging_database)
        new_metadata = dict(legacy)
        new_metadata["schema_version"] = INDEX_SCHEMA_VERSION
        new_metadata["row_count"] = row_count
        new_metadata["sum_token_count"] = sum_token_count
        new_metadata["database_sha256"] = new_database_sha256
        staging_vectors_metadata.write_text(
            json.dumps(new_metadata, sort_keys=True, separators=(",", ":")),
            encoding="utf-8",
            newline="",
        )
        backup_database = database.with_name(database.name + ".legacy-backup")
        backup_vectors_metadata = vectors_metadata.with_name(
            vectors_metadata.name + ".legacy-backup"
        )
        shutil.move(str(database), str(backup_database))
        shutil.move(str(vectors_metadata), str(backup_vectors_metadata))
        try:
            shutil.move(str(staging_database), str(database))
            shutil.move(str(staging_vectors_metadata), str(vectors_metadata))
        except Exception:
            shutil.move(str(backup_database), str(database))
            shutil.move(str(backup_vectors_metadata), str(vectors_metadata))
            raise
    finally:
        shutil.rmtree(staging_dir, ignore_errors=True)
    return {
        "schema_version": INDEX_SCHEMA_VERSION,
        "row_count": row_count,
        "sum_token_count": sum_token_count,
        "covering_index": CHUNKS_VECTOR_ROW_COVER,
        "vectors_unchanged": True,
        "document_hashes_unchanged": True,
    }


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Upgrade a schema 2 RAG index to schema 3.")
    parser.add_argument("pack_root", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.dry_run:
            legacy = _read_legacy_vector_metadata(args.pack_root / "index")
            row_count, sum_token_count = _compute_statistics(
                args.pack_root / "index" / "metadata.sqlite3"
            )
            payload = {
                "dry_run": True,
                "current_schema_version": legacy.get("schema_version"),
                "row_count": row_count,
                "sum_token_count": sum_token_count,
                "covering_index": CHUNKS_VECTOR_ROW_COVER,
                "target_schema_version": INDEX_SCHEMA_VERSION,
            }
        else:
            payload = upgrade_pack(args.pack_root)
    except (UpgradeError, HybridIndexError) as error:
        print(f"upgrade failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(payload, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
