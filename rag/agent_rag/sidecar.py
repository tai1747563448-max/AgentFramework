from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import BinaryIO, Callable, Protocol, TextIO

from .embedding import BgeM3Embedding, EmbeddingError, encode_normalized, select_embedding_device
from .hybrid_retriever import EvidenceItem, HybridRetriever
from .pack import verify_complete_pack
from .protocol import ClientMessage, ProtocolError, parse_v2_message


SERVER_REQUEST_ID = "req-00000000000000000000000000000000"
MAX_LINE_BYTES = 65_536


class Retriever(Protocol):
    def query(
        self,
        query: str,
        *,
        top_k: int,
        max_total_bytes: int,
        mode: str,
    ) -> list[EvidenceItem]: ...


@dataclass(frozen=True)
class LoadedRuntime:
    retriever: Retriever
    ready_payload: dict[str, object]


def _load_runtime(pack_root: Path, device: str = "auto") -> LoadedRuntime:
    manifest = verify_complete_pack(pack_root)
    model_root = pack_root / "model" / "bge-m3"
    selected = select_embedding_device(device)
    try:
        embedding = BgeM3Embedding(model_root, device=selected)
        encode_normalized(embedding, ["sidecar startup self test"], dimensions=1024)
    except EmbeddingError:
        if selected != "cuda":
            raise
        selected = "cpu"
        embedding = BgeM3Embedding(model_root, device=selected)
        encode_normalized(embedding, ["sidecar startup self test"], dimensions=1024)
    retriever = HybridRetriever(
        pack_root / "index",
        embedding=embedding,
        dense_min=manifest.relevance_dense_min,
    )
    return LoadedRuntime(
        retriever,
        {
            "pack_id": manifest.pack_id,
            "snapshot_date": manifest.snapshot_date,
            "document_count": manifest.document_count,
            "chunk_count": manifest.chunk_count,
            "model": manifest.embedding_model,
            "revision": manifest.embedding_revision,
            "dimensions": manifest.embedding_dimensions,
            "device": selected,
        },
    )


def _write(output: BinaryIO | TextIO, message: dict[str, object]) -> None:
    try:
        encoded = (
            json.dumps(
                message,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            ).encode("utf-8")
            + b"\n"
        )
    except (ValueError, TypeError, UnicodeError) as error:
        raise ProtocolError("response serialization failed") from error
    if len(encoded) > MAX_LINE_BYTES + 1:
        raise ProtocolError("response is too large")
    try:
        output.write(encoded)  # type: ignore[arg-type]
    except TypeError:
        output.write(encoded.decode("utf-8"))  # type: ignore[arg-type]
    output.flush()


def _response(request_id: str, op: str, payload: dict[str, object]) -> dict[str, object]:
    return {
        "schema_version": 2,
        "request_id": request_id,
        "op": op,
        "payload": payload,
    }


def _error(request_id: str, code: str) -> dict[str, object]:
    return _response(
        request_id,
        "error",
        {"code": code, "message": "request rejected"},
    )


def _readline(source: BinaryIO | TextIO) -> bytes | None:
    line = source.readline(MAX_LINE_BYTES + 2)
    if line in {b"", ""}:
        return None
    if type(line) is str:
        try:
            data = line.encode("utf-8", errors="strict")
        except UnicodeError as error:
            raise ProtocolError("request is not UTF-8") from error
    else:
        data = bytes(line)
    if len(data) > MAX_LINE_BYTES and not data.endswith(b"\n"):
        while True:
            remainder = source.readline(MAX_LINE_BYTES + 2)
            if remainder in {b"", ""}:
                break
            if type(remainder) is str:
                ended = remainder.endswith("\n")
            else:
                ended = bytes(remainder).endswith(b"\n")
            if ended:
                break
        raise ProtocolError("request is too large")
    if data.endswith(b"\n"):
        data = data[:-1]
        if data.endswith(b"\r"):
            data = data[:-1]
    if len(data) > MAX_LINE_BYTES:
        raise ProtocolError("request is too large")
    return data


def _evidence_json(item: EvidenceItem) -> dict[str, object]:
    return {
        "source_id": item.chunk_id,
        "content": item.content,
        "metadata": {
            "citation": item.citation,
            "path": item.path,
            "start_line": item.start_line,
            "end_line": item.end_line,
            "snapshot_date": item.snapshot_date,
            "official_url": item.official_url,
            "content_sha256": item.content_sha256,
            "document_sha256": item.document_sha256,
            "bm25_rank": item.lexical_rank,
            "dense_rank": item.dense_rank,
            "fusion_score": item.fusion_score,
            "is_neighbor": item.is_neighbor,
            "neighbor_of": item.neighbor_of,
        },
    }


def _handle(runtime: LoadedRuntime, message: ClientMessage) -> tuple[dict[str, object], bool]:
    if message.op == "health":
        return _response(message.request_id, "health_result", {"status": "ok"}), False
    if message.op == "shutdown":
        return _response(
            message.request_id, "shutdown_result", {"status": "stopping"}
        ), True
    payload = message.payload
    items = runtime.retriever.query(
        str(payload["query"]),
        top_k=int(payload["top_k"]),
        max_total_bytes=int(payload["max_total_bytes"]),
        mode=str(payload["mode"]),
    )
    return _response(
        message.request_id,
        "query_result",
        {"items": [_evidence_json(item) for item in items]},
    ), False


def run_sidecar(
    pack_root: Path,
    stdin: BinaryIO | TextIO,
    stdout: BinaryIO | TextIO,
    stderr: TextIO,
    *,
    device: str = "auto",
    runtime_factory: Callable[[Path], LoadedRuntime] | None = None,
) -> int:
    root = Path(pack_root)
    if not root.is_absolute():
        stderr.write("rag sidecar initialization failed\n")
        stderr.flush()
        return 2
    try:
        if device not in {"auto", "cuda", "cpu"}:
            raise ValueError("invalid device")
        runtime = (
            runtime_factory(root)
            if runtime_factory is not None
            else _load_runtime(root, device=device)
        )
        _write(stdout, _response(SERVER_REQUEST_ID, "ready", runtime.ready_payload))
    except Exception:
        stderr.write("rag sidecar initialization failed\n")
        stderr.flush()
        return 2
    seen_request_ids: set[str] = set()
    while True:
        try:
            raw = _readline(stdin)
        except ProtocolError:
            _write(stdout, _error(SERVER_REQUEST_ID, "InvalidRequest"))
            continue
        if raw is None:
            return 0
        try:
            message = parse_v2_message(raw)
        except ProtocolError:
            _write(stdout, _error(SERVER_REQUEST_ID, "InvalidRequest"))
            continue
        if message.request_id in seen_request_ids:
            _write(stdout, _error(message.request_id, "InvalidRequest"))
            continue
        seen_request_ids.add(message.request_id)
        try:
            response, stop = _handle(runtime, message)
        except Exception:
            _write(stdout, _error(message.request_id, "DependencyUnavailable"))
            continue
        try:
            _write(stdout, response)
        except ProtocolError:
            _write(stdout, _error(message.request_id, "ResponseTooLarge"))
            continue
        if stop:
            return 0
