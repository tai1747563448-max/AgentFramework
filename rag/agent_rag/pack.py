from __future__ import annotations

from dataclasses import dataclass
import datetime as _datetime
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
from typing import Any, Callable, Iterable


BGE_M3_MODEL = "BAAI/bge-m3"
BGE_M3_REVISION = "5617a9f61b028005a4858fdac845db406aefb181"
PACK_SCHEMA_VERSION = 2
INDEX_LEGACY_SCHEMA_VERSION = 2
INDEX_SCHEMA_VERSION = 3
INDEX_SCHEMA_VERSION_BACKEND = 4
EMBEDDING_DIMENSIONS = 1024

# T6 embedding backend identity.  ``pack.json`` does not yet publish these
# fields so the C++ verifier can stay on its strict key set; the dataclass
# defaults below match the only currently supported backend so the
# retrieval_revision hash seed and the index metadata remain consistent
# without a manifest schema bump.
DEFAULT_EMBEDDING_BACKEND = "sentence_transformers"
DEFAULT_EMBEDDING_PRECISION = "float32"

_PACK_KEYS = {
    "schema_version",
    "pack_id",
    "snapshot_date",
    "document_count",
    "chunk_count",
    "embedding_model",
    "embedding_revision",
    "embedding_dimensions",
    "relevance_dense_min",
    "complete",
    "files",
}
_FILE_KEYS = {"path", "bytes", "sha256"}
_LEGACY_VECTOR_KEYS = {
    "schema_version",
    "dtype",
    "rows",
    "dimensions",
    "model",
    "revision",
    "tokenizer_sha256",
    "matrix_sha256",
    "database_sha256",
}
_VECTOR_KEYS = _LEGACY_VECTOR_KEYS | {
    "row_count",
    "sum_token_count",
}
_MODEL_LOCK_KEYS = {
    "schema_version",
    "model",
    "revision",
    "dimensions",
    "files",
}
_DOCUMENT_KEYS = {
    "schema_version",
    "document_id",
    "citation",
    "title_number",
    "title_name",
    "chapter",
    "chapter_name",
    "subchapter",
    "subchapter_name",
    "part",
    "part_name",
    "section",
    "section_title",
    "snapshot_date",
    "source_xml_url",
    "official_url",
    "source_xml_sha256",
    "path",
    "body_sha256",
    "markdown_sha256",
    "body_bytes",
    "retrieved_utc",
    "legal_status",
}
_REQUIRED_FILES = {
    "manifest/documents.jsonl",
    "index/metadata.sqlite3",
    "index/vectors.f16",
    "index/vectors.json",
    "model.lock.json",
}
_RUNTIME_REQUIRED_FILES = _REQUIRED_FILES | {
    "runtime/python.exe",
    "runtime.lock.json",
    "sidecar/agent_rag_cli.py",
}
_RUNTIME_HASHED_CONTROL_FILES = {
    "index/vectors.json",
    "model.lock.json",
    "runtime.lock.json",
    "sidecar/agent_rag_cli.py",
}
_PACK_ID = re.compile(r"pack-[0-9a-f]{32}\Z")
_DOCUMENT_ID = re.compile(r"doc-[0-9a-f]{32}\Z")
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class PackError(RuntimeError):
    pass


@dataclass(frozen=True)
class FileDigest:
    path: str
    bytes: int
    sha256: str


@dataclass(frozen=True)
class PackManifest:
    schema_version: int
    pack_id: str
    snapshot_date: str
    document_count: int
    chunk_count: int
    embedding_model: str
    embedding_revision: str
    embedding_dimensions: int
    relevance_dense_min: float
    complete: bool
    files: tuple[FileDigest, ...]
    embedding_backend: str = DEFAULT_EMBEDDING_BACKEND
    embedding_precision: str = DEFAULT_EMBEDDING_PRECISION


def _reject_constant(_: str) -> None:
    raise ValueError("nonfinite number")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate key")
        value[key] = item
    return value


