from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import BinaryIO, Callable, Protocol, TextIO

from .embedding import BgeM3Embedding, EmbeddingError, encode_normalized, select_embedding_device
from .hybrid_retriever import EvidenceItem, HybridRetriever, RetrievalResult
from .pack import retrieval_revision, verify_runtime_pack
from .progress import ProgressReporter
from .protocol import ClientMessage, ProtocolError, parse_message, parse_v2_message


SERVER_REQUEST_ID = "req-00000000000000000000000000000000"
MAX_LINE_BYTES = 65_536


class Retriever(Protocol):
    def query_result(
        self,
        query: str,
        *,
        top_k: int,
        max_total_bytes: int,
        mode: str,
    ) -> RetrievalResult: ...


@dataclass(frozen=True)
class LoadedRuntime:
    retriever: Retriever
    ready_payload: dict[str, object]
    index_ready: bool = True
    embedding_ready: bool = True


def _load_runtime(
    pack_root: Path,
    device: str = "auto",
    progress: Callable[..., None] | None = None,
) -> LoadedRuntime:
    manifest = verify_runtime_pack(pack_root, progress=progress)
    runtime_revision = retrieval_revision(manifest)
    model_root = pack_root / "model" / "bge-m3"
    selected = select_embedding_device(device)
    if progress is not None:
        progress("sidecar-model", 0, 1)
    embedding_loaded = True
    try:
        embedding = BgeM3Embedding(model_root, device=selected)
        encode_normalized(embedding, ["sidecar startup self test"], dimensions=1024)
    except EmbeddingError:
        if selected != "cuda":
            raise
        selected = "cpu"
        embedding = BgeM3Embedding(model_root, device=selected)
        encode_normalized(embedding, ["sidecar startup self test"], dimensions=1024)
    if progress is not None:
        progress("sidecar-model", 1, 1)
    # T5: index-only mode is not yet wired (see hybrid_retriever
    # notes). The flag is exposed today so the sidecar can advertise
    # the capability honestly once the lexical-only path lands.
    index_ready = True
    retriever = HybridRetriever(
        pack_root / "index",
        embedding=embedding,
        dense_min=manifest.relevance_dense_min,
        progress=progress,
        verify_database_integrity=False,
    )
    return LoadedRuntime(
        retriever,
        {
            "pack_id": manifest.pack_id,
            "retrieval_revision": runtime_revision,
            "snapshot_date": manifest.snapshot_date,
            "document_count": manifest.document_count,
            "chunk_count": manifest.chunk_count,
            "model": manifest.embedding_model,
            "revision": manifest.embedding_revision,
            "dimensions": manifest.embedding_dimensions,
            "device": selected,
        },
        index_ready=index_ready,
        embedding_ready=embedding_loaded,
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


def _response(
    request_id: str,
    op: str,
    payload: dict[str, object],
    schema_version: int = 2,
) -> dict[str, object]:
    # T5: errors must echo the request's schema version so the
    # orchestrator never sees a v2 frame in response to a v3 request
    # (or vice versa).
    return {
        "schema_version": schema_version,
        "request_id": request_id,
        "op": op,
        "payload": payload,
    }


def _error(request_id: str, code: str, schema_version: int = 2) -> dict[str, object]:
    return _response(
        request_id,
        "error",
        {"code": code, "message": "request rejected"},
        schema_version=schema_version,
    )


def _schema_version(data: bytes) -> int:
    """Best-effort schema_version extraction for error reporting.

    The sidecar must echo the request's schema version on errors so a
    client never receives a v2 frame in response to a v3 request. We
    parse the frame loosely here; if the request is so malformed we
    cannot read the version we fall back to v2 (the protocol default).
    """
    try:
        value = json.loads(data)
    except (ValueError, TypeError, UnicodeError):
        return 2
    if not isinstance(value, dict):
        return 2
    version = value.get("schema_version")
    if isinstance(version, int) and version in {2, 3}:
        return version
    return 2


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


def _evidence_json(
    item: EvidenceItem, runtime_revision: str
) -> dict[str, object]:
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
            "retrieval_revision": runtime_revision,
            "bm25_rank": item.lexical_rank,
            "dense_rank": item.dense_rank,
            "fusion_score": item.fusion_score,
            "is_neighbor": item.is_neighbor,
            "neighbor_of": item.neighbor_of,
        },
    }


