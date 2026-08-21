from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
import sys

from .indexer import build_index
from .protocol import parse_query_request, response_json
from .retriever import query_index


def _build(arguments: list[str]) -> int:
    if len(arguments) != 5 or arguments[1] != "--source" or arguments[3] != "--index":
        raise ValueError("invalid build arguments")
    summary = build_index(Path(arguments[2]), Path(arguments[4]))
    sys.stdout.write(
        json.dumps(asdict(summary), ensure_ascii=False, separators=(",", ":"))
        + "\n"
    )
    return 0


def _query(arguments: list[str]) -> int:
    if len(arguments) != 3 or arguments[1] != "--index":
        raise ValueError("invalid query arguments")
    request = parse_query_request(sys.stdin.read(65_537))
    items = query_index(
        Path(arguments[2]),
        request.query,
        top_k=request.top_k,
        max_total_bytes=request.max_total_bytes,
    )
    sys.stdout.write(response_json(items) + "\n")
    return 0


def main(arguments: list[str] | None = None) -> int:
    values = list(sys.argv[1:] if arguments is None else arguments)
    command = values[0] if values else ""
    try:
        if command == "build":
            return _build(values)
        if command == "query":
            return _query(values)
        raise ValueError("unknown command")
    except Exception:
        if command == "build":
            sys.stderr.write("rag build failed\n")
        elif command == "query":
            sys.stderr.write("rag query failed\n")
        else:
            sys.stderr.write("rag command failed\n")
        return 2