def _parse_json_bytes(data: bytes, *, maximum: int, label: str) -> Any:
    if not data or len(data) > maximum or b"\x00" in data:
        raise PackError(f"{label} is invalid")
    try:
        text = data.decode("utf-8", errors="strict")
        return json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeError, ValueError, TypeError, json.JSONDecodeError) as error:
        raise PackError(f"{label} is invalid") from error


def _is_link_or_reparse(path: Path) -> bool:
    try:
        status = os.lstat(path)
    except OSError as error:
        raise PackError("pack path inspection failed") from error
    if stat.S_ISLNK(status.st_mode):
        return True
    attributes = getattr(status, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse)


def _require_unlinked_components(path: Path) -> None:
    absolute = path.absolute()
    current = Path(absolute.anchor)
    for component in absolute.parts[1:]:
        current /= component
        if not current.exists() and not current.is_symlink():
            continue
        if _is_link_or_reparse(current):
            raise PackError("pack path is linked")


def require_trusted_directory(path: Path) -> Path:
    supplied = Path(path)
    if not supplied.is_absolute():
        raise PackError("pack root must be absolute")
    _require_unlinked_components(supplied)
    try:
        root = supplied.resolve(strict=True)
    except OSError as error:
        raise PackError("pack root is unavailable") from error
    if not root.is_dir():
        raise PackError("pack root is unavailable")
    return root


def _relative_path(text: Any) -> str:
    if type(text) is not str or not text or len(text) > 512:
        raise PackError("pack manifest is invalid")
    if "\\" in text or ":" in text or "\x00" in text:
        raise PackError("pack manifest is invalid")
    path = PurePosixPath(text)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise PackError("pack manifest is invalid")
    return path.as_posix()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise PackError("pack file could not be read") from error
    return digest.hexdigest()


def _require_regular_single_link(path: Path) -> None:
    _require_unlinked_components(path)
    try:
        status = os.stat(path, follow_symlinks=False)
    except OSError as error:
        raise PackError("pack file is unavailable") from error
    if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
        raise PackError("pack file is not a regular single-link file")


def _read_trusted(path: Path, *, maximum: int, label: str) -> bytes:
    _require_regular_single_link(path)
    try:
        if path.stat().st_size > maximum:
            raise PackError(f"{label} is invalid")
        return path.read_bytes()
    except OSError as error:
        raise PackError(f"{label} is invalid") from error


def _positive_int(value: Any) -> bool:
    return type(value) is int and value > 0


def _parse_file_digests(value: Any) -> tuple[FileDigest, ...]:
    if type(value) is not list or not value:
        raise PackError("pack manifest is invalid")
    records: list[FileDigest] = []
    seen: set[str] = set()
    for item in value:
        if type(item) is not dict or set(item) != _FILE_KEYS:
            raise PackError("pack manifest is invalid")
        path = _relative_path(item["path"])
        size = item["bytes"]
        digest = item["sha256"]
        if (
            path in seen
            or type(size) is not int
            or size < 0
            or type(digest) is not str
            or _SHA256.fullmatch(digest) is None
        ):
            raise PackError("pack manifest is invalid")
        seen.add(path)
        records.append(FileDigest(path, size, digest))
    if not _REQUIRED_FILES.issubset(seen):
        raise PackError("pack manifest is invalid")
    return tuple(records)


def _parse_pack_manifest(value: Any) -> PackManifest:
    if type(value) is not dict or set(value) != _PACK_KEYS:
        raise PackError("pack manifest is invalid")
    score = value["relevance_dense_min"]
    if (
        type(value["schema_version"]) is not int
        or value["schema_version"] != PACK_SCHEMA_VERSION
        or type(value["pack_id"]) is not str
        or _PACK_ID.fullmatch(value["pack_id"]) is None
        or not _positive_int(value["document_count"])
        or not _positive_int(value["chunk_count"])
        or value["embedding_model"] != BGE_M3_MODEL
        or value["embedding_revision"] != BGE_M3_REVISION
        or value["embedding_dimensions"] != EMBEDDING_DIMENSIONS
        or type(value["embedding_dimensions"]) is not int
        or type(score) not in {int, float}
        or not math.isfinite(float(score))
        or not -1.0 <= float(score) <= 1.0
        or type(value["complete"]) is not bool
    ):
        raise PackError("pack manifest is invalid")
    try:
        parsed_date = _datetime.date.fromisoformat(value["snapshot_date"])
    except (TypeError, ValueError) as error:
        raise PackError("pack manifest is invalid") from error
    if parsed_date.isoformat() != value["snapshot_date"]:
        raise PackError("pack manifest is invalid")
    return PackManifest(
        value["schema_version"],
        value["pack_id"],
        value["snapshot_date"],
        value["document_count"],
        value["chunk_count"],
        value["embedding_model"],
        value["embedding_revision"],
        value["embedding_dimensions"],
        float(score),
        value["complete"],
        _parse_file_digests(value["files"]),
    )


