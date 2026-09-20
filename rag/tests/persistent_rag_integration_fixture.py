"""Offline C++ integration: real v2 sidecar/index/retriever, tiny synthetic corpus.

Only model loading and release-pack verification are substituted. The ready
identity satisfies the production release protocol; it is not a corpus census.
"""
from pathlib import Path
import hashlib
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent_rag.chunker import DocumentSource, chunk_documents
from agent_rag.hybrid_index import build_index_from_chunks
from agent_rag.hybrid_retriever import HybridRetriever
from agent_rag.sidecar import LoadedRuntime, run_sidecar


class WordTokenizer:
    def encode(self, text):
        return text.split()

    def decode(self, tokens):
        return " ".join(tokens)

    def get_vocab(self):
        return {"parseIssue": 1, "warning": 2, "中文": 3}


class OfflineEmbedding:
    # T19 schema-3 identity: backend_identity (model, revision, backend,
    # precision, dimensions) is the gating tuple for index reuse. The
    # fixture is offline (numpy-backed) so backend = "numpy" and
    # precision = "float32" exactly match what SUPPORTED_BACKENDS and
    # SUPPORTED_PRECISIONS whitelist.
    model = "BAAI/bge-m3"
    revision = "d" * 40
    backend = "numpy"
    precision = "float32"
    dimensions = 1024
    tokenizer = WordTokenizer()

    def encode(self, texts, *, batch_size=16):
        rows = np.zeros((len(texts), self.dimensions), dtype=np.float32)
        rows[:, 0] = 1.0
        return rows


def load_fixture(root):
    backend = OfflineEmbedding()
    index = root / "index"
    if not index.exists():
        content = "parseIssue handles 中文 warning. Preserve UTF-8 and add tests."
        document = DocumentSource(
            document_id="doc-" + "a" * 32,
            citation="40 CFR 60.1",
            path="corpus/title-040/section-60.1.md",
            section_title="Fixture rule",
            snapshot_date="2026-09-03",
            official_url="https://www.ecfr.gov/on/2026-09-03/title-40/section-60.1",
            document_sha256=hashlib.sha256(content.encode()).hexdigest(),
            body=content,
            body_start_line=10,
        )
        chunks = chunk_documents([document], backend.tokenizer)
        build_index_from_chunks(index, [document], chunks, backend)
    return LoadedRuntime(
        retriever=HybridRetriever(index, embedding=backend),
        ready_payload={
            "pack_id": "pack-" + "c" * 32,
            "retrieval_revision": "retrieval-" + "e" * 64,
            "snapshot_date": "2026-09-03",
            "document_count": 30_000,
            "chunk_count": 45_000,
            "model": backend.model,
            "revision": backend.revision,
            # schema-3 identity: the index must round-trip through
            # backend/precision so the verifier and reloader can
            # refuse a release pack whose embedding backend does not
            # match the offline fixture.
            "backend": backend.backend,
            "precision": backend.precision,
            "dimensions": backend.dimensions,
            "device": "cpu",
        },
    )


if __name__ == "__main__":
    assert sys.argv[1:3] == ["serve", "--pack-root"]
    raise SystemExit(run_sidecar(
        Path(sys.argv[3]), sys.stdin.buffer, sys.stdout.buffer, sys.stderr,
        runtime_factory=load_fixture,
    ))
