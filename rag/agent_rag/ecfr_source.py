from __future__ import annotations

from dataclasses import asdict, dataclass, replace
import datetime as _datetime
from enum import Enum
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import time
from typing import Callable, Protocol
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import HTTPRedirectHandler, OpenerDirector, Request, build_opener
import uuid


TITLES_URL = "https://www.ecfr.gov/api/versioner/v1/titles.json"
_FULL_URL = "https://www.ecfr.gov/api/versioner/v1/full/{date}/title-{number}.xml"
_DATE = re.compile(r"\d{4}-\d{2}-\d{2}\Z")


class DownloadError(RuntimeError):
    pass


class DownloadDisposition(Enum):
    DOWNLOADED = "downloaded"
    REUSED = "reused"


@dataclass(frozen=True)
class DownloadLimits:
    max_bytes: int = 768 * 1024 * 1024
    attempts: int = 5
    timeout_seconds: int = 60


@dataclass(frozen=True)
class DownloadRecord:
    url: str
    relative_path: str
    bytes: int
    sha256: str
    etag: str
    last_modified: str
    retrieved_utc: str
    disposition: DownloadDisposition


@dataclass(frozen=True)
class DownloadConfig:
    snapshot_date: str
    staging_root: Path
    limits: DownloadLimits = DownloadLimits()


@dataclass(frozen=True)
class DownloadSummary:
    snapshot_date: str
    title_count: int
    downloaded: int
    reused: int
    records: tuple[DownloadRecord, ...]


class _ReadableResponse(Protocol):
    status: int
    headers: object

    def read(self, size: int = -1) -> bytes: ...
    def __enter__(self) -> _ReadableResponse: ...
    def __exit__(self, *args: object) -> None: ...


class _Opener(Protocol):
    def open(self, request: object, timeout: int) -> _ReadableResponse: ...


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(
        self,
        req: object,
        fp: object,
        code: int,
        msg: str,
        headers: object,
        newurl: str,
    ) -> None:
        del req, fp, code, msg, headers, newurl
        return None


def titles_url() -> str:
    return TITLES_URL


def title_xml_url(snapshot_date: str, title_number: int) -> str:
    if type(snapshot_date) is not str or _DATE.fullmatch(snapshot_date) is None:
        raise DownloadError("snapshot date is invalid")
    try:
        parsed = _datetime.date.fromisoformat(snapshot_date)
    except ValueError as error:
        raise DownloadError("snapshot date is invalid") from error
    if parsed.isoformat() != snapshot_date:
        raise DownloadError("snapshot date is invalid")
    if type(title_number) is not int or not 1 <= title_number <= 50:
        raise DownloadError("title number is invalid")
    return _FULL_URL.format(date=snapshot_date, number=title_number)


def _validate_limits(limits: DownloadLimits) -> None:
    if (
        type(limits.max_bytes) is not int
        or not 1 <= limits.max_bytes <= 1024 * 1024 * 1024
        or type(limits.attempts) is not int
        or not 1 <= limits.attempts <= 8
        or type(limits.timeout_seconds) is not int
        or not 1 <= limits.timeout_seconds <= 300
    ):
        raise DownloadError("download limits are invalid")


def _validate_url(url: str, allowed_host: str) -> None:
    if type(url) is not str or type(allowed_host) is not str:
        raise DownloadError("download URL is invalid")
    parsed = urlparse(url)
    if (
        parsed.scheme != "https"
        or parsed.hostname != allowed_host
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port not in {None, 443}
        or not parsed.path.startswith("/")
        or parsed.fragment
    ):
        raise DownloadError("download URL is invalid")


def _is_link_or_reparse(path: Path) -> bool:
    status = os.lstat(path)
    if stat.S_ISLNK(status.st_mode):
        return True
    attributes = getattr(status, "st_file_attributes", 0)
    reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse)