def _inventory(root: Path) -> tuple[set[str], set[str]]:
    files: set[str] = set()
    directories: set[str] = set()
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError as error:
            raise PackError("pack inventory is invalid") from error
        for entry in entries:
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            if entry.is_symlink() or _is_link_or_reparse(path):
                raise PackError("pack inventory is invalid")
            if entry.is_dir(follow_symlinks=False):
                directories.add(relative)
                stack.append(path)
            elif entry.is_file(follow_symlinks=False):
                try:
                    if os.stat(path, follow_symlinks=False).st_nlink != 1:
                        raise PackError("pack inventory is invalid")
                except OSError as error:
                    raise PackError("pack inventory is invalid") from error
                files.add(relative)
            else:
                raise PackError("pack inventory is invalid")
    return files, directories


def _expected_directories(files: Iterable[str]) -> set[str]:
    expected: set[str] = set()
    for name in files:
        parent = PurePosixPath(name).parent
        while parent != PurePosixPath("."):
            expected.add(parent.as_posix())
            parent = parent.parent
    return expected


def _verify_file_records(
    root: Path,
    records: tuple[FileDigest, ...],
    progress: Callable[..., None] | None = None,
) -> None:
    if progress is not None:
        progress("verify-pack-files", 0, len(records))
    expected_files = {"pack.json", *(record.path for record in records)}
    files, directories = _inventory(root)
    if files != expected_files or directories != _expected_directories(expected_files):
        raise PackError("pack inventory is invalid")
    for completed, record in enumerate(records, start=1):
        path = root.joinpath(*PurePosixPath(record.path).parts)
        _require_regular_single_link(path)
        try:
            size = path.stat().st_size
        except OSError as error:
            raise PackError("pack file is unavailable") from error
        if size != record.bytes or _sha256_file(path) != record.sha256:
            raise PackError("pack file digest is invalid")
        if progress is not None:
            progress("verify-pack-files", completed, len(records))


def _canonical_date(value: Any) -> bool:
    if type(value) is not str:
        return False
    try:
        return _datetime.date.fromisoformat(value).isoformat() == value
    except ValueError:
        return False


