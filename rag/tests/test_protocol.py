from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.protocol import (  # type: ignore[import-not-found]
    ProtocolError,
    parse_message,
    parse_v2_message,
    parse_v3_message,
)


def _encode(payload: dict[str, object]) -> bytes:
    return json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"


def test_parse_message_accepts_v2_health_request() -> None:
    request = _encode(
        {
            "schema_version": 2,
            "request_id": "req-11111111111111111111111111111111",
            "op": "health",
            "payload": {},
        }
    )
    message = parse_message(request)
    assert message.schema_version == 2
    assert message.op == "health"


def test_parse_message_accepts_v3_prepare_request() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "prepare",
            "payload": {"capabilities": ["index"]},
        }
    )
    message = parse_message(request)
    assert message.schema_version == 3
    assert message.op == "prepare"
    assert message.payload == {"capabilities": ["index"]}


def test_parse_message_rejects_unknown_schema_version() -> None:
    request = _encode(
        {
            "schema_version": 4,
            "request_id": "req-11111111111111111111111111111111",
            "op": "health",
            "payload": {},
        }
    )
    with pytest.raises(ProtocolError):
        parse_message(request)


def test_v2_decoder_rejects_v3_query_request() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "query",
            "payload": {
                "query": "x",
                "top_k": 1,
                "max_total_bytes": 1,
                "mode": "hybrid",
            },
        }
    )
    with pytest.raises(ProtocolError):
        parse_v2_message(request)


def test_v3_decoder_rejects_v2_query_request() -> None:
    request = _encode(
        {
            "schema_version": 2,
            "request_id": "req-11111111111111111111111111111111",
            "op": "query",
            "payload": {
                "query": "x",
                "top_k": 1,
                "max_total_bytes": 1,
                "mode": "hybrid",
            },
        }
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)


def test_v3_decoder_rejects_unknown_op() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "elevenses",
            "payload": {},
        }
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)


def test_v3_decoder_rejects_prepare_with_unknown_capability() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "prepare",
            "payload": {"capabilities": ["quantum"]},
        }
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)


def test_v3_decoder_rejects_prepare_with_non_list_capabilities() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "prepare",
            "payload": {"capabilities": "embedding"},
        }
    )
    with pytest.raises(ProtocolError):
        parse_v3_message(request)


def test_v2_decoder_accepts_legacy_query_request() -> None:
    request = _encode(
        {
            "schema_version": 2,
            "request_id": "req-11111111111111111111111111111111",
            "op": "query",
            "payload": {
                "query": "x",
                "top_k": 1,
                "max_total_bytes": 1,
                "mode": "hybrid",
            },
        }
    )
    message = parse_v2_message(request)
    assert message.schema_version == 2


def test_v3_decoder_accepts_v3_prepare_request() -> None:
    request = _encode(
        {
            "schema_version": 3,
            "request_id": "req-11111111111111111111111111111111",
            "op": "prepare",
            "payload": {"capabilities": ["index", "embedding"]},
        }
    )
    message = parse_v3_message(request)
    assert message.schema_version == 3
    assert message.payload == {"capabilities": ["index", "embedding"]}


def test_parse_message_rejects_duplicate_keys() -> None:
    request = (
        b'{"schema_version":2,"schema_version":2,'
        b'"request_id":"req-11111111111111111111111111111111",'
        b'"op":"health","payload":{}}'
    )
    with pytest.raises(ProtocolError):
        parse_message(request)