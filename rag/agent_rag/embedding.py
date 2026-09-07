from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
from typing import Any, Protocol, Sequence

import numpy as np

from .pack import BGE_M3_MODEL, BGE_M3_REVISION, EMBEDDING_DIMENSIONS


_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class EmbeddingError(RuntimeError):
    pass


class EmbeddingBackend(Protocol):
    model: str
    revision: str
    dimensions: int

    def encode(
        self, texts: Sequence[str], *, batch_size: int = 16
    ) -> np.ndarray: ...


def select_embedding_device(
    requested: str, *, torch_module: object | None = None
) -> str:
    if requested not in {"auto", "cpu", "cuda"}:
        raise EmbeddingError("embedding device is invalid")
    if requested == "cpu":
        return "cpu"
    try:
        if torch_module is None:
            import torch as loaded_torch

            torch_module = loaded_torch
        cuda = getattr(torch_module, "cuda")
        if not cuda.is_available():
            if requested == "cuda":
                raise EmbeddingError("CUDA is unavailable")
            return "cpu"
        getattr(torch_module, "empty")((1,), device="cuda")
        return "cuda"
    except EmbeddingError:
        raise
    except Exception as error:
        if requested == "cuda":
            raise EmbeddingError("CUDA self-test failed") from error
        return "cpu"


def normalize_rows(values: np.ndarray) -> np.ndarray:
    try:
        matrix = np.asarray(values, dtype=np.float32)
    except (TypeError, ValueError, OverflowError) as error:
        raise EmbeddingError("embedding matrix is invalid") from error
    if matrix.ndim != 2:
        raise EmbeddingError("embedding matrix must have two dimensions")
    if matrix.shape[0] == 0:
        raise EmbeddingError("embedding row count is invalid")
    if not np.isfinite(matrix).all():
        raise EmbeddingError("embedding contains nonfinite values")
    norms = np.linalg.norm(matrix, axis=1, keepdims=True)
    if not np.isfinite(norms).all() or np.any(norms <= 0.0):
        raise EmbeddingError("embedding contains a zero vector")
    normalized = matrix / norms
    if not np.isfinite(normalized).all():
        raise EmbeddingError("normalized embedding contains nonfinite values")
    return np.ascontiguousarray(normalized, dtype=np.float32)


def encode_normalized(
    backend: EmbeddingBackend,
    texts: Sequence[str],
    *,
    dimensions: int,
    batch_size: int = 16,
) -> np.ndarray:
    if (
        type(dimensions) is not int
        or dimensions <= 0
        or type(batch_size) is not int
        or batch_size <= 0
        or not texts
        or any(type(text) is not str or not text for text in texts)
    ):
        raise EmbeddingError("embedding request is invalid")
    try:
        raw = backend.encode(list(texts), batch_size=batch_size)
    except EmbeddingError:
        raise
    except Exception as error:
        raise EmbeddingError("embedding failed") from error
    try:
        matrix = np.asarray(raw)
    except (TypeError, ValueError, OverflowError) as error:
        raise EmbeddingError("embedding matrix is invalid") from error
    if matrix.ndim != 2 or matrix.shape[0] != len(texts):
        raise EmbeddingError("embedding row count is invalid")
    if matrix.shape[1] != dimensions:
        raise EmbeddingError(f"expected {dimensions} dimensions")
    return normalize_rows(matrix)