def _count_documents(
    path: Path,
    root: Path,
    *,
    expected_count: int,
    progress: Callable[..., None] | None = None,
) -> int:
    data = _read_trusted(path, maximum=512 * 1024 * 1024, label="document manifest")
    count = 0
    seen: set[str] = set()
    if progress is not None:
        progress("verify-pack-documents", 0, expected_count)
    for raw_line in data.splitlines():
        if not raw_line:
            raise PackError("document manifest is invalid")
        value = _parse_json_bytes(raw_line, maximum=64 * 1024, label="document manifest")
        if type(value) is not dict or set(value) != _DOCUMENT_KEYS:
            raise PackError("document manifest is invalid")
        document_id = value["document_id"]
        if (
            value["schema_version"] != PACK_SCHEMA_VERSION
            or type(value["schema_version"]) is not int
            or type(document_id) is not str
            or _DOCUMENT_ID.fullmatch(document_id) is None
            or document_id in seen
            or type(value["citation"]) is not str
            or not value["citation"]
            or type(value["title_number"]) is not int
            or not 1 <= value["title_number"] <= 50
            or any(
                type(value[name]) is not str
                for name in (
                    "title_name",
                    "chapter",
                    "chapter_name",
                    "subchapter",
                    "subchapter_name",
                    "part",
                    "part_name",
                    "section",
                    "section_title",
                    "snapshot_date",
                    "source_xml_url",
                    "official_url",
                    "source_xml_sha256",
                    "retrieved_utc",
                    "legal_status",
                )
            )
            or not value["title_name"]
            or not value["section"]
            or not _canonical_date(value["snapshot_date"])
            or not value["source_xml_url"].startswith("https://www.ecfr.gov/")
            or not value["official_url"].startswith("https://www.ecfr.gov/")
            or _SHA256.fullmatch(value["source_xml_sha256"]) is None
            or _relative_path(value["path"]) != value["path"]
            or type(value["body_sha256"]) is not str
            or _SHA256.fullmatch(value["body_sha256"]) is None
            or type(value["markdown_sha256"]) is not str
            or _SHA256.fullmatch(value["markdown_sha256"]) is None
            or type(value["body_bytes"]) is not int
            or value["body_bytes"] <= 0
            or not value["retrieved_utc"].endswith("Z")
            or not value["legal_status"]
        ):
            raise PackError("document manifest is invalid")
        markdown_path = root.joinpath(*PurePosixPath(value["path"]).parts)
        _require_regular_single_link(markdown_path)
        if _sha256_file(markdown_path) != value["markdown_sha256"]:
            raise PackError("document markdown digest is invalid")
        seen.add(document_id)
        count += 1
        if progress is not None:
            progress("verify-pack-documents", count, expected_count)
    return count


def _verify_model_lock(root: Path, manifest: PackManifest) -> None:
    value = _parse_json_bytes(
        _read_trusted(root / "model.lock.json", maximum=16 * 1024 * 1024, label="model lock"),
        maximum=16 * 1024 * 1024,
        label="model lock",
    )
    if (
        type(value) is not dict
        or set(value) != _MODEL_LOCK_KEYS
        or value["schema_version"] != PACK_SCHEMA_VERSION
        or value["model"] != manifest.embedding_model
        or value["revision"] != manifest.embedding_revision
        or value["dimensions"] != manifest.embedding_dimensions
        or type(value["files"]) is not list
    ):
        raise PackError("model lock is invalid")


def _verify_vectors(
    root: Path,
    manifest: PackManifest,
    progress: Callable[..., None] | None = None,
) -> None:
    value = _parse_json_bytes(
        _read_trusted(root / "index" / "vectors.json", maximum=64 * 1024, label="vector metadata"),
        maximum=64 * 1024,
        label="vector metadata",
    )
    schema_version = value.get("schema_version") if type(value) is dict else None
    if schema_version == INDEX_SCHEMA_VERSION:
        expected_keys = _VECTOR_KEYS
    elif schema_version == INDEX_LEGACY_SCHEMA_VERSION:
        expected_keys = _LEGACY_VECTOR_KEYS
    else:
        raise PackError("vector metadata is invalid")
    if type(value) is not dict or set(value) != expected_keys:
        raise PackError("vector metadata is invalid")
    if (
        value["dtype"] != "<f2"
        or type(value["rows"]) is not int
        or value["rows"] != manifest.chunk_count
        or type(value["dimensions"]) is not int
        or value["dimensions"] != manifest.embedding_dimensions
        or value["model"] != manifest.embedding_model
        or value["revision"] != manifest.embedding_revision
    ):
        raise PackError("pack counts are inconsistent")
    if schema_version == INDEX_SCHEMA_VERSION:
        if (
            type(value.get("row_count")) is not int
            or value["row_count"] != manifest.chunk_count
            or type(value.get("sum_token_count")) is not int
            or value["sum_token_count"] <= 0
        ):
            raise PackError("vector metadata is invalid")
    if (
        type(value["tokenizer_sha256"]) is not str
        or _SHA256.fullmatch(value["tokenizer_sha256"]) is None
    ):
        raise PackError("vector metadata is invalid")
    vectors = root / "index" / "vectors.f16"
    database = root / "index" / "metadata.sqlite3"
    if progress is not None:
        progress("verify-pack-vectors", 0, 2)
    try:
        vector_size = vectors.stat().st_size
    except OSError as error:
        raise PackError("vector matrix is unavailable") from error
    matrix_sha256 = _sha256_file(vectors)
    if progress is not None:
        progress("verify-pack-vectors", 1, 2)
    database_sha256 = _sha256_file(database)
    if progress is not None:
        progress("verify-pack-vectors", 2, 2)
    if (
        vector_size != manifest.chunk_count * manifest.embedding_dimensions * 2
        or value["matrix_sha256"] != matrix_sha256
        or value["database_sha256"] != database_sha256
    ):
        raise PackError("pack counts are inconsistent")


