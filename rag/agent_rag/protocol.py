from __future__ import annotations

from dataclasses import dataclass
import json
import re
from typing import Any


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class ClientMessage:
    request_id: str
    op: str
    payload: dict[str, object]
    schema_version: int = 2


_REQUEST_ID = re.compile(r"req-[0-9a-f]{32}\Z")
SUPPORTED_VERSIONS = frozenset({2, 3})


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def _reject_constant(_: str) -> None:
    raise ValueError("nonfinite number")


def _validate_payload(op: str, payload: dict[str, object]) -> None:
    if op in {"health", "shutdown"}:
        if payload:
            raise ProtocolError("payload keys are invalid")
        return
    # T5: both v2 and v3 queries share the same payload shape. The
    # only addition is the requested capability set, which lives at
    # the envelope level (op == "prepare") rather than inside the
    # query payload.
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
        or mode not in {"hybrid", "dense", "lexical"}
    ):
        raise ProtocolError("query payload is invalid")


def _validate_prepare_payload(payload: dict[str, object]) -> None:
    """T5: validate the `prepare` payload.

    The client declares which capabilities it needs for the upcoming
    turn so the sidecar can avoid loading the embedding model for
    pure citation lookups.
    """
    if set(payload) != {"capabilities"}:
        raise ProtocolError("payload keys are invalid")
    capabilities = payload["capabilities"]
    if not isinstance(capabilities, list) or not capabilities:
        raise ProtocolError("capabilities are invalid")
    if not all(isinstance(value, str) for value in capabilities):
        raise ProtocolError("capabilities are invalid")
    if not all(value in {"index", "embedding"} for value in capabilities):
        raise ProtocolError("capabilities are invalid")


def parse_message(line: str | bytes) -> ClientMessage:
    """T5: unified decoder that accepts both v2 and v3 frames.

    Each decoder still rejects frames tagged with a version it was
    not written for. The orchestrator relies on this to keep v3
    capability negotiation off the v2 path and vice versa.
    """
    if isinstance(line, bytes):
        data = line
    elif type(line) is str:
        try:
            data = line.encode("utf-8", errors="strict")
        except UnicodeError as error:
            raise ProtocolError("request is not UTF-8") from error
    else:
        raise ProtocolError("request type is invalid")
    # The sidecar always strips the trailing newline, but the public
    # decoder is also called directly from tests that hand it framed
    # bytes. Drop a single trailing CRLF so both paths agree on what
    # "one frame" means.
    if data.endswith(b"\n"):
        data = data[:-1]
        if data.endswith(b"\r"):
            data = data[:-1]
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
    if type(value["schema_version"]) is not int or value["schema_version"] not in SUPPORTED_VERSIONS:
        raise ProtocolError("schema version is invalid")
    request_id = value["request_id"]
    op = value["op"]
    payload = value["payload"]
    if type(request_id) is not str or _REQUEST_ID.fullmatch(request_id) is None:
        raise ProtocolError("request id is invalid")
    if type(op) is not str:
        raise ProtocolError("operation is invalid")
    if type(payload) is not dict:
        raise ProtocolError("payload is invalid")
    if value["schema_version"] == 3:
        # T5: v3 accepts the legacy health/query/shutdown ops plus a
        # new "prepare" op for capability negotiation. The decoder
        # must reject any v3 op it does not understand so the
        # orchestrator can rely on the sidecar rejecting unknown ops
        # rather than silently dropping them.
        if op not in {"health", "query", "shutdown", "prepare"}:
            raise ProtocolError("operation is invalid")
        if op == "prepare":
            _validate_prepare_payload(payload)
        else:
            _validate_payload(op, payload)
    else:
        if op not in {"health", "query", "shutdown"}:
            raise ProtocolError("operation is invalid")
        _validate_payload(op, payload)
    return ClientMessage(request_id, op, payload, value["schema_version"])


def parse_v2_message(line: str | bytes) -> ClientMessage:
    """Strict v2 decoder used by legacy packs. Rejects v3 frames."""
    message = parse_message(line)
    if message.schema_version != 2:
        raise ProtocolError("schema version is invalid")
    return message


def parse_v3_message(line: str | bytes) -> ClientMessage:
    """Strict v3 decoder used by capability-aware packs. Rejects v2 frames."""
    message = parse_message(line)
    if message.schema_version != 3:
        raise ProtocolError("schema version is invalid")
    return message
