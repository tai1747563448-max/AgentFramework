from __future__ import annotations

import email.message
import gzip
from io import BytesIO
from pathlib import Path
import sys
from urllib.error import HTTPError, URLError

import pytest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.ecfr_source import (  # type: ignore[import-not-found]
    DownloadDisposition,
    DownloadError,
    DownloadLimits,
    download_one,
    title_xml_url,
    titles_url,
)


class _Response:
    def __init__(self, body: bytes, headers: dict[str, str] | None = None) -> None:
        self._stream = BytesIO(body)
        self.headers = email.message.Message()
        for name, value in (headers or {}).items():
            self.headers[name] = value
        self.status = 200

    def read(self, size: int = -1) -> bytes:
        return self._stream.read(size)

    def __enter__(self) -> _Response:
        return self

    def __exit__(self, *_: object) -> None:
        return None


class _ScriptedOpener:
    def __init__(self, outcomes: list[object]) -> None:
        self.outcomes = outcomes
        self.calls = 0
        self.requests: list[object] = []

    def open(self, request: object, timeout: int) -> _Response:
        del timeout
        self.requests.append(request)
        outcome = self.outcomes[self.calls]
        self.calls += 1
        if isinstance(outcome, BaseException):
            raise outcome
        assert isinstance(outcome, _Response)
        return outcome


def _http_error(code: int, location: str | None = None) -> HTTPError:
    headers = email.message.Message()
    if location is not None:
        headers["Location"] = location
    return HTTPError("https://www.ecfr.gov/source", code, "fixture", headers, None)


def test_official_snapshot_urls_are_exact() -> None:
    assert titles_url() == "https://www.ecfr.gov/api/versioner/v1/titles.json"
    assert title_xml_url("2026-09-03", 1) == (
        "https://www.ecfr.gov/api/versioner/v1/full/2026-09-03/title-1.xml"
    )
    with pytest.raises(DownloadError):
        title_xml_url("2026-9-3", 1)
    with pytest.raises(DownloadError):
        title_xml_url("2026-09-03", 0)


def test_download_retries_only_transient_failures_and_records_digest(
    tmp_path: Path,
) -> None:
    opener = _ScriptedOpener(
        [
            URLError("offline"),
            _http_error(503),
            _Response(b"<ECFR/>", {"ETag": '"v1"', "Last-Modified": "date"}),
        ]
    )
    delays: list[float] = []

    record = download_one(
        "https://www.ecfr.gov/title.xml",
        tmp_path / "title.xml",
        opener=opener,
        sleeper=delays.append,
        limits=DownloadLimits(max_bytes=1024, attempts=5, timeout_seconds=1),
    )

    assert opener.calls == 3
    assert delays == [1.0, 2.0]
    assert record.disposition is DownloadDisposition.DOWNLOADED
    assert record.bytes == len(b"<ECFR/>")
    assert record.sha256 == "4acebcc95f53ce376fee0c831d3d3531d1a080b18cb990f2b9bb9d381c23fd32"
    assert (tmp_path / "title.xml").read_bytes() == b"<ECFR/>"


def test_download_rejects_redirect_and_does_not_leave_partial_file(
    tmp_path: Path,
) -> None:
    opener = _ScriptedOpener([_http_error(302, "https://example.com/stolen.xml")])
    target = tmp_path / "title.xml"

    with pytest.raises(DownloadError, match="redirect target is not allowed"):
        download_one(
            "https://www.ecfr.gov/title.xml",
            target,
            opener=opener,
            sleeper=lambda _: None,
        )

    assert opener.calls == 1
    assert not target.exists()
    assert not list(tmp_path.glob("*.partial-*"))


def test_download_rejects_oversized_response(tmp_path: Path) -> None:
    opener = _ScriptedOpener([_Response(b"x" * 17)])

    with pytest.raises(DownloadError, match="response exceeded byte limit"):
        download_one(
            "https://www.ecfr.gov/title.xml",
            tmp_path / "title.xml",
            opener=opener,
            sleeper=lambda _: None,
            limits=DownloadLimits(max_bytes=16, attempts=1, timeout_seconds=1),
        )


def test_download_reuses_only_a_file_matching_its_sidecar(tmp_path: Path) -> None:
    first_opener = _ScriptedOpener([_Response(b"<ECFR/>")])
    first = download_one(
        "https://www.ecfr.gov/title.xml",
        tmp_path / "title.xml",
        opener=first_opener,
        sleeper=lambda _: None,
    )
    unused_opener = _ScriptedOpener([AssertionError("network must not be used")])

    second = download_one(
        "https://www.ecfr.gov/title.xml",
        tmp_path / "title.xml",
        opener=unused_opener,
        sleeper=lambda _: None,
    )

    assert first.disposition is DownloadDisposition.DOWNLOADED
    assert second.disposition is DownloadDisposition.REUSED
    assert second.sha256 == first.sha256
    assert unused_opener.calls == 0


def test_download_rejects_explicit_non_data_content_type(tmp_path: Path) -> None:
    opener = _ScriptedOpener([_Response(b"<html>login</html>", {"Content-Type": "text/html"})])

    with pytest.raises(DownloadError, match="response content type is invalid"):
        download_one(
            "https://www.ecfr.gov/title.xml",
            tmp_path / "title.xml",
            opener=opener,
            sleeper=lambda _: None,
        )


def test_download_requests_and_decodes_required_gzip_response(tmp_path: Path) -> None:
    opener = _ScriptedOpener(
        [
            _Response(
                gzip.compress(b"<ECFR/>"),
                {"Content-Type": "application/xml", "Content-Encoding": "gzip"},
            )
        ]
    )

    record = download_one(
        "https://www.ecfr.gov/title.xml",
        tmp_path / "title.xml",
        opener=opener,
        sleeper=lambda _: None,
    )

    request = opener.requests[0]
    assert getattr(request, "get_header")("Accept-encoding") == "gzip"
    assert (tmp_path / "title.xml").read_bytes() == b"<ECFR/>"
    assert record.bytes == 7