def verify_runtime_pack(
    root: Path, *, progress: Callable[..., None] | None = None
) -> PackManifest:
    """Validate the bounded startup surface of a previously published pack.

    Full inventory and digest validation remains mandatory in
    ``verify_complete_pack`` and ``atomic_publish``. Startup validates the
    manifest contract, every directly consumed root asset's identity and size,
    and hashes the small control files. Large model and index payloads are
    subsequently opened and structurally checked by their owning adapters.
    """
    trusted_root = require_trusted_directory(Path(root))
    value = _parse_json_bytes(
        _read_trusted(
            trusted_root / "pack.json",
            maximum=64 * 1024 * 1024,
            label="pack manifest",
        ),
        maximum=64 * 1024 * 1024,
        label="pack manifest",
    )
    manifest = _parse_pack_manifest(value)
    if not manifest.complete:
        raise PackError("pack is incomplete")
    records = {record.path: record for record in manifest.files}
    if not _RUNTIME_REQUIRED_FILES.issubset(records):
        raise PackError("pack manifest is invalid")
    ordered = sorted(_RUNTIME_REQUIRED_FILES)
    if progress is not None:
        progress("runtime-pack-assets", 0, len(ordered))
    for completed, name in enumerate(ordered, start=1):
        record = records[name]
        path = trusted_root.joinpath(*PurePosixPath(name).parts)
        _require_regular_single_link(path)
        try:
            actual_size = path.stat().st_size
        except OSError as error:
            raise PackError("pack file is unavailable") from error
        if actual_size != record.bytes:
            raise PackError("pack file digest is invalid")
        if name in _RUNTIME_HASHED_CONTROL_FILES and _sha256_file(path) != record.sha256:
            raise PackError("pack file digest is invalid")
        if progress is not None:
            progress("runtime-pack-assets", completed, len(ordered))

    _verify_model_lock(trusted_root, manifest)
    vector_value = _parse_json_bytes(
        _read_trusted(
            trusted_root / "index" / "vectors.json",
            maximum=64 * 1024,
            label="vector metadata",
        ),
        maximum=64 * 1024,
        label="vector metadata",
    )
    schema_version = vector_value.get("schema_version") if type(vector_value) is dict else None
    if schema_version == INDEX_SCHEMA_VERSION:
        expected_vector_keys = _VECTOR_KEYS
    elif schema_version == INDEX_LEGACY_SCHEMA_VERSION:
        expected_vector_keys = _LEGACY_VECTOR_KEYS
    else:
        raise PackError("vector metadata is invalid")
    if type(vector_value) is not dict or set(vector_value) != expected_vector_keys:
        raise PackError("vector metadata is invalid")
    vectors = records["index/vectors.f16"]
    database = records["index/metadata.sqlite3"]
    if (
        vector_value["dtype"] != "<f2"
        or vector_value["rows"] != manifest.chunk_count
        or vector_value["dimensions"] != manifest.embedding_dimensions
        or vector_value["model"] != manifest.embedding_model
        or vector_value["revision"] != manifest.embedding_revision
        or vector_value["matrix_sha256"] != vectors.sha256
        or vector_value["database_sha256"] != database.sha256
        or vectors.bytes
        != manifest.chunk_count * manifest.embedding_dimensions * 2
        or database.bytes <= 0
        or type(vector_value["tokenizer_sha256"]) is not str
        or _SHA256.fullmatch(vector_value["tokenizer_sha256"]) is None
    ):
        raise PackError("pack counts are inconsistent")
    if schema_version == INDEX_SCHEMA_VERSION:
        if (
            type(vector_value["row_count"]) is not int
            or vector_value["row_count"] != manifest.chunk_count
            or type(vector_value["sum_token_count"]) is not int
            or vector_value["sum_token_count"] <= 0
        ):
            raise PackError("vector metadata is invalid")
    return manifest