def _validate_destination(destination: Path) -> Path:
    target = Path(destination)
    if not target.is_absolute():
        raise DownloadError("download destination must be absolute")
    current = Path(target.anchor)
    for component in target.parts[1:]:
        current /= component
        if current.exists() or current.is_symlink():
            try:
                if _is_link_or_reparse(current):
                    raise DownloadError("download destination is linked")
            except OSError as error:
                raise DownloadError("download destination inspection failed") from error
    if target.exists():
        status = os.stat(target, follow_symlinks=False)
        if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
            raise DownloadError("download destination is not a regular single-link file")
    return target


def _utc_now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def _header(headers: object, name: str) -> str:
    getter = getattr(headers, "get", None)
    if not callable(getter):
        return ""
    value = getter(name, "")
    return value if type(value) is str else ""


def _sidecar_path(destination: Path) -> Path:
    return destination.with_name(destination.name + ".download.json")


def _load_reusable(destination: Path, url: str) -> DownloadRecord | None:
    metadata_path = _sidecar_path(destination)
    if not destination.exists() or not metadata_path.exists():
        return None
    try:
        _validate_destination(destination)
        _validate_destination(metadata_path)
        value = json.loads(metadata_path.read_text(encoding="utf-8"))
        if type(value) is not dict or set(value) != {
            "url",
            "relative_path",
            "bytes",
            "sha256",
            "etag",
            "last_modified",
            "retrieved_utc",
        }:
            return None
        data = destination.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if value["url"] != url or value["bytes"] != len(data) or value["sha256"] != digest:
            return None
        return DownloadRecord(
            url=url,
            relative_path=str(value["relative_path"]),
            bytes=len(data),
            sha256=digest,
            etag=str(value["etag"]),
            last_modified=str(value["last_modified"]),
            retrieved_utc=str(value["retrieved_utc"]),
            disposition=DownloadDisposition.REUSED,
        )
    except (OSError, UnicodeError, ValueError, TypeError, DownloadError):
        return None


def _write_metadata(path: Path, record: DownloadRecord) -> None:
    value = asdict(record)
    value.pop("disposition")
    temporary = path.with_name(path.name + f".partial-{uuid.uuid4().hex}")
    data = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def download_one(
    url: str,
    destination: Path,
    *,
    allowed_host: str = "www.ecfr.gov",
    opener: _Opener | None = None,
    sleeper: Callable[[float], None] = time.sleep,
    limits: DownloadLimits = DownloadLimits(),
) -> DownloadRecord:
    _validate_url(url, allowed_host)
    _validate_limits(limits)
    target = _validate_destination(Path(destination))
    reusable = _load_reusable(target, url)
    if reusable is not None:
        return reusable
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        _validate_destination(target)
    except OSError as error:
        raise DownloadError("download destination could not be prepared") from error
    active_opener: _Opener = opener if opener is not None else build_opener(_NoRedirect())
    last_error: BaseException | None = None
    for attempt in range(limits.attempts):
        partial = target.with_name(target.name + f".partial-{uuid.uuid4().hex}")
        try:
            request = Request(
                url,
                headers={
                    "User-Agent": "AgentFramework-eCFR-Knowledge-Builder/1.0",
                    "Accept": "application/xml,application/json;q=0.9",
                    "Accept-Encoding": "gzip",
                },
                method="GET",
            )
            with active_opener.open(request, timeout=limits.timeout_seconds) as response:
                status_code = getattr(response, "status", 200)
                if 300 <= status_code < 400:
                    raise DownloadError("redirect target is not allowed")
                content_type = _header(response.headers, "Content-Type").casefold()
                if content_type and not any(
                    allowed in content_type
                    for allowed in (
                        "application/xml",
                        "text/xml",
                        "application/json",
                        "application/octet-stream",
                    )
                ):
                    raise DownloadError("response content type is invalid")
                content_length = _header(response.headers, "Content-Length")
                if content_length:
                    try:
                        if int(content_length) > limits.max_bytes:
                            raise DownloadError("response exceeded byte limit")
                    except ValueError as error:
                        raise DownloadError("response length is invalid") from error
                content_encoding = _header(response.headers, "Content-Encoding").casefold().strip()
                if content_encoding not in {"", "identity", "gzip"}:
                    raise DownloadError("response content encoding is invalid")
                source = gzip.GzipFile(fileobj=response) if content_encoding == "gzip" else response
                digest = hashlib.sha256()
                total = 0
                with partial.open("xb") as stream:
                    while block := source.read(min(1024 * 1024, limits.max_bytes + 1)):
                        total += len(block)
                        if total > limits.max_bytes:
                            raise DownloadError("response exceeded byte limit")
                        stream.write(block)
                        digest.update(block)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(partial, target)
                record = DownloadRecord(
                    url=url,
                    relative_path=target.name,
                    bytes=total,
                    sha256=digest.hexdigest(),
                    etag=_header(response.headers, "ETag"),
                    last_modified=_header(response.headers, "Last-Modified"),
                    retrieved_utc=_utc_now(),
                    disposition=DownloadDisposition.DOWNLOADED,
                )
                _write_metadata(_sidecar_path(target), record)
                return record
        except HTTPError as error:
            last_error = error
            if 300 <= error.code < 400:
                raise DownloadError("redirect target is not allowed") from error
            if error.code not in {429, 500, 502, 503, 504}:
                raise DownloadError("download returned a permanent HTTP error") from error
        except URLError as error:
            last_error = error
        except DownloadError:
            raise
        except OSError as error:
            last_error = error
        finally:
            try:
                partial.unlink(missing_ok=True)
            except OSError:
                pass
        if attempt + 1 < limits.attempts:
            sleeper(float(min(2**attempt, 8)))
    raise DownloadError("download failed after retries") from last_error


