from __future__ import annotations

from dataclasses import dataclass
import json


class ProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class QueryRequest:
    query: str
    top_k: int
    max_total_bytes: int


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

