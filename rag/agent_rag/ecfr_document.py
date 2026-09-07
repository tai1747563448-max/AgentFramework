from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
from typing import Iterable, Iterator
from urllib.parse import quote
import uuid

from defusedxml import ElementTree


_SHA256 = re.compile(r"[0-9a-f]{64}\Z")
_DIVISION = re.compile(r"DIV\d+\Z")
_SPACE = re.compile(r"\s+")
_UNSAFE_XML_DECLARATION = re.compile(br"<!\s*(?:DOCTYPE|ENTITY)\b", re.IGNORECASE)
_SAFE_COMPONENT = re.compile(r"[^A-Za-z0-9._-]+")
_LEGAL_STATUS = "Official eCFR snapshot; informational retrieval only; not legal advice."


class CorpusError(RuntimeError):
    pass


@dataclass(frozen=True)
class TitleMetadata:
    number: int
    name: str
    snapshot_date: str
    source_url: str
    source_sha256: str
    retrieved_utc: str


@dataclass(frozen=True)
class EcfrDocument:
    schema_version: int
    document_id: str
    citation: str
    title_number: int
    title_name: str
    chapter: str
    chapter_name: str
    subchapter: str
    subchapter_name: str
    part: str
    part_name: str
    section: str
    section_title: str
    snapshot_date: str
    source_xml_url: str
    official_url: str
    source_xml_sha256: str
    body: str
    body_sha256: str
    body_bytes: int
    retrieved_utc: str
    legal_status: str

    @classmethod
    def create(
        cls,
        *,
        metadata: TitleMetadata,
        chapter: str,
        chapter_name: str,
        subchapter: str,
        subchapter_name: str,
        part: str,
        part_name: str,
        section: str,
        section_title: str,
        body: str,
    ) -> EcfrDocument:
        _validate_metadata(metadata)
        normalized_body = _normalize_body(body)
        if not normalized_body:
            raise CorpusError("section body is empty")
        normalized_section = _one_line(section)
        if not normalized_section:
            raise CorpusError("section number is empty")
        citation = f"{metadata.number} CFR {normalized_section}"
        body_sha256 = hashlib.sha256(normalized_body.encode("utf-8")).hexdigest()
        document_seed = f"{metadata.snapshot_date}\0{citation}\0{body_sha256}".encode("utf-8")
        document_id = "doc-" + hashlib.sha256(document_seed).hexdigest()[:32]
        official_url = (
            f"https://www.ecfr.gov/on/{metadata.snapshot_date}/title-{metadata.number}"
            f"/section-{quote(normalized_section, safe='.-')}"
        )
        return cls(
            schema_version=2,
            document_id=document_id,
            citation=citation,
            title_number=metadata.number,
            title_name=_one_line(metadata.name),
            chapter=_one_line(chapter),
            chapter_name=_one_line(chapter_name),
            subchapter=_one_line(subchapter),
            subchapter_name=_one_line(subchapter_name),
            part=_one_line(part),
            part_name=_one_line(part_name),
            section=normalized_section,
            section_title=_one_line(section_title),
            snapshot_date=metadata.snapshot_date,
            source_xml_url=metadata.source_url,
            official_url=official_url,
            source_xml_sha256=metadata.source_sha256,
            body=normalized_body,
            body_sha256=body_sha256,
            body_bytes=len(normalized_body.encode("utf-8")),
            retrieved_utc=metadata.retrieved_utc,
            legal_status=_LEGAL_STATUS,
        )


@dataclass(frozen=True)
class CorpusSummary:
    schema_version: int
    candidates: int
    selected: int
    manifest_path: str


@dataclass
class _Frame:
    tag: str
    number: str
    type: str
    heading: str = ""


def _one_line(text: str) -> str:
    return _SPACE.sub(" ", text).strip()


def _normalize_body(text: str) -> str:
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    retained: list[str] = []
    blank = False
    for line in lines:
        value = line.strip()
        if not value:
            if retained and not blank:
                retained.append("")
            blank = True
            continue
        retained.append(value)
        blank = False
    while retained and not retained[-1]:
        retained.pop()
    return "\n".join(retained)


