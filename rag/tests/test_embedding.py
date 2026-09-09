from __future__ import annotations

from pathlib import Path
import sys

import numpy as np
import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.embedding import (  # type: ignore[import-not-found]
    BGE_M3_MODEL,
    BgeM3Embedding,
    EmbeddingError,
    encode_normalized,
    normalize_rows,
    tokenizer_fingerprint,
)
from agent_rag.cli import select_embedding_device  # type: ignore[import-not-found]


class FixedBackend:
    model = BGE_M3_MODEL
    revision = "fixed-revision"
    dimensions = 2

    def __init__(self, rows: np.ndarray) -> None:
        self.rows = rows

    def encode(self, texts: list[str], *, batch_size: int = 16) -> np.ndarray:
        del texts, batch_size
        return self.rows.copy()


class FixedTokenizer:
    def get_vocab(self) -> dict[str, int]:
        return {"beta": 2, "alpha": 1}

    def encode(self, text: str) -> list[int]:
        return [len(part) for part in text.split()]

    def decode(self, tokens: list[int]) -> str:
        return " ".join("x" * token for token in tokens)


def test_embedding_backend_rejects_wrong_dimensions() -> None:
    backend = FixedBackend(np.ones((1, 3), dtype=np.float32))

    with pytest.raises(EmbeddingError, match="expected 2 dimensions"):
        encode_normalized(backend, ["query"], dimensions=2)


def test_normalization_produces_unit_float32_before_f16_storage() -> None:
    result = normalize_rows(np.array([[3.0, 4.0]], dtype=np.float64))

    assert result.dtype == np.float32
    np.testing.assert_allclose(np.linalg.norm(result, axis=1), [1.0])


@pytest.mark.parametrize(
    "rows, message",
    [
        (np.empty((0, 2), dtype=np.float32), "row count"),
        (np.array([[np.nan, 1.0]], dtype=np.float32), "nonfinite"),
        (np.zeros((1, 2), dtype=np.float32), "zero"),
    ],
)
def test_encode_rejects_empty_nonfinite_and_zero_vectors(
    rows: np.ndarray, message: str
) -> None:
    backend = FixedBackend(rows)

    with pytest.raises(EmbeddingError, match=message):
        encode_normalized(backend, ["query"], dimensions=2)


def test_encode_rejects_row_count_mismatch() -> None:
    backend = FixedBackend(np.ones((2, 2), dtype=np.float32))

    with pytest.raises(EmbeddingError, match="row count"):
        encode_normalized(backend, ["only one"], dimensions=2)


def test_tokenizer_fingerprint_is_stable_and_vocab_order_independent() -> None:
    first = tokenizer_fingerprint(FixedTokenizer())

    class ReverseTokenizer(FixedTokenizer):
        def get_vocab(self) -> dict[str, int]:
            return {"alpha": 1, "beta": 2}

    assert first == tokenizer_fingerprint(ReverseTokenizer())
    assert len(first) == 64


def test_bge_adapter_fails_closed_when_model_lock_revision_is_wrong(
    tmp_path: Path,
) -> None:
    model_root = tmp_path / "model"
    model_root.mkdir()
    (tmp_path / "model.lock.json").write_text(
        '{"schema_version":2,"model":"BAAI/bge-m3","revision":"wrong",'
        '"dimensions":1024,"files":[]}',
        encoding="utf-8",
    )

    with pytest.raises(EmbeddingError, match="model lock"):
        BgeM3Embedding(model_root, expected_revision="expected", device="cpu")


def test_auto_device_falls_back_to_cpu_when_cuda_self_test_fails() -> None:
    class FakeCuda:
        @staticmethod
        def is_available() -> bool:
            return True

    class FakeTorch:
        cuda = FakeCuda()

        @staticmethod
        def empty(*args: object, **kwargs: object) -> None:
            del args, kwargs
            raise RuntimeError("allocation failed")

    assert select_embedding_device("auto", torch_module=FakeTorch()) == "cpu"


def test_explicit_cuda_fails_when_cuda_is_unavailable() -> None:
    class FakeCuda:
        @staticmethod
        def is_available() -> bool:
            return False

    class FakeTorch:
        cuda = FakeCuda()

    with pytest.raises(EmbeddingError, match="CUDA"):
        select_embedding_device("cuda", torch_module=FakeTorch())
