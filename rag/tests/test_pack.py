from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))


from agent_rag.pack import (  # type: ignore[import-not-found]
    BGE_M3_MODEL,
    BGE_M3_REVISION,
    PackError,
    atomic_publish,
    verify_complete_pack,
)


PACK_ID = "pack-0123456789abcdef0123456789abcdef"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def _minimal_pack(
    root: Path,
    *,
    document_count: int = 1,
    chunk_count: int = 2,
    vector_rows: int = 2,
    complete: bool = True,
) -> Path:
    root.mkdir(parents=True)
    document = {
        "schema_version": 2,
        "document_id": "doc-0123456789abcdef0123456789abcdef",
        "citation": "1 CFR 1.1",
        "path": "corpus/title-001/part-1/section-1.1--0123456789ab.md",
        "body_sha256": "a" * 64,
        "markdown_sha256": "b" * 64,
    }
    documents = b"".join(
        (
            json.dumps(document, sort_keys=True, separators=(",", ":")).encode("utf-8")
            + b"\n"
        )
        for _ in range(document_count)
    )
    _write(root / "manifest" / "documents.jsonl", documents)
    _write(root / "index" / "metadata.sqlite3", b"sqlite-fixture")
    vector_bytes = b"\x00\x00" * vector_rows * 1024
    _write(root / "index" / "vectors.f16", vector_bytes)
    vector_metadata = {
        "schema_version": 2,
        "dtype": "<f2",
        "rows": vector_rows,
        "dimensions": 1024,
        "model": BGE_M3_MODEL,
        "revision": BGE_M3_REVISION,
        "matrix_sha256": _sha256(root / "index" / "vectors.f16"),
        "database_sha256": _sha256(root / "index" / "metadata.sqlite3"),
    }
    _write(
        root / "index" / "vectors.json",
        json.dumps(vector_metadata, sort_keys=True, separators=(",", ":")).encode("utf-8"),
    )
    model_lock = {
        "schema_version": 2,
        "model": BGE_M3_MODEL,
        "revision": BGE_M3_REVISION,
        "dimensions": 1024,
        "files": [],
    }
    _write(
        root / "model.lock.json",
        json.dumps(model_lock, sort_keys=True, separators=(",", ":")).encode("utf-8"),
    )
    files = []
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        files.append(
            {
                "path": path.relative_to(root).as_posix(),
                "bytes": path.stat().st_size,
                "sha256": _sha256(path),
            }
        )
    manifest = {
        "schema_version": 2,
        "pack_id": PACK_ID,
        "snapshot_date": "2026-09-03",
        "document_count": document_count,
        "chunk_count": chunk_count,
        "embedding_model": BGE_M3_MODEL,
        "embedding_revision": BGE_M3_REVISION,
        "embedding_dimensions": 1024,
        "relevance_dense_min": 0.0,
        "complete": complete,
        "files": files,
    }
    _write(
        root / "pack.json",
        json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode("utf-8"),
    )
    return root


def test_verify_complete_pack_accepts_consistent_fixture(tmp_path: Path) -> None:
    manifest = verify_complete_pack(_minimal_pack(tmp_path / "pack"))

    assert manifest.pack_id == PACK_ID
    assert manifest.document_count == 1
    assert manifest.chunk_count == 2
    assert manifest.embedding_dimensions == 1024


def test_verify_pack_rejects_chunk_vector_count_mismatch(tmp_path: Path) -> None:
    root = _minimal_pack(
        tmp_path / "pack", document_count=1, chunk_count=2, vector_rows=1
    )

    with pytest.raises(PackError, match="pack counts are inconsistent"):
        verify_complete_pack(root)


def test_verify_pack_rejects_duplicate_manifest_keys(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack")
    text = (root / "pack.json").read_text(encoding="utf-8")
    (root / "pack.json").write_text(
        text.replace('{"chunk_count":2,', '{"chunk_count":2,"chunk_count":2,'),
        encoding="utf-8",
        newline="",
    )

    with pytest.raises(PackError, match="pack manifest is invalid"):
        verify_complete_pack(root)


def test_atomic_publish_preserves_existing_complete_pack_on_failure(
    tmp_path: Path,
) -> None:
    destination = _minimal_pack(tmp_path / "published")
    staging = _minimal_pack(tmp_path / "staging", complete=False)

    with pytest.raises(PackError, match="pack is incomplete"):
        atomic_publish(staging, destination)

    assert verify_complete_pack(destination).pack_id == PACK_ID
    assert staging.exists()


def test_verify_pack_rejects_unlisted_and_linked_files(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack")
    _write(root / "unexpected.txt", b"not in manifest")
    with pytest.raises(PackError, match="pack inventory is invalid"):
        verify_complete_pack(root)

    (root / "unexpected.txt").unlink()
    source = root / "model.lock.json"
    linked = root / "linked-model.lock.json"
    try:
        os.link(source, linked)
    except OSError as error:
        pytest.skip(f"hard links unavailable: {error}")
    with pytest.raises(PackError, match="pack inventory is invalid"):
        verify_complete_pack(root)
