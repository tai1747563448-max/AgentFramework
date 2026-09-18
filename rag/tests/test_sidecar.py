from __future__ import annotations

from dataclasses import dataclass
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.hybrid_retriever import (  # type: ignore[import-not-found]
    EvidenceItem,
    RetrievalResult,
)
from agent_rag import sidecar as sidecar_module  # type: ignore[import-not-found]
from agent_rag.protocol import (  # type: ignore[import-not-found]
    ProtocolError,
    parse_v2_message,
    parse_v3_message,
)
from agent_rag.sidecar import (  # type: ignore[import-not-found]
    LoadedRuntime,
    run_sidecar,
)


REQUEST_ONE = "req-11111111111111111111111111111111"
REQUEST_TWO = "req-22222222222222222222222222222222"
REQUEST_THREE = "req-33333333333333333333333333333333"


def test_runtime_loader_uses_bounded_pack_check_and_skips_duplicate_quick_check(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls: dict[str, object] = {}
    manifest = SimpleNamespace(
        pack_id="pack-" + "c" * 32,
        snapshot_date="2026-09-03",
        document_count=30_000,
        chunk_count=45_000,
        embedding_model="BAAI/bge-m3",
        embedding_revision="d" * 40,
        embedding_dimensions=1024,
        relevance_dense_min=0.6,
    )

    def verify(root: Path, *, progress: object) -> object:
        calls["verified_root"] = root
        calls["verify_progress"] = progress
        return manifest

    class FakeEmbedding:
        pass

    class FakeRetriever:
        def __init__(self, root: Path, **kwargs: object) -> None:
            calls["retriever_root"] = root
            calls.update(kwargs)

    monkeypatch.setattr(sidecar_module, "verify_runtime_pack", verify)
    monkeypatch.setattr(
        sidecar_module, "retrieval_revision", lambda _: "retrieval-" + "e" * 64
    )
    monkeypatch.setattr(sidecar_module, "select_embedding_device", lambda _: "cpu")
    monkeypatch.setattr(sidecar_module, "BgeM3Embedding", lambda *_, **__: FakeEmbedding())
    monkeypatch.setattr(sidecar_module, "encode_normalized", lambda *_, **__: None)
    monkeypatch.setattr(sidecar_module, "HybridRetriever", FakeRetriever)
    events: list[tuple[str, int, int]] = []

    loaded = sidecar_module._load_runtime(
        tmp_path.resolve(),
        device="cpu",
        progress=lambda phase, completed, total, **_: events.append(
            (phase, completed, total)
        ),
    )

    assert loaded.ready_payload["pack_id"] == manifest.pack_id
    assert loaded.ready_payload["retrieval_revision"] == "retrieval-" + "e" * 64
    assert calls["verified_root"] == tmp_path.resolve()
    assert calls["verify_database_integrity"] is False
    assert events == [("sidecar-model", 0, 1), ("sidecar-model", 1, 1)]


def _request(request_id: str, op: str, payload: dict[str, object]) -> bytes:
    return (
        json.dumps(
            {
                "schema_version": 2,
                "request_id": request_id,
                "op": op,
                "payload": payload,
            },
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )


def _evidence(content: str = "legal evidence") -> EvidenceItem:
    return EvidenceItem(
        chunk_id="doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-chunk-0000000000000000",
        document_id="doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        content=content,
        citation="1 CFR 1.1",
        path="corpus/title-001/section-1.1.md",
        start_line=10,
        end_line=12,
        snapshot_date="2026-09-03",
        official_url="https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1",
        content_sha256="a" * 64,
        document_sha256="b" * 64,
        lexical_rank=1,
        dense_rank=2,
        fusion_score=0.03,
        is_neighbor=False,
        neighbor_of=None,
    )


class ScriptedRetriever:
    def __init__(self, *, item: EvidenceItem | None = None, fail: bool = False) -> None:
        self.item = item or _evidence()
        self.fail = fail
        self.calls: list[tuple[str, int, int, str]] = []

    def query_result(
        self, query: str, *, top_k: int, max_total_bytes: int, mode: str
    ) -> RetrievalResult:
        self.calls.append((query, top_k, max_total_bytes, mode))
        if self.fail:
            raise RuntimeError("SENTINEL query secret")
        return RetrievalResult([self.item], "matched")


@dataclass
class Factory:
    retriever: ScriptedRetriever
    calls: int = 0
    embedding_ready: bool = True
    index_ready: bool = True

    def __call__(self, root: Path) -> LoadedRuntime:
        assert root.is_absolute()
        self.calls += 1
        return LoadedRuntime(
            retriever=self.retriever,
            ready_payload={
                "pack_id": "pack-" + "c" * 32,
                "retrieval_revision": "retrieval-" + "e" * 64,
                "snapshot_date": "2026-09-03",
                "document_count": 30_000,
                "chunk_count": 45_000,
                "model": "BAAI/bge-m3",
                "revision": "d" * 40,
                "dimensions": 1024,
                "device": "cpu",
            },
            index_ready=self.index_ready,
            embedding_ready=self.embedding_ready,
        )


def _run(
    tmp_path: Path,
    lines: list[bytes],
    *,
    retriever: ScriptedRetriever | None = None,
) -> tuple[int, list[dict[str, object]], str, Factory]:
    factory = Factory(retriever or ScriptedRetriever())
    output = io.BytesIO()
    errors = io.StringIO()
    code = run_sidecar(
        tmp_path.resolve(),
        io.BytesIO(b"".join(lines)),
        output,
        errors,
        runtime_factory=factory,
    )
    decoded = [json.loads(line) for line in output.getvalue().decode("utf-8").splitlines()]
    return code, decoded, errors.getvalue(), factory


def test_sidecar_emits_ready_once_and_reuses_loaded_retriever(tmp_path: Path) -> None:
    code, output, errors, factory = _run(
        tmp_path,
        [
            _request(REQUEST_ONE, "health", {}),
            _request(
                REQUEST_TWO,
                "query",
                {"query": "tax", "top_k": 6, "max_total_bytes": 32768, "mode": "hybrid"},
            ),
            _request(REQUEST_THREE, "shutdown", {}),
        ],
    )

    assert code == 0
    assert errors == ""
    assert [line["op"] for line in output] == [
        "ready",
        "health_result",
        "query_result",
        "shutdown_result",
    ]
    assert output[1]["request_id"] == REQUEST_ONE
    assert output[2]["request_id"] == REQUEST_TWO
    assert output[2]["payload"]["outcome"] == "matched"
    assert factory.calls == 1
    assert factory.retriever.calls == [("tax", 6, 32768, "hybrid")]
    item = output[2]["payload"]["items"][0]
    assert item["source_id"].startswith("doc-")
    assert item["metadata"]["citation"] == "1 CFR 1.1"
    assert item["metadata"]["retrieval_revision"] == "retrieval-" + "e" * 64


def test_sidecar_accepts_dense_query_mode(tmp_path: Path) -> None:
    code, output, errors, factory = _run(
        tmp_path,
        [
            _request(
                REQUEST_ONE,
                "query",
                {"query": "tax", "top_k": 6, "max_total_bytes": 32768, "mode": "dense"},
            ),
            _request(REQUEST_TWO, "shutdown", {}),
        ],
    )

    assert code == 0
    assert errors == ""
    assert [line["op"] for line in output] == ["ready", "query_result", "shutdown_result"]
    assert factory.retriever.calls == [("tax", 6, 32768, "dense")]


@pytest.mark.parametrize(
    "line",
    [
        b'{"schema_version":2,"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"health","payload":{}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"health","payload":{},"extra":1}',
        b'{"schema_version":NaN,"request_id":"req-11111111111111111111111111111111","op":"health","payload":{}}',
        b'{"schema_version":2,"request_id":"wrong","op":"health","payload":{}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"health","payload":{"extra":1}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"query","payload":{"query":"x","top_k":true,"max_total_bytes":1,"mode":"hybrid"}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"query","payload":{"query":"x","top_k":1,"max_total_bytes":1,"mode":"auto"}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"bogus","payload":{}}',
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111","op":"health","payload":{}}\x01',
        b"\xff\xfe",
        b"x" * 65_537,
    ],
    ids=[
        "duplicate-key",
        "extra-key",
        "nan",
        "wrong-id",
        "payload-extra",
        "boolean-top-k",
        "unknown-mode",
        "unknown-op",
        "control-byte",
        "invalid-utf8",
        "oversized",
    ],
)
def test_protocol_rejects_owned_schema_violations(line: bytes) -> None:
    with pytest.raises(ProtocolError):
        parse_v2_message(line)


def test_clean_eof_after_ready_is_success(tmp_path: Path) -> None:
    code, output, errors, factory = _run(tmp_path, [])

    assert code == 0
    assert [line["op"] for line in output] == ["ready"]
    assert errors == ""
    assert factory.calls == 1


def test_invalid_request_gets_one_safe_error_and_later_health_still_works(
    tmp_path: Path,
) -> None:
    code, output, errors, _ = _run(
        tmp_path,
        [b"SENTINEL invalid JSON\n", _request(REQUEST_ONE, "health", {})],
    )

    assert code == 0
    assert [line["op"] for line in output] == ["ready", "error", "health_result"]
    assert output[1]["payload"] == {
        "code": "InvalidRequest",
        "message": "request rejected",
    }
    assert "SENTINEL" not in json.dumps(output)
    assert errors == ""


def test_oversized_input_line_is_drained_and_emits_exactly_one_error(
    tmp_path: Path,
) -> None:
    code, output, errors, _ = _run(
        tmp_path,
        [b"x" * 70_000 + b"\n", _request(REQUEST_ONE, "health", {})],
    )

    assert code == 0
    assert [line["op"] for line in output] == ["ready", "error", "health_result"]
    assert errors == ""


def test_reused_request_id_is_rejected(tmp_path: Path) -> None:
    code, output, errors, _ = _run(
        tmp_path,
        [
            _request(REQUEST_ONE, "health", {}),
            _request(REQUEST_ONE, "health", {}),
        ],
    )

    assert code == 0
    assert [line["op"] for line in output] == ["ready", "health_result", "error"]
    assert output[2]["request_id"] == REQUEST_ONE
    assert output[2]["payload"]["code"] == "InvalidRequest"
    assert errors == ""


def test_query_failure_is_safe_and_emits_no_partial_result(tmp_path: Path) -> None:
    code, output, errors, _ = _run(
        tmp_path,
        [
            _request(
                REQUEST_ONE,
                "query",
                {"query": "SENTINEL private", "top_k": 1, "max_total_bytes": 20, "mode": "hybrid"},
            )
        ],
        retriever=ScriptedRetriever(fail=True),
    )

    assert code == 0
    assert [line["op"] for line in output] == ["ready", "error"]
    assert output[1]["request_id"] == REQUEST_ONE
    assert output[1]["payload"]["code"] == "DependencyUnavailable"
    assert "SENTINEL" not in json.dumps(output) + errors


def test_oversized_response_becomes_safe_error(tmp_path: Path) -> None:
    code, output, errors, _ = _run(
        tmp_path,
        [
            _request(
                REQUEST_ONE,
                "query",
                {"query": "x", "top_k": 1, "max_total_bytes": 32768, "mode": "hybrid"},
            )
        ],
        retriever=ScriptedRetriever(item=_evidence("x" * 65_000)),
    )

    assert code == 0
    assert [line["op"] for line in output] == ["ready", "error"]
    assert output[1]["payload"]["code"] == "ResponseTooLarge"
    assert errors == ""


def test_runtime_load_failure_emits_no_ready_or_private_path(tmp_path: Path) -> None:
    def fail(root: Path) -> LoadedRuntime:
        raise RuntimeError(f"SENTINEL {root}")

    output = io.BytesIO()
    errors = io.StringIO()
    code = run_sidecar(
        tmp_path.resolve(), io.BytesIO(), output, errors, runtime_factory=fail
    )

    assert code == 2
    assert output.getvalue() == b""
    assert errors.getvalue() == "rag sidecar initialization failed\n"
    assert "SENTINEL" not in errors.getvalue()


def test_default_runtime_load_failure_marks_the_active_progress_phase_failed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def fail(root: Path, *, device: str, progress: object) -> LoadedRuntime:
        del root, device
        assert callable(progress)
        progress("sidecar-model", 0, 1)
        raise RuntimeError("SENTINEL")

    monkeypatch.setattr(sidecar_module, "_load_runtime", fail)
    output = io.BytesIO()
    errors = io.StringIO()

    code = run_sidecar(tmp_path.resolve(), io.BytesIO(), output, errors)

    lines = errors.getvalue().splitlines()
    started = json.loads(lines[0])
    failed = json.loads(lines[1])
    assert code == 2
    assert output.getvalue() == b""
    assert started["phase"] == failed["phase"] == "sidecar-model"
    assert started["status"] == "running"
    assert failed["status"] == "failed"
    assert lines[2] == "rag sidecar initialization failed"
    assert "SENTINEL" not in errors.getvalue()


# T5: ready frames now carry `index_ready` and `embedding_ready`. The
# sidecar advertises both as true by default and a v3 client can read
# either combination.
def test_sidecar_emits_v3_ready_with_capability_fields(tmp_path: Path) -> None:
    code, output, _, _ = _run(tmp_path, [])
    assert code == 0
    ready = output[0]
    assert ready["op"] == "ready"
    assert ready["schema_version"] == 3
    assert ready["payload"]["index_ready"] is True
    assert ready["payload"]["embedding_ready"] is True


def test_sidecar_prepare_op_reports_negotiated_capabilities(
    tmp_path: Path,
) -> None:
    prepare_request = (
        json.dumps(
            {
                "schema_version": 3,
                "request_id": REQUEST_ONE,
                "op": "prepare",
                "payload": {"capabilities": ["index", "embedding"]},
            },
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )
    code, output, _, _ = _run(tmp_path, [prepare_request])
    assert code == 0
    result = output[1]
    assert result["op"] == "prepare_result"
    assert result["schema_version"] == 3
    assert result["payload"]["requested"] == ["index", "embedding"]
    assert result["payload"]["capabilities"] == {
        "index": True,
        "embedding": True,
    }


def test_sidecar_refuses_semantic_query_without_embedding_ready(
    tmp_path: Path,
) -> None:
    factory = Factory(ScriptedRetriever())
    factory.embedding_ready = False
    output = io.BytesIO()
    errors = io.StringIO()
    query_request = (
        json.dumps(
            {
                "schema_version": 3,
                "request_id": REQUEST_ONE,
                "op": "query",
                "payload": {
                    "query": "tax",
                    "top_k": 6,
                    "max_total_bytes": 32768,
                    "mode": "dense",
                },
            },
            separators=(",", ":"),
        ).encode("utf-8")
        + b"\n"
    )
    code = run_sidecar(
        tmp_path.resolve(),
        io.BytesIO(b"".join([query_request])),
        output,
        errors,
        runtime_factory=factory,
    )
    decoded = [json.loads(line) for line in output.getvalue().decode("utf-8").splitlines()]
    assert code == 0
    assert decoded[0]["op"] == "ready"
    assert decoded[0]["payload"]["embedding_ready"] is False
    assert decoded[1]["op"] == "error"
    assert decoded[1]["payload"]["code"] == "EmbeddingUnavailable"


def test_sidecar_error_uses_request_schema_version(tmp_path: Path) -> None:
    bad_v3 = (
        b'{"schema_version":3,"request_id":"req-11111111111111111111111111111111",'
        b'"op":"bogus","payload":{}}\n'
    )
    code, output, _, _ = _run(tmp_path, [bad_v3])
    assert code == 0
    error = output[1]
    assert error["schema_version"] == 3
    assert error["payload"]["code"] == "InvalidRequest"


def test_sidecar_v2_error_keeps_v2_envelope(tmp_path: Path) -> None:
    bad_v2 = (
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111",'
        b'"op":"bogus","payload":{}}\n'
    )
    code, output, _, _ = _run(tmp_path, [bad_v2])
    assert code == 0
    assert output[1]["schema_version"] == 2


def test_v2_decoder_rejects_v3_query_request() -> None:
    request = (
        b'{"schema_version":3,"request_id":"req-11111111111111111111111111111111",'
        b'"op":"query","payload":{"query":"x","top_k":1,"max_total_bytes":1,"mode":"hybrid"}}'
    )
    with pytest.raises(ProtocolError):
        parse_v2_message(request)


def test_v3_decoder_rejects_v2_query_request() -> None:
    request = (
        b'{"schema_version":2,"request_id":"req-11111111111111111111111111111111",'
        b'"op":"query","payload":{"query":"x","top_k":1,"max_total_bytes":1,"mode":"hybrid"}}'
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)


def test_v3_decoder_rejects_v3_prepare_with_unknown_capability() -> None:
    request = (
        b'{"schema_version":3,"request_id":"req-11111111111111111111111111111111",'
        b'"op":"prepare","payload":{"capabilities":["quantum"]}}'
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)
