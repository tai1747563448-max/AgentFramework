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
from typing import Any, Iterable


BGE_M3_MODEL = "BAAI/bge-m3"
BGE_M3_REVISION = "5617a9f61b028005a4858fdac845db406aefb181"
PACK_SCHEMA_VERSION = 2
EMBEDDING_DIMENSIONS = 1024

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
_VECTOR_KEYS = {
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


def _verify_file_records(root: Path, records: tuple[FileDigest, ...]) -> None:
    expected_files = {"pack.json", *(record.path for record in records)}
    files, directories = _inventory(root)
    if files != expected_files or directories != _expected_directories(expected_files):
        raise PackError("pack inventory is invalid")
    for record in records:
        path = root.joinpath(*PurePosixPath(record.path).parts)
        _require_regular_single_link(path)
        try:
            size = path.stat().st_size
        except OSError as error:
            raise PackError("pack file is unavailable") from error
        if size != record.bytes or _sha256_file(path) != record.sha256:
            raise PackError("pack file digest is invalid")


def _canonical_date(value: Any) -> bool:
    if type(value) is not str:
        return False
    try:
        return _datetime.date.fromisoformat(value).isoformat() == value
    except ValueError:
        return False


def _count_documents(path: Path, root: Path) -> int:
    data = _read_trusted(path, maximum=512 * 1024 * 1024, label="document manifest")
    count = 0
    seen: set[str] = set()
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


def _verify_vectors(root: Path, manifest: PackManifest) -> None:
    value = _parse_json_bytes(
        _read_trusted(root / "index" / "vectors.json", maximum=64 * 1024, label="vector metadata"),
        maximum=64 * 1024,
        label="vector metadata",
    )
    if type(value) is not dict or set(value) != _VECTOR_KEYS:
        raise PackError("vector metadata is invalid")
    if (
        value["schema_version"] != PACK_SCHEMA_VERSION
        or value["dtype"] != "<f2"
        or type(value["rows"]) is not int
        or value["rows"] != manifest.chunk_count
        or type(value["dimensions"]) is not int
        or value["dimensions"] != manifest.embedding_dimensions
        or value["model"] != manifest.embedding_model
        or value["revision"] != manifest.embedding_revision
    ):
        raise PackError("pack counts are inconsistent")
    if (
        type(value["tokenizer_sha256"]) is not str
        or _SHA256.fullmatch(value["tokenizer_sha256"]) is None
    ):
        raise PackError("vector metadata is invalid")
    vectors = root / "index" / "vectors.f16"
    database = root / "index" / "metadata.sqlite3"
    try:
        vector_size = vectors.stat().st_size
    except OSError as error:
        raise PackError("vector matrix is unavailable") from error
    if (
        vector_size != manifest.chunk_count * manifest.embedding_dimensions * 2
        or value["matrix_sha256"] != _sha256_file(vectors)
        or value["database_sha256"] != _sha256_file(database)
    ):
        raise PackError("pack counts are inconsistent")


def verify_complete_pack(root: Path) -> PackManifest:
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
    _verify_file_records(trusted_root, manifest.files)
    if (
        _count_documents(
            trusted_root / "manifest" / "documents.jsonl", trusted_root
        )
        != manifest.document_count
    ):
        raise PackError("pack counts are inconsistent")
    _verify_model_lock(trusted_root, manifest)
    _verify_vectors(trusted_root, manifest)
    return manifest


def atomic_publish(staging: Path, destination: Path) -> None:
    staging_root = require_trusted_directory(Path(staging))
    verify_complete_pack(staging_root)
    destination_path = Path(destination)
    if not destination_path.is_absolute():
        raise PackError("publish destination must be absolute")
    if staging_root.parent != destination_path.parent.resolve(strict=True):
        raise PackError("publish paths must share one parent")
    if destination_path.exists() or destination_path.is_symlink():
        raise PackError("publish destination already exists")
    try:
        os.replace(staging_root, destination_path)
    except OSError as error:
        raise PackError("pack publication failed") from error