def tokenizer_fingerprint(tokenizer: Any) -> str:
    try:
        vocabulary = tokenizer.get_vocab()
        if type(vocabulary) is not dict or not vocabulary:
            raise ValueError("empty vocabulary")
        items = []
        for token, identifier in vocabulary.items():
            if type(token) is not str or type(identifier) is not int:
                raise ValueError("invalid vocabulary")
            items.append((token, identifier))
        payload = json.dumps(
            sorted(items, key=lambda item: (item[0], item[1])),
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except Exception as error:
        raise EmbeddingError("tokenizer fingerprint failed") from error
    return hashlib.sha256(payload).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError as error:
        raise EmbeddingError("model file could not be read") from error
    return digest.hexdigest()


def _find_model_lock(model_root: Path) -> tuple[Path, Path]:
    for pack_root in (model_root.parent, *model_root.parents):
        candidate = pack_root / "model.lock.json"
        if candidate.is_file():
            return candidate, pack_root
    raise EmbeddingError("model lock is unavailable")


def _relative_model_path(value: object) -> PurePosixPath:
    if type(value) is not str or not value or "\\" in value or ":" in value:
        raise EmbeddingError("model lock is invalid")
    relative = PurePosixPath(value)
    if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
        raise EmbeddingError("model lock is invalid")
    return relative


def _verify_model_lock(
    model_root: Path, *, expected_revision: str, expected_dimensions: int
) -> dict[str, object]:
    lock_path, pack_root = _find_model_lock(model_root)
    try:
        if lock_path.stat().st_size > 16 * 1024 * 1024:
            raise EmbeddingError("model lock is invalid")
        value = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError, TypeError) as error:
        raise EmbeddingError("model lock is invalid") from error
    if (
        type(value) is not dict
        or set(value) != {"schema_version", "model", "revision", "dimensions", "files"}
        or type(value["schema_version"]) is not int
        or value["schema_version"] != 2
        or value["model"] != BGE_M3_MODEL
        or value["revision"] != expected_revision
        or type(value["dimensions"]) is not int
        or value["dimensions"] != expected_dimensions
        or type(value["files"]) is not list
    ):
        raise EmbeddingError("model lock is invalid")
    seen: set[str] = set()
    for record in value["files"]:
        if type(record) is not dict or set(record) != {"path", "bytes", "sha256"}:
            raise EmbeddingError("model lock is invalid")
        relative = _relative_model_path(record["path"])
        path_text = relative.as_posix()
        size = record["bytes"]
        digest = record["sha256"]
        if (
            path_text in seen
            or type(size) is not int
            or size < 0
            or type(digest) is not str
            or _SHA256.fullmatch(digest) is None
        ):
            raise EmbeddingError("model lock is invalid")
        seen.add(path_text)
        path = pack_root.joinpath(*relative.parts)
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(model_root.resolve(strict=True))
            if not resolved.is_file() or resolved.stat().st_size != size:
                raise EmbeddingError("model lock is invalid")
        except (OSError, ValueError) as error:
            raise EmbeddingError("model lock is invalid") from error
        if _sha256_file(resolved) != digest:
            raise EmbeddingError("model lock is invalid")
    return value


class BgeM3Embedding:
    model = BGE_M3_MODEL

    def __init__(
        self,
        model_root: Path,
        *,
        expected_revision: str = BGE_M3_REVISION,
        device: str,
        dimensions: int = EMBEDDING_DIMENSIONS,
    ) -> None:
        root = Path(model_root)
        if (
            not root.is_absolute()
            or not root.is_dir()
            or device not in {"cpu", "cuda"}
            or type(dimensions) is not int
            or dimensions <= 0
            or type(expected_revision) is not str
            or not expected_revision
        ):
            raise EmbeddingError("embedding configuration is invalid")
        _verify_model_lock(
            root,
            expected_revision=expected_revision,
            expected_dimensions=dimensions,
        )
        try:
            from sentence_transformers import SentenceTransformer

            implementation = SentenceTransformer(
                str(root),
                device=device,
                local_files_only=True,
                trust_remote_code=False,
            )
        except Exception as error:
            raise EmbeddingError("local embedding model could not be loaded") from error
        implementation.max_seq_length = 768
        self.revision = expected_revision
        self.dimensions = dimensions
        self.device = device
        self._implementation = implementation
        self.tokenizer = implementation.tokenizer

    def encode(
        self, texts: Sequence[str], *, batch_size: int = 16
    ) -> np.ndarray:
        try:
            values = self._implementation.encode(
                list(texts),
                batch_size=batch_size,
                normalize_embeddings=True,
                convert_to_numpy=True,
                show_progress_bar=False,
            )
        except Exception as error:
            raise EmbeddingError("embedding failed") from error
        return encode_normalized(
            _PrecomputedEmbedding(values),
            list(texts),
            dimensions=self.dimensions,
            batch_size=batch_size,
        )


class _PrecomputedEmbedding:
    def __init__(self, values: np.ndarray) -> None:
        self._values = values

    def encode(self, texts: Sequence[str], *, batch_size: int = 16) -> np.ndarray:
        del texts, batch_size
        return self._values