def _validate_metadata(metadata: TitleMetadata) -> None:
    if (
        type(metadata.number) is not int
        or not 1 <= metadata.number <= 50
        or not metadata.name
        or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", metadata.snapshot_date)
        or not metadata.source_url.startswith("https://www.ecfr.gov/")
        or _SHA256.fullmatch(metadata.source_sha256) is None
        or not metadata.retrieved_utc.endswith("Z")
    ):
        raise CorpusError("title metadata is invalid")


def _tag(element: object) -> str:
    value = getattr(element, "tag", "")
    if type(value) is not str:
        return ""
    return value.rsplit("}", 1)[-1].upper()


def _text(element: object) -> str:
    iterator = getattr(element, "itertext", None)
    if not callable(iterator):
        return ""
    return _one_line(" ".join(iterator()))


def _escape_cell(value: str) -> str:
    return value.replace("|", "\\|").replace("\n", " ")


def _render_table(element: object) -> str:
    rows: list[list[str]] = []
    iterator = getattr(element, "iter", None)
    if not callable(iterator):
        return ""
    for row in iterator():
        if _tag(row) not in {"ROW", "TR"}:
            continue
        cells = [
            _escape_cell(_text(cell))
            for cell in list(row)
            if _tag(cell) in {"ENT", "ENTRY", "TD", "TH"}
        ]
        if cells:
            rows.append(cells)
    if not rows:
        return _text(element)
    width = max(len(row) for row in rows)
    padded = [row + [""] * (width - len(row)) for row in rows]
    output = ["| " + " | ".join(row) + " |" for row in padded]
    output.insert(1, "| " + " | ".join("---" for _ in range(width)) + " |")
    return "\n".join(output)


def _render_list(element: object) -> str:
    items: list[str] = []
    for child in list(element):
        if _tag(child) in {"LI", "ITEM"}:
            value = _text(child)
            if value:
                items.append(f"- {value}")
    return "\n".join(items) if items else _text(element)


def _render_block(element: object) -> str:
    tag = _tag(element)
    if tag in {"TABLE", "GPOTABLE"}:
        return _render_table(element)
    if tag in {"UL", "OL", "LIST"}:
        return _render_list(element)
    value = _text(element)
    if tag in {"NOTE", "NOTES"} and value:
        return f"> Note: {value}"
    return value


def _section_body(element: object) -> str:
    blocks: list[str] = []
    for child in list(element):
        if _tag(child) == "HEAD":
            continue
        rendered = _render_block(child)
        if rendered:
            blocks.append(rendered)
    return _normalize_body("\n\n".join(blocks))


def _heading_without_number(heading: str, section: str) -> str:
    value = re.sub(rf"^§+\s*{re.escape(section)}\s*", "", heading).strip()
    return value.rstrip(".") if value else heading


def _frame(frames: list[_Frame], kind: str) -> _Frame:
    for frame in reversed(frames):
        if frame.type == kind:
            return frame
    return _Frame("", "", kind, "")


def _scan_forbidden_xml(path: Path, maximum_bytes: int) -> str:
    digest = hashlib.sha256()
    try:
        if path.stat().st_size > maximum_bytes:
            raise CorpusError("title XML exceeded byte limit")
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                if _UNSAFE_XML_DECLARATION.search(block):
                    raise CorpusError("DTD and entities are forbidden")
                digest.update(block)
    except OSError as error:
        raise CorpusError("title XML could not be read") from error
    return digest.hexdigest()


