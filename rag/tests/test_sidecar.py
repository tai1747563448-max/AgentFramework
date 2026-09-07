from __future__ import annotations

from dataclasses import dataclass
import io
import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.hybrid_retriever import EvidenceItem  # type: ignore[import-not-found]
from agent_rag.protocol import ProtocolError, parse_v2_message  # type: ignore[import-not-found]
from agent_rag.sidecar import (  # type: ignore[import-not-found]
    LoadedRuntime,
    run_sidecar,
)


REQUEST_ONE = "req-11111111111111111111111111111111"
REQUEST_TWO = "req-22222222222222222222222222222222"
REQUEST_THREE = "req-33333333333333333333333333333333"


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

    def query(
        self, query: str, *, top_k: int, max_total_bytes: int, mode: str
    ) -> list[EvidenceItem]:
        self.calls.append((query, top_k, max_total_bytes, mode))
        if self.fail:
            raise RuntimeError("SENTINEL query secret")
        return [self.item]


@dataclass
class Factory:
    retriever: ScriptedRetriever
    calls: int = 0

    def __call__(self, root: Path) -> LoadedRuntime:
        assert root.is_absolute()
        self.calls += 1
        return LoadedRuntime(
            retriever=self.retriever,
            ready_payload={
                "pack_id": "pack-" + "c" * 32,
                "snapshot_date": "2026-09-03",
                "document_count": 30_000,
                "chunk_count": 45_000,
                "model": "BAAI/bge-m3",
                "revision": "d" * 40,
                "dimensions": 1024,
                "device": "cpu",
            },
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
    assert factory.calls == 1
    assert factory.retriever.calls == [("tax", 6, 32768, "hybrid")]
    item = output[2]["payload"]["items"][0]
    assert item["source_id"].startswith("doc-")
    assert item["metadata"]["citation"] == "1 CFR 1.1"


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
