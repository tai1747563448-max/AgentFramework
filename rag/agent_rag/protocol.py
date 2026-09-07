from __future__ import annotations

from dataclasses import dataclass
import json
import re
from typing import Any


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class QueryRequest:
    query: str
    top_k: int
    max_total_bytes: int


@dataclass(frozen=True)
class ClientMessage:
    request_id: str
    op: str
    payload: dict[str, object]


_REQUEST_ID = re.compile(r"req-[0-9a-f]{32}\Z")


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def _reject_constant(_: str) -> None:
    raise ValueError("nonfinite number")


def parse_v2_message(line: str | bytes) -> ClientMessage:
    if isinstance(line, bytes):
        data = line
    elif type(line) is str:
        try:
            data = line.encode("utf-8", errors="strict")
        except UnicodeError as error:
            raise ProtocolError("request is not UTF-8") from error
    else:
        raise ProtocolError("request type is invalid")
    if (
        not data
        or len(data) > 65_536
        or any(byte < 0x20 or byte == 0x7F for byte in data)
    ):
        raise ProtocolError("request framing is invalid")
    try:
        text = data.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (UnicodeError, ValueError, TypeError, json.JSONDecodeError) as error:
        raise ProtocolError("request is not valid JSON") from error
    if type(value) is not dict or set(value) != {
        "schema_version",
        "request_id",
        "op",
        "payload",
    }:
        raise ProtocolError("request keys are invalid")
    if type(value["schema_version"]) is not int or value["schema_version"] != 2:
        raise ProtocolError("schema version is invalid")
    request_id = value["request_id"]
    op = value["op"]
    payload = value["payload"]
    if type(request_id) is not str or _REQUEST_ID.fullmatch(request_id) is None:
        raise ProtocolError("request id is invalid")
    if type(op) is not str or op not in {"health", "query", "shutdown"}:
        raise ProtocolError("operation is invalid")
    if type(payload) is not dict:
        raise ProtocolError("payload is invalid")
    if op in {"health", "shutdown"}:
        if payload:
            raise ProtocolError("payload keys are invalid")
    else:
        if set(payload) != {"query", "top_k", "max_total_bytes", "mode"}:
            raise ProtocolError("payload keys are invalid")
        query = payload["query"]
        top_k = payload["top_k"]
        maximum = payload["max_total_bytes"]
        mode = payload["mode"]
        if (
            type(query) is not str
            or not query
            or "\x00" in query
            or len(query.encode("utf-8")) > 16_384
            or type(top_k) is not int
            or not 1 <= top_k <= 20
            or type(maximum) is not int
            or not 1 <= maximum <= 32_768
            or type(mode) is not str
            or mode not in {"hybrid", "lexical"}
        ):
            raise ProtocolError("query payload is invalid")
    return ClientMessage(request_id, op, payload)


def parse_query_request(text: str) -> QueryRequest:
    if len(text.encode("utf-8")) > 65_536:
        raise ProtocolError("request is too large")
    try:
        value = json.loads(text)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise ProtocolError("request is not JSON") from error
    if type(value) is not dict:
        raise ProtocolError("request must be an object")
    if set(value) != {"schema_version", "query", "top_k", "max_total_bytes"}:
        raise ProtocolError("request keys are invalid")
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        raise ProtocolError("schema version is invalid")
    query = value["query"]
    if (
        type(query) is not str
        or not query
        or "\x00" in query
        or len(query.encode("utf-8")) > 16_384
    ):
        raise ProtocolError("query is invalid")
    top_k = value["top_k"]
    if type(top_k) is not int or not 1 <= top_k <= 20:
        raise ProtocolError("top_k is invalid")
    max_total_bytes = value["max_total_bytes"]
    if (
        type(max_total_bytes) is not int
        or not 1 <= max_total_bytes <= 32_768
    ):
        raise ProtocolError("max_total_bytes is invalid")
    return QueryRequest(query, top_k, max_total_bytes)


def response_json(items: list[dict[str, object]]) -> str:
    return json.dumps(
        {"schema_version": 1, "items": items},
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    )
