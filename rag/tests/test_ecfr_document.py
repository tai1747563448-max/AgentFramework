from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.ecfr_document import (  # type: ignore[import-not-found]
    build_corpus_from_downloads,
    CorpusError,
    EcfrDocument,
    TitleMetadata,
    parse_title_xml,
    select_documents,
    serialize_markdown,
    write_corpus,
)


def _metadata(source_sha256: str = "a" * 64) -> TitleMetadata:
    return TitleMetadata(
        number=1,
        name="General Provisions",
        snapshot_date="2026-09-03",
        source_url="https://www.ecfr.gov/api/versioner/v1/full/2026-09-03/title-1.xml",
        source_sha256=source_sha256,
        retrieved_utc="2026-09-07T00:00:00Z",
    )


def _write_xml(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "title-1.xml"
    path.write_text(text, encoding="utf-8", newline="")
    return path


def _fixture_xml() -> str:
    return """<?xml version="1.0" encoding="UTF-8"?>
<ECFR>
  <DIV1 N="1" TYPE="TITLE"><HEAD>Title 1—General Provisions</HEAD>
    <DIV3 N="I" TYPE="CHAPTER"><HEAD>Chapter I—Administrative Committee</HEAD>
      <DIV4 N="A" TYPE="SUBCHAP"><HEAD>Subchapter A—General</HEAD>
        <DIV5 N="1" TYPE="PART"><HEAD>Part 1—Definitions</HEAD>
          <DIV8 N="1.1" TYPE="SECTION"><HEAD>§ 1.1 Definitions.</HEAD>
            <P>Agency means the test agency.</P>
            <TABLE><ROW><ENT>Term</ENT><ENT>Meaning</ENT></ROW><ROW><ENT>A</ENT><ENT>Alpha</ENT></ROW></TABLE>
            <NOTE><P>Source note remains part of the section.</P></NOTE>
          </DIV8>
          <DIV8 N="1.2" TYPE="SECTION"><HEAD>§ 1.2 [Reserved]</HEAD></DIV8>
        </DIV5>
      </DIV4>
    </DIV3>
  </DIV1>
</ECFR>
"""


def test_section_becomes_one_complete_markdown_with_provenance(tmp_path: Path) -> None:
    xml = _write_xml(tmp_path, _fixture_xml())
    metadata = _metadata(hashlib.sha256(xml.read_bytes()).hexdigest())

    documents = list(parse_title_xml(xml, metadata))

    assert [document.citation for document in documents] == ["1 CFR 1.1"]
    document = documents[0]
    assert document.chapter == "I"
    assert document.subchapter == "A"
    assert document.part == "1"
    assert "Agency means the test agency." in document.body
    assert "| Term | Meaning |" in document.body
    assert "Source note remains part of the section." in document.body
    markdown = serialize_markdown(document)
    assert 'official_url: "https://www.ecfr.gov/on/2026-09-03/title-1/section-1.1"' in markdown
    assert 'citation: "1 CFR 1.1"' in markdown
    assert markdown.endswith("Source note remains part of the section.\n")


def _document(citation: str, body: str, suffix: str) -> EcfrDocument:
    body_sha = hashlib.sha256(body.encode("utf-8")).hexdigest()
    return EcfrDocument.create(
        metadata=_metadata(),
        chapter="I",
        chapter_name="Chapter I",
        subchapter="A",
        subchapter_name="Subchapter A",
        part="1",
        part_name="Part 1",
        section=citation.split()[-1],
        section_title=f"Title {suffix}",
        body=body,
    )


def test_stable_selection_uses_body_bytes_then_citation_then_sha() -> None:
    short = _document("1 CFR 1.3", "short", "short")
    long_b = _document("1 CFR 1.2", "equally-long", "b")
    long_a = _document("1 CFR 1.1", "equally-long", "a")

    chosen = select_documents([short, long_b, long_a], count=2)

    assert [item.citation for item in chosen] == ["1 CFR 1.1", "1 CFR 1.2"]


def test_conflicting_duplicate_citation_fails() -> None:
    with pytest.raises(CorpusError, match="conflicting duplicate citation"):
        select_documents(
            [_document("1 CFR 1.1", "first", "a"), _document("1 CFR 1.1", "second", "b")],
            count=1,
        )


def test_xml_with_dtd_or_entity_is_rejected(tmp_path: Path) -> None:
    xml = _write_xml(
        tmp_path,
        '<!DOCTYPE ECFR [<!ENTITY leak SYSTEM "file:///secret">]><ECFR>&leak;</ECFR>',
    )
    with pytest.raises(CorpusError, match="DTD and entities are forbidden"):
        list(parse_title_xml(xml, _metadata()))


def test_title_xml_digest_must_match_download_metadata(tmp_path: Path) -> None:
    xml = _write_xml(tmp_path, _fixture_xml())

    with pytest.raises(CorpusError, match="title XML digest does not match manifest"):
        list(parse_title_xml(xml, _metadata("0" * 64)))


def test_write_corpus_streams_candidates_and_writes_exact_manifest(tmp_path: Path) -> None:
    root = tmp_path / "staging"
    documents = (
        item
        for item in [
            _document("1 CFR 1.3", "short", "short"),
            _document("1 CFR 1.2", "the longest body", "long"),
            _document("1 CFR 1.1", "medium body", "medium"),
        ]
    )

    summary = write_corpus(documents, root, count=2)

    assert summary.candidates == 3
    assert summary.selected == 2
    manifest_path = root / "manifest" / "documents.jsonl"
    rows = [json.loads(line) for line in manifest_path.read_text("utf-8").splitlines()]
    assert [row["citation"] for row in rows] == ["1 CFR 1.2", "1 CFR 1.1"]
    assert all((root / row["path"]).is_file() for row in rows)
    assert len(list((root / "corpus").rglob("*.md"))) == 2
    assert not (root / "manifest" / "candidates.sqlite3").exists()


def test_build_corpus_consumes_all_49_download_manifest_titles(tmp_path: Path) -> None:
    root = tmp_path / "staging"
    raw = root / "raw"
    raw.mkdir(parents=True)
    titles = []
    download_rows = []
    for number in range(1, 50):
        titles.append({"number": number, "name": f"Title {number}", "reserved": False})
        xml_path = raw / f"title-{number:03d}.xml"
        xml_path.write_text(_fixture_xml(), encoding="utf-8", newline="")
        digest = hashlib.sha256(xml_path.read_bytes()).hexdigest()
        download_rows.append(
            {
                "url": f"https://www.ecfr.gov/api/versioner/v1/full/2026-09-03/title-{number}.xml",
                "relative_path": f"raw/title-{number:03d}.xml",
                "bytes": xml_path.stat().st_size,
                "sha256": digest,
                "etag": "",
                "last_modified": "",
                "retrieved_utc": "2026-09-07T00:00:00Z",
                "disposition": "downloaded",
            }
        )
    (raw / "titles.json").write_text(
        json.dumps({"titles": titles}), encoding="utf-8", newline=""
    )
    manifest = root / "manifest" / "downloads.jsonl"
    manifest.parent.mkdir(parents=True)
    manifest.write_text(
        "\n".join(json.dumps(row, sort_keys=True) for row in download_rows) + "\n",
        encoding="utf-8",
        newline="",
    )

    summary = build_corpus_from_downloads(root, snapshot_date="2026-09-03", count=2)

    assert summary.candidates == 49
    assert summary.selected == 2
