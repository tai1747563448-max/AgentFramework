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
    retrieval_revision,
    verify_complete_pack,
    verify_runtime_pack,
)
from agent_rag.cli import main as rag_main  # type: ignore[import-not-found]


PACK_ID = "pack-0123456789abcdef0123456789abcdef"


def test_retrieval_revision_binds_runtime_policy_and_evaluation_identity() -> None:
    from agent_rag.pack import FileDigest, PackManifest

    paths = {
        "build.intent.json": "a",
        "runtime.lock.json": "b",
        "eval/ecfr-cases.jsonl": "c",
        "reports/retrieval-eval.json": "d",
    }

    def manifest(dense_min: float, report_digest: str = "d") -> PackManifest:
        records = tuple(
            FileDigest(path, 1, (report_digest if path.startswith("reports/") else digest) * 64)
            for path, digest in paths.items()
        )
        return PackManifest(
            2, PACK_ID, "2026-09-03", 30_000, 45_000,
            BGE_M3_MODEL, BGE_M3_REVISION, 1024, dense_min, True, records
        )

    baseline = retrieval_revision(manifest(0.61))
    assert baseline.startswith("retrieval-")
    assert len(baseline) == 74
    assert retrieval_revision(manifest(0.62)) != baseline
    assert retrieval_revision(manifest(0.61, "e")) != baseline


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
    runtime_assets: bool = False,
) -> Path:
    root.mkdir(parents=True)
    document_path = "corpus/title-001/part-1/section-1.1--0123456789ab.md"
    markdown = b"---\ncitation: 1 CFR 1.1\n---\n\n# 1 CFR 1.1\n\nbody\n"
    _write(root / document_path, markdown)
    document = {
        "schema_version": 2,
        "document_id": "doc-0123456789abcdef0123456789abcdef",
        "citation": "1 CFR 1.1",
        "title_number": 1,
        "title_name": "General Provisions",
        "chapter": "I",
        "chapter_name": "Administrative Committee",
        "subchapter": "A",
        "subchapter_name": "General",
        "part": "1",
        "part_name": "Definitions",
        "section": "1.1",
        "section_title": "Definitions",
        "snapshot_date": "2026-09-03",
        "source_xml_url": "https://www.ecfr.gov/api/versioner/v1/full/2026-09-03/title-1.xml",
        "official_url": "https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1",
        "source_xml_sha256": "c" * 64,
        "path": document_path,
        "body_sha256": "a" * 64,
        "markdown_sha256": hashlib.sha256(markdown).hexdigest(),
        "body_bytes": 4,
        "retrieved_utc": "2026-09-07T00:00:00Z",
        "legal_status": "Official eCFR snapshot; informational retrieval only; not legal advice.",
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
        "tokenizer_sha256": "e" * 64,
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
    if runtime_assets:
        _write(root / "runtime" / "python.exe", b"runtime-fixture")
        _write(root / "runtime.lock.json", b"runtime-lock-fixture")
        _write(root / "sidecar" / "agent_rag_cli.py", b"sidecar-fixture")
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


def test_runtime_pack_uses_bounded_startup_validation_and_reports_progress(
    tmp_path: Path,
) -> None:
    root = _minimal_pack(tmp_path / "pack", runtime_assets=True)
    document = next((root / "corpus").rglob("*.md"))
    document.write_bytes(document.read_bytes().replace(b"body", b"BODY"))
    events: list[tuple[str, int, int]] = []

    manifest = verify_runtime_pack(
        root,
        progress=lambda phase, completed, total, **_: events.append(
            (phase, completed, total)
        ),
    )

    assert manifest.pack_id == PACK_ID
    assert events[0][0] == "runtime-pack-assets"
    assert events[-1][1] == events[-1][2]
    with pytest.raises(PackError, match="pack file digest"):
        verify_complete_pack(root)


def test_runtime_pack_rejects_resized_retrieval_asset(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack", runtime_assets=True)
    with (root / "index" / "vectors.f16").open("ab") as stream:
        stream.write(b"extra")

    with pytest.raises(PackError, match="pack file digest"):
        verify_runtime_pack(root)


def test_verify_complete_pack_accepts_consistent_fixture(tmp_path: Path) -> None:
    events: list[tuple[str, int, int]] = []
    manifest = verify_complete_pack(
        _minimal_pack(tmp_path / "pack"),
        progress=lambda phase, completed, total, **_: events.append(
            (phase, completed, total)
        ),
    )

    assert manifest.pack_id == PACK_ID
    assert manifest.document_count == 1
    assert manifest.chunk_count == 2
    assert manifest.embedding_dimensions == 1024
    assert events[0] == ("verify-pack-files", 0, 6)
    assert events[6] == ("verify-pack-files", 6, 6)
    assert [event for event in events if event[0] == "verify-pack-vectors"] == [
        ("verify-pack-vectors", 0, 2),
        ("verify-pack-vectors", 1, 2),
        ("verify-pack-vectors", 2, 2),
    ]


def test_verify_pack_cli_reports_only_stable_manifest_metadata(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    root = _minimal_pack(tmp_path / "pack")

    assert rag_main(["verify-pack", "--pack-root", str(root)]) == 0

    result = json.loads(capsys.readouterr().out)
    assert result == {
        "chunk_count": 2,
        "document_count": 1,
        "embedding_dimensions": 1024,
        "pack_id": PACK_ID,
        "schema_version": 2,
        "snapshot_date": "2026-09-03",
    }


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


def test_atomic_publish_reports_verification_progress(tmp_path: Path) -> None:
    staging = _minimal_pack(tmp_path / "staging")
    destination = tmp_path / "published"
    events: list[tuple[str, int, int]] = []

    atomic_publish(
        staging,
        destination,
        progress=lambda phase, completed, total, **_: events.append(
            (phase, completed, total)
        ),
    )

    assert destination.is_dir()
    assert not staging.exists()
    assert events[0][0] == "verify-pack-files"
    assert events[-1] == ("verify-pack-vectors", 2, 2)


def test_atomic_publish_accepts_exact_hidden_staging_parent(tmp_path: Path) -> None:
    staging_parent = tmp_path / ".staging"
    staging_parent.mkdir()
    staging = _minimal_pack(staging_parent / "pack-build")
    destination = tmp_path / "published"

    atomic_publish(staging, destination)

    assert destination.is_dir()
    assert not staging.exists()


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


def test_verify_pack_rejects_manifest_markdown_digest_mismatch(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack")
    document_path = root / "corpus" / "title-001" / "part-1" / "section-1.1--0123456789ab.md"
    document_path.write_text("changed but pack digest will be refreshed", encoding="utf-8")
    manifest = json.loads((root / "pack.json").read_text("utf-8"))
    for record in manifest["files"]:
        if record["path"].endswith(".md"):
            record["bytes"] = document_path.stat().st_size
            record["sha256"] = _sha256(document_path)
    (root / "pack.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )

    with pytest.raises(PackError, match="document markdown digest is invalid"):
        verify_complete_pack(root)


def test_verify_pack_rejects_invalid_tokenizer_fingerprint(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack")
    vectors_path = root / "index" / "vectors.json"
    vectors = json.loads(vectors_path.read_text("utf-8"))
    vectors["tokenizer_sha256"] = "not-a-digest"
    vectors_path.write_text(
        json.dumps(vectors, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    manifest = json.loads((root / "pack.json").read_text("utf-8"))
    for record in manifest["files"]:
        if record["path"] == "index/vectors.json":
            record["bytes"] = vectors_path.stat().st_size
            record["sha256"] = _sha256(vectors_path)
    (root / "pack.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )

    with pytest.raises(PackError, match="vector metadata is invalid"):
        verify_complete_pack(root)


def _upgrade_pack_to_schema3(root: Path, *, chunk_count: int) -> dict[str, object]:
    vectors_path = root / "index" / "vectors.json"
    payload = json.loads(vectors_path.read_text("utf-8"))
    payload["schema_version"] = 3
    payload["row_count"] = chunk_count
    payload["sum_token_count"] = chunk_count * 5
    payload["database_sha256"] = _sha256(root / "index" / "metadata.sqlite3")
    payload["matrix_sha256"] = _sha256(root / "index" / "vectors.f16")
    vectors_path.write_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    manifest = json.loads((root / "pack.json").read_text("utf-8"))
    for record in manifest["files"]:
        if record["path"] == "index/vectors.json":
            record["bytes"] = vectors_path.stat().st_size
            record["sha256"] = _sha256(vectors_path)
    (root / "pack.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    return payload


def test_verify_pack_accepts_schema3_vector_metadata(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack", chunk_count=2)
    _upgrade_pack_to_schema3(root, chunk_count=2)

    manifest = verify_complete_pack(root)

    assert manifest.chunk_count == 2
    vectors = json.loads((root / "index" / "vectors.json").read_text("utf-8"))
    assert vectors["schema_version"] == 3
    assert vectors["row_count"] == 2
    assert vectors["sum_token_count"] == 10


def test_verify_pack_rejects_schema3_with_missing_row_count(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack", chunk_count=2)
    vectors_path = root / "index" / "vectors.json"
    payload = json.loads(vectors_path.read_text("utf-8"))
    payload["schema_version"] = 3
    payload["row_count"] = 2
    payload["sum_token_count"] = 10
    payload["database_sha256"] = _sha256(root / "index" / "metadata.sqlite3")
    payload["matrix_sha256"] = _sha256(root / "index" / "vectors.f16")
    del payload["row_count"]
    vectors_path.write_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    manifest = json.loads((root / "pack.json").read_text("utf-8"))
    for record in manifest["files"]:
        if record["path"] == "index/vectors.json":
            record["bytes"] = vectors_path.stat().st_size
            record["sha256"] = _sha256(vectors_path)
    (root / "pack.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )

    with pytest.raises(PackError, match="vector metadata is invalid"):
        verify_complete_pack(root)


def test_verify_pack_rejects_schema3_with_inconsistent_row_count(tmp_path: Path) -> None:
    root = _minimal_pack(tmp_path / "pack", chunk_count=2)
    vectors_path = root / "index" / "vectors.json"
    payload = json.loads(vectors_path.read_text("utf-8"))
    payload["schema_version"] = 3
    payload["row_count"] = 2
    payload["sum_token_count"] = 10
    payload["database_sha256"] = _sha256(root / "index" / "metadata.sqlite3")
    payload["matrix_sha256"] = _sha256(root / "index" / "vectors.f16")
    payload["row_count"] = 99
    vectors_path.write_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )
    manifest = json.loads((root / "pack.json").read_text("utf-8"))
    for record in manifest["files"]:
        if record["path"] == "index/vectors.json":
            record["bytes"] = vectors_path.stat().st_size
            record["sha256"] = _sha256(vectors_path)
    (root / "pack.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
        newline="",
    )

    with pytest.raises(PackError, match="vector metadata is invalid"):
        verify_complete_pack(root)


def test_verify_runtime_pack_accepts_legacy_schema2(tmp_path: Path) -> None:
    """The runtime validator must accept legacy schema 2 packs; the
    retriever then runs its legacy AVG fallback.  Old packs are not
    auto-upgraded."""
    root = _minimal_pack(tmp_path / "pack", chunk_count=2, runtime_assets=True)
    vectors_path = root / "index" / "vectors.json"
    payload = json.loads(vectors_path.read_text("utf-8"))
    assert payload["schema_version"] == 2
    assert "row_count" not in payload
    assert "sum_token_count" not in payload

    manifest = verify_runtime_pack(root)

    assert manifest.chunk_count == 2