def _handle(
    runtime: LoadedRuntime, message: ClientMessage
) -> tuple[dict[str, object], bool]:
    schema_version = message.schema_version
    if message.op == "health":
        return (
            _response(
                message.request_id,
                "health_result",
                {"status": "ok"},
                schema_version=schema_version,
            ),
            False,
        )
    if message.op == "shutdown":
        return (
            _response(
                message.request_id,
                "shutdown_result",
                {"status": "stopping"},
                schema_version=schema_version,
            ),
            True,
        )
    if message.op == "prepare":
        # T5: capability negotiation is a no-op on the wire for the
        # first iteration. The sidecar already advertises the
        # available capabilities in the ready frame, and v3 clients
        # honour that by refusing to send a query that requires a
        # missing capability. The op exists so the orchestrator can
        # log explicit intent and so future versions can defer the
        # expensive embedding load until the client asks for it.
        requested = list(message.payload.get("capabilities", []))
        available = {
            name: ready
            for name, ready in (
                ("index", runtime.index_ready),
                ("embedding", runtime.embedding_ready),
            )
        }
        return (
            _response(
                message.request_id,
                "prepare_result",
                {"capabilities": available, "requested": requested},
                schema_version=schema_version,
            ),
            False,
        )
    payload = message.payload
    # T5: refuse a semantic query if the embedding model was not
    # loaded. The orchestrator already checks the ready frame, but
    # the sidecar enforces it as well so a buggy client cannot
    # silently downgrade to lexical.
    mode = str(payload["mode"])
    if mode in {"dense", "hybrid"} and not runtime.embedding_ready:
        return (
            _error(
                message.request_id,
                "EmbeddingUnavailable",
                schema_version=schema_version,
            ),
            False,
        )
    result = runtime.retriever.query_result(
        str(payload["query"]),
        top_k=int(payload["top_k"]),
        max_total_bytes=int(payload["max_total_bytes"]),
        mode=mode,
    )
    return (
        _response(
            message.request_id,
            "query_result",
            {
                "outcome": result.outcome,
                "items": [
                    _evidence_json(item, str(runtime.ready_payload["retrieval_revision"]))
                    for item in result.items
                ],
            },
            schema_version=schema_version,
        ),
        False,
    )


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
        reporter = ProgressReporter(stderr)
        active_phase: str | None = None

        def report(phase: str, completed: int, total: int, **_: object) -> None:
            nonlocal active_phase
            reporter.update(phase, completed, total, mode="sidecar")
            if completed == total:
                if active_phase == phase:
                    active_phase = None
            else:
                active_phase = phase

        runtime = (
            runtime_factory(root)
            if runtime_factory is not None
            else _load_runtime(root, device=device, progress=report)
        )
        # T5: always emit a v3 ready frame so new clients can read
        # the index_ready / embedding_ready fields. Legacy clients
        # keep parsing it as v2 because they ignore unknown payload
        # fields.
        ready_payload = dict(runtime.ready_payload)
        ready_payload["index_ready"] = runtime.index_ready
        ready_payload["embedding_ready"] = runtime.embedding_ready
        _write(
            stdout,
            _response(
                SERVER_REQUEST_ID,
                "ready",
                ready_payload,
                schema_version=3,
            ),
        )
    except Exception:
        if "active_phase" in locals() and active_phase is not None:
            try:
                reporter.fail(active_phase, mode="sidecar")
            except Exception:
                pass
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
            message = parse_message(raw)
        except ProtocolError:
            # T5: echo the request's schema version even when the
            # frame is otherwise malformed so a v3 client never
            # receives a v2 error (or vice versa).
            _write(
                stdout,
                _error(
                    SERVER_REQUEST_ID,
                    "InvalidRequest",
                    schema_version=_schema_version(raw),
                ),
            )
            continue
        if message.request_id in seen_request_ids:
            _write(
                stdout,
                _error(
                    message.request_id,
                    "InvalidRequest",
                    schema_version=message.schema_version,
                ),
            )
            continue
        seen_request_ids.add(message.request_id)
        try:
            response, stop = _handle(runtime, message)
        except Exception:
            _write(
                stdout,
                _error(
                    message.request_id,
                    "DependencyUnavailable",
                    schema_version=message.schema_version,
                ),
            )
            continue
        try:
            _write(stdout, response)
        except ProtocolError:
            _write(
                stdout,
                _error(
                    message.request_id,
                    "ResponseTooLarge",
                    schema_version=message.schema_version,
                ),
            )
            continue
        if stop:
            return 0