def _require_source_file(path: Path) -> Path:
    source = Path(path)
    if not source.is_absolute():
        raise CorpusError("title XML path must be absolute")
    try:
        status = os.lstat(source)
    except OSError as error:
        raise CorpusError("title XML is unavailable") from error
    attributes = getattr(status, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    if (
        stat.S_ISLNK(status.st_mode)
        or bool(attributes & reparse)
        or not stat.S_ISREG(status.st_mode)
        or status.st_nlink != 1
    ):
        raise CorpusError("title XML must be a regular single-link file")
    return source


def parse_title_xml(
    path: Path,
    metadata: TitleMetadata,
    *,
    maximum_bytes: int = 768 * 1024 * 1024,
) -> Iterator[EcfrDocument]:
    _validate_metadata(metadata)
    source = _require_source_file(Path(path))
    if _scan_forbidden_xml(source, maximum_bytes) != metadata.source_sha256:
        raise CorpusError("title XML digest does not match manifest")
    frames: list[_Frame] = []
    try:
        events = ElementTree.iterparse(
            source,
            events=("start", "end"),
            forbid_dtd=True,
            forbid_entities=True,
            forbid_external=True,
        )
        for event, element in events:
            tag = _tag(element)
            if event == "start" and _DIVISION.fullmatch(tag):
                frames.append(
                    _Frame(
                        tag=tag,
                        number=_one_line(element.attrib.get("N", "")),
                        type=_one_line(element.attrib.get("TYPE", "")).upper(),
                    )
                )
                continue
            if event == "end" and tag == "HEAD" and frames:
                frames[-1].heading = _text(element)
                continue
            if event != "end" or not _DIVISION.fullmatch(tag):
                continue
            if not frames or frames[-1].tag != tag:
                raise CorpusError("title XML hierarchy is invalid")
            current = frames[-1]
            if current.type == "SECTION":
                body = _section_body(element)
                if "[reserved]" not in current.heading.casefold() and body:
                    chapter = _frame(frames, "CHAPTER")
                    subchapter = _frame(frames, "SUBCHAP")
                    part = _frame(frames, "PART")
                    yield EcfrDocument.create(
                        metadata=metadata,
                        chapter=chapter.number,
                        chapter_name=chapter.heading,
                        subchapter=subchapter.number,
                        subchapter_name=subchapter.heading,
                        part=part.number,
                        part_name=part.heading,
                        section=current.number,
                        section_title=_heading_without_number(current.heading, current.number),
                        body=body,
                    )
            frames.pop()
            element.clear()
    except CorpusError:
        raise
    except Exception as error:
        raise CorpusError("title XML parsing failed") from error


def _same_document(left: EcfrDocument, right: EcfrDocument) -> bool:
    return asdict(left) == asdict(right)


def _deduplicate(documents: Iterable[EcfrDocument]) -> list[EcfrDocument]:
    by_citation: dict[str, EcfrDocument] = {}
    for document in documents:
        existing = by_citation.get(document.citation)
        if existing is None:
            by_citation[document.citation] = document
        elif not _same_document(existing, document):
            raise CorpusError("conflicting duplicate citation")
    return list(by_citation.values())


def select_documents(documents: Iterable[EcfrDocument], *, count: int) -> list[EcfrDocument]:
    if type(count) is not int or count <= 0:
        raise CorpusError("document count is invalid")
    unique = _deduplicate(documents)
    if len(unique) < count:
        raise CorpusError("not enough complete documents")
    return sorted(
        unique,
        key=lambda item: (-item.body_bytes, item.citation, item.body_sha256),
    )[:count]


def _component(value: str, fallback: str) -> str:
    normalized = _SAFE_COMPONENT.sub("-", value.strip()).strip(".-_")
    return (normalized or fallback)[:48]


def document_relative_path(document: EcfrDocument) -> str:
    return "/".join(
        [
            "corpus",
            f"title-{document.title_number:03d}",
            f"chapter-{_component(document.chapter, 'none')}",
            f"subchapter-{_component(document.subchapter, 'none')}",
            f"part-{_component(document.part, 'none')}",
            f"section-{_component(document.section, 'unknown')}--{document.body_sha256[:12]}.md",
        ]
    )


_FRONT_MATTER_FIELDS = (
    "schema_version",
    "document_id",
    "citation",
    "title_number",
    "title_name",
    "chapter",
    "chapter_name",
    "subchapter",
    "subchapter_name",
    "part",
    "part_name",
    "section",
    "section_title",
    "snapshot_date",
    "source_xml_url",
    "official_url",
    "source_xml_sha256",
    "body_sha256",
    "body_bytes",
    "retrieved_utc",
    "legal_status",
)


def serialize_markdown(document: EcfrDocument) -> str:
    lines = ["---"]
    for name in _FRONT_MATTER_FIELDS:
        value = getattr(document, name)
        if type(value) is str:
            rendered = json.dumps(value, ensure_ascii=False)
        else:
            rendered = str(value)
        lines.append(f"{name}: {rendered}")
    lines.extend(["---", "", f"# {document.citation} — {document.section_title}", "", document.body])
    return "\n".join(lines) + "\n"


def _document_payload(document: EcfrDocument) -> str:
    return json.dumps(asdict(document), ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _from_payload(payload: str) -> EcfrDocument:
    value = json.loads(payload)
    return EcfrDocument(**value)


def _write_atomic(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + f".partial-{uuid.uuid4().hex}")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _manifest_record(document: EcfrDocument, path: str, markdown_sha256: str) -> dict[str, object]:
    value = asdict(document)
    value.pop("body")
    value["path"] = path
    value["markdown_sha256"] = markdown_sha256
    return value


def write_corpus(
    documents: Iterable[EcfrDocument],
    staging_root: Path,
    *,
    count: int,
) -> CorpusSummary:
    root = Path(staging_root)
    if not root.is_absolute() or type(count) is not int or count <= 0:
        raise CorpusError("corpus build configuration is invalid")
    root.mkdir(parents=True, exist_ok=True)
    candidates_path = root / "manifest" / "candidates.sqlite3"
    candidates_path.parent.mkdir(parents=True, exist_ok=True)
    if candidates_path.exists():
        raise CorpusError("candidate database already exists")
    connection = sqlite3.connect(candidates_path)
    candidate_count = 0
    selected: list[EcfrDocument] = []
    try:
        connection.execute(
            "CREATE TABLE candidates ("
            "citation TEXT PRIMARY KEY, body_bytes INTEGER NOT NULL, "
            "body_sha256 TEXT NOT NULL, payload TEXT NOT NULL)"
        )
        for document in documents:
            payload = _document_payload(document)
            existing = connection.execute(
                "SELECT payload FROM candidates WHERE citation = ?", (document.citation,)
            ).fetchone()
            if existing is None:
                connection.execute(
                    "INSERT INTO candidates VALUES (?, ?, ?, ?)",
                    (document.citation, document.body_bytes, document.body_sha256, payload),
                )
                candidate_count += 1
            elif existing[0] != payload:
                raise CorpusError("conflicting duplicate citation")
        connection.commit()
        rows = connection.execute(
            "SELECT payload FROM candidates "
            "ORDER BY body_bytes DESC, citation ASC, body_sha256 ASC LIMIT ?",
            (count,),
        ).fetchall()
        if len(rows) != count:
            raise CorpusError("not enough complete documents")
        selected = [_from_payload(row[0]) for row in rows]
    except (sqlite3.Error, UnicodeError, ValueError, TypeError) as error:
        raise CorpusError("candidate selection failed") from error
    finally:
        connection.close()
    manifest_lines: list[str] = []
    seen_paths: set[str] = set()
    for document in selected:
        relative_path = document_relative_path(document)
        if relative_path in seen_paths:
            raise CorpusError("document path collision")
        seen_paths.add(relative_path)
        markdown = serialize_markdown(document).encode("utf-8")
        markdown_sha256 = hashlib.sha256(markdown).hexdigest()
        _write_atomic(root.joinpath(*relative_path.split("/")), markdown)
        manifest_lines.append(
            json.dumps(
                _manifest_record(document, relative_path, markdown_sha256),
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            )
        )
    manifest_path = root / "manifest" / "documents.jsonl"
    _write_atomic(manifest_path, ("\n".join(manifest_lines) + "\n").encode("utf-8"))
    report = {
        "schema_version": 2,
        "candidates": candidate_count,
        "selected": len(selected),
        "manifest_path": "manifest/documents.jsonl",
    }
    _write_atomic(
        root / "reports" / "corpus-build.json",
        json.dumps(report, sort_keys=True, separators=(",", ":")).encode("utf-8"),
    )
    try:
        candidates_path.unlink()
    except OSError as error:
        raise CorpusError("candidate database cleanup failed") from error
    return CorpusSummary(2, candidate_count, len(selected), "manifest/documents.jsonl")


_DOWNLOAD_RECORD_KEYS = {
    "url",
    "relative_path",
    "bytes",
    "sha256",
    "etag",
    "last_modified",
    "retrieved_utc",
    "disposition",
}


def _downloaded_title_inputs(
    root: Path, snapshot_date: str
) -> list[tuple[Path, TitleMetadata]]:
    try:
        titles_value = json.loads((root / "raw" / "titles.json").read_text(encoding="utf-8"))
        download_lines = (root / "manifest" / "downloads.jsonl").read_text(
            encoding="utf-8"
        ).splitlines()
    except (OSError, UnicodeError, ValueError) as error:
        raise CorpusError("download manifest is invalid") from error
    if type(titles_value) is not dict or type(titles_value.get("titles")) is not list:
        raise CorpusError("titles manifest is invalid")
    titles: dict[int, str] = {}
    for item in titles_value["titles"]:
        if type(item) is not dict:
            raise CorpusError("titles manifest is invalid")
        number = item.get("number")
        name = item.get("name")
        reserved = item.get("reserved")
        if type(number) is not int or type(name) is not str or type(reserved) is not bool:
            raise CorpusError("titles manifest is invalid")
        if not reserved:
            if number in titles:
                raise CorpusError("titles manifest is invalid")
            titles[number] = name
    if len(titles) != 49 or any(not 1 <= number <= 50 for number in titles):
        raise CorpusError("titles manifest must contain 49 non-reserved titles")
    records: dict[str, dict[str, object]] = {}
    for line in download_lines:
        try:
            value = json.loads(line)
        except (ValueError, TypeError) as error:
            raise CorpusError("download manifest is invalid") from error
        if type(value) is not dict or set(value) != _DOWNLOAD_RECORD_KEYS:
            raise CorpusError("download manifest is invalid")
        relative_path = value["relative_path"]
        if type(relative_path) is not str or relative_path in records:
            raise CorpusError("download manifest is invalid")
        records[relative_path] = value
    inputs: list[tuple[Path, TitleMetadata]] = []
    for number, name in sorted(titles.items()):
        relative_path = f"raw/title-{number:03d}.xml"
        record = records.get(relative_path)
        if record is None:
            raise CorpusError("download manifest is missing a title")
        url = record["url"]
        digest = record["sha256"]
        retrieved_utc = record["retrieved_utc"]
        if (
            url != f"https://www.ecfr.gov/api/versioner/v1/full/{snapshot_date}/title-{number}.xml"
            or type(digest) is not str
            or _SHA256.fullmatch(digest) is None
            or type(retrieved_utc) is not str
            or not retrieved_utc.endswith("Z")
        ):
            raise CorpusError("download manifest is invalid")
        inputs.append(
            (
                root / "raw" / f"title-{number:03d}.xml",
                TitleMetadata(
                    number=number,
                    name=name,
                    snapshot_date=snapshot_date,
                    source_url=url,
                    source_sha256=digest,
                    retrieved_utc=retrieved_utc,
                ),
            )
        )
    return inputs


def build_corpus_from_downloads(
    staging_root: Path,
    *,
    snapshot_date: str,
    count: int,
) -> CorpusSummary:
    root = Path(staging_root)
    if not root.is_absolute():
        raise CorpusError("staging root must be absolute")
    inputs = _downloaded_title_inputs(root, snapshot_date)

    def documents() -> Iterator[EcfrDocument]:
        for path, metadata in inputs:
            yield from parse_title_xml(path, metadata)

    return write_corpus(documents(), root, count=count)