def _load_titles(path: Path) -> list[tuple[int, str]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        raise DownloadError("titles response is invalid") from error
    if type(value) is not dict or type(value.get("titles")) is not list:
        raise DownloadError("titles response is invalid")
    titles: list[tuple[int, str]] = []
    for item in value["titles"]:
        if type(item) is not dict:
            raise DownloadError("titles response is invalid")
        number = item.get("number")
        name = item.get("name")
        reserved = item.get("reserved")
        if type(number) is not int or type(name) is not str or type(reserved) is not bool:
            raise DownloadError("titles response is invalid")
        if not reserved:
            titles.append((number, name))
    if len(titles) != 49 or len({number for number, _ in titles}) != 49:
        raise DownloadError("titles response did not contain 49 non-reserved titles")
    return sorted(titles)


def download_ecfr_snapshot(config: DownloadConfig) -> DownloadSummary:
    root = Path(config.staging_root)
    if not root.is_absolute():
        raise DownloadError("staging root must be absolute")
    raw_root = root / "raw"
    titles_record = replace(
        download_one(TITLES_URL, raw_root / "titles.json", limits=config.limits),
        relative_path="raw/titles.json",
    )
    title_metadata = _load_titles(raw_root / "titles.json")
    records = [titles_record]
    for number, _ in title_metadata:
        record = download_one(
            title_xml_url(config.snapshot_date, number),
            raw_root / f"title-{number:03d}.xml",
            limits=config.limits,
        )
        records.append(replace(record, relative_path=f"raw/title-{number:03d}.xml"))
    manifest_path = root / "manifest" / "downloads.jsonl"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for record in records:
        value = asdict(record)
        value["disposition"] = record.disposition.value
        lines.append(json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
    manifest_path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="")
    return DownloadSummary(
        snapshot_date=config.snapshot_date,
        title_count=len(title_metadata),
        downloaded=sum(item.disposition is DownloadDisposition.DOWNLOADED for item in records),
        reused=sum(item.disposition is DownloadDisposition.REUSED for item in records),
        records=tuple(records),
    )