def retrieval_revision(manifest: PackManifest) -> str:
    """Return the immutable retrieval/runtime policy identity for a pack.

    ``pack_id`` deliberately identifies knowledge content.  This separate
    revision binds the executable runtime, sidecar build, fitted relevance
    policy, and frozen evaluation inputs/results used with that content.
    The seed also embeds the embedding backend identity (T6) so swapping
    ``sentence_transformers``/``float32`` for an evaluated alternative
    invalidates every cache key and per-task evidence derived from the
    previous revision.
    """
    records = {record.path: record for record in manifest.files}
    required = (
        "build.intent.json",
        "runtime.lock.json",
        "eval/ecfr-cases.jsonl",
        "reports/retrieval-eval.json",
    )
    if any(name not in records for name in required):
        raise PackError("retrieval revision inputs are unavailable")
    seed = {
        "schema_version": PACK_SCHEMA_VERSION,
        "pack_id": manifest.pack_id,
        "relevance_dense_min": manifest.relevance_dense_min,
        "build_intent_sha256": records["build.intent.json"].sha256,
        "runtime_lock_sha256": records["runtime.lock.json"].sha256,
        "evaluation_cases_sha256": records["eval/ecfr-cases.jsonl"].sha256,
        "evaluation_report_sha256": records[
            "reports/retrieval-eval.json"
        ].sha256,
        "embedding_backend": manifest.embedding_backend,
        "embedding_precision": manifest.embedding_precision,
    }
    try:
        encoded = json.dumps(
            seed,
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise PackError("retrieval revision inputs are invalid") from error
    return "retrieval-" + hashlib.sha256(encoded).hexdigest()


def verify_complete_pack(
    root: Path, *, progress: Callable[..., None] | None = None
) -> PackManifest:
    trusted_root = require_trusted_directory(Path(root))
    manifest_path = trusted_root / "pack.json"
    value = _parse_json_bytes(
        _read_trusted(manifest_path, maximum=64 * 1024 * 1024, label="pack manifest"),
        maximum=64 * 1024 * 1024,
        label="pack manifest",
    )
    manifest = _parse_pack_manifest(value)
    if not manifest.complete:
        raise PackError("pack is incomplete")
    _verify_file_records(trusted_root, manifest.files, progress)
    if (
        _count_documents(
            trusted_root / "manifest" / "documents.jsonl",
            trusted_root,
            expected_count=manifest.document_count,
            progress=progress,
        )
        != manifest.document_count
    ):
        raise PackError("pack counts are inconsistent")
    _verify_model_lock(trusted_root, manifest)
    _verify_vectors(trusted_root, manifest, progress)
    return manifest


def atomic_publish(
    staging: Path,
    destination: Path,
    *,
    progress: Callable[..., None] | None = None,
) -> None:
    staging_root = require_trusted_directory(Path(staging))
    verify_complete_pack(staging_root, progress=progress)
    destination_path = Path(destination)
    if not destination_path.is_absolute():
        raise PackError("publish destination must be absolute")
    _require_unlinked_components(destination_path.parent)
    destination_parent = destination_path.parent.resolve(strict=True)
    staging_parent = staging_root.parent
    direct_sibling = staging_parent == destination_parent
    hidden_staging_child = (
        staging_parent.name.casefold() == ".staging"
        and staging_parent.parent == destination_parent
    )
    if not (direct_sibling or hidden_staging_child):
        raise PackError("publish paths must share one parent")
    if destination_path.exists() or destination_path.is_symlink():
        raise PackError("publish destination already exists")
    try:
        os.replace(staging_root, destination_path)
    except OSError as error:
        raise PackError("pack publication failed") from error
