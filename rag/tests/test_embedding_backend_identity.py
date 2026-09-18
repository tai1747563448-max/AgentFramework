"""Tests for the T6 embedding backend identity contract.

The six-tuple ``(model, revision, backend, precision, dimensions,
tokenizer_sha256)`` is what gates reusable index vectors, the
retrieval_revision hash seed, and per-task evidence caches.  Changing
any component must therefore change the hash digest, and a backend
whose attributes are inconsistent with the canonical BGE-M3 contract
must be rejected up-front so the rest of the pipeline cannot accidentally
mix vector spaces.
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
import sys

import numpy as np
import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))


from agent_rag.embedding import (  # type: ignore[import-not-found]
    BACKEND_REMOTE_HTTP,
    BACKEND_SENTENCE_TRANSFORMERS,
    BGE_M3_MODEL,
    BGE_M3_REVISION,
    EMBEDDING_DIMENSIONS,
    PRECISION_FLOAT32,
    PRECISION_INT8,
    SUPPORTED_BACKENDS,
    SUPPORTED_PRECISIONS,
    EmbeddingBackend,
    EmbeddingError,
    backend_identity,
    backend_identity_digest,
    backend_identity_string,
)
from agent_rag.pack import (  # type: ignore[import-not-found]
    DEFAULT_EMBEDDING_BACKEND,
    DEFAULT_EMBEDDING_PRECISION,
    FileDigest,
    PACK_SCHEMA_VERSION,
    PackManifest,
    retrieval_revision,
)


PACK_ID = "pack-0123456789abcdef0123456789abcdef"


class _StaticTokenizer:
    def get_vocab(self) -> dict[str, int]:
        return {"alpha": 1, "beta": 2}


@dataclasses.dataclass
class _FakeBackend:
    model: str = BGE_M3_MODEL
    revision: str = BGE_M3_REVISION
    dimensions: int = EMBEDDING_DIMENSIONS
    backend: str = BACKEND_SENTENCE_TRANSFORMERS
    precision: str = PRECISION_FLOAT32
    tokenizer: object = dataclasses.field(default_factory=_StaticTokenizer)

    def encode(self, texts, *, batch_size: int = 16) -> np.ndarray:  # pragma: no cover
        return np.zeros((len(list(texts)), self.dimensions), dtype=np.float32)


def _identity(**overrides) -> tuple[str, str, str, str, int, str]:
    backend = _FakeBackend(**overrides)
    return backend_identity(backend, tokenizer=backend.tokenizer)


def test_default_supported_backend_and_precision_sets_are_stable() -> None:
    """The baseline values are part of the public contract used by
    ``retrieval_revision`` and the cache key — re-naming or removing a
    default must be a deliberate bump that ships with rebuilt vectors.
    """
    assert BACKEND_SENTENCE_TRANSFORMERS in SUPPORTED_BACKENDS
    assert PRECISION_FLOAT32 in SUPPORTED_PRECISIONS
    assert DEFAULT_EMBEDDING_BACKEND == BACKEND_SENTENCE_TRANSFORMERS
    assert DEFAULT_EMBEDDING_PRECISION == PRECISION_FLOAT32


def test_backend_identity_string_matches_canonical_format() -> None:
    identity = _identity()
    canonical = backend_identity_string(identity)
    assert canonical == (
        f"{BGE_M3_MODEL}@{BGE_M3_REVISION}+"
        f"{BACKEND_SENTENCE_TRANSFORMERS}/{PRECISION_FLOAT32}/{EMBEDDING_DIMENSIONS}"
    )


def test_backend_identity_digest_changes_when_any_field_changes() -> None:
    baseline = backend_identity_digest(_identity())
    assert backend_identity_digest(_identity(revision="other")) != baseline
    assert (
        backend_identity_digest(_identity(backend=BACKEND_REMOTE_HTTP))
        != baseline
    )
    assert backend_identity_digest(_identity(precision=PRECISION_INT8)) != baseline
    assert backend_identity_digest(_identity(dimensions=512)) != baseline


def test_backend_identity_rejects_unknown_backend() -> None:
    with pytest.raises(EmbeddingError, match="backend"):
        backend_identity(_FakeBackend(backend="onnx_runtime_ghost"))


def test_backend_identity_rejects_unknown_precision() -> None:
    with pytest.raises(EmbeddingError, match="precision"):
        backend_identity(_FakeBackend(precision="bf16"))


def test_backend_identity_rejects_missing_tokenizer() -> None:
    backend = _FakeBackend(tokenizer=None)
    with pytest.raises(EmbeddingError, match="tokenizer"):
        backend_identity(backend, tokenizer=None)


def test_pack_manifest_default_backend_and_precision_match_baseline() -> None:
    manifest = PackManifest(
        PACK_SCHEMA_VERSION,
        PACK_ID,
        "2026-09-03",
        30_000,
        45_000,
        BGE_M3_MODEL,
        BGE_M3_REVISION,
        EMBEDDING_DIMENSIONS,
        0.61,
        True,
        tuple(
            FileDigest(
                path,
                1,
                digest * 64,
            )
            for path, digest in {
                "build.intent.json": "a",
                "runtime.lock.json": "b",
                "eval/ecfr-cases.jsonl": "c",
                "reports/retrieval-eval.json": "d",
            }.items()
        ),
    )
    assert manifest.embedding_backend == DEFAULT_EMBEDDING_BACKEND
    assert manifest.embedding_precision == DEFAULT_EMBEDDING_PRECISION


def test_retrieval_revision_changes_when_backend_or_precision_changes() -> None:
    base_files = tuple(
        FileDigest(
            path,
            1,
            digest * 64,
        )
        for path, digest in {
            "build.intent.json": "a",
            "runtime.lock.json": "b",
            "eval/ecfr-cases.jsonl": "c",
            "reports/retrieval-eval.json": "d",
        }.items()
    )
    baseline = PackManifest(
        PACK_SCHEMA_VERSION, PACK_ID, "2026-09-03", 30_000, 45_000,
        BGE_M3_MODEL, BGE_M3_REVISION, EMBEDDING_DIMENSIONS, 0.61, True,
        base_files,
    )
    base_revision = retrieval_revision(baseline)

    alt_backend = dataclasses.replace(baseline, embedding_backend=BACKEND_REMOTE_HTTP)
    alt_precision = dataclasses.replace(baseline, embedding_precision=PRECISION_INT8)
    assert retrieval_revision(alt_backend) != base_revision
    assert retrieval_revision(alt_precision) != base_revision


def test_backend_identity_accepts_provided_tokenizer_argument() -> None:
    backend = _FakeBackend()
    identity_via_arg = backend_identity(backend, tokenizer=backend.tokenizer)
    identity_via_default = backend_identity(backend)
    assert identity_via_arg == identity_via_default