from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


RAG_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RAG_ROOT))

from agent_rag.indexer import (  # noqa: E402
    BuildLimits,
    IndexBuildError,
    build_index,
)
from agent_rag.protocol import ProtocolError, parse_query_request  # noqa: E402
from agent_rag.retriever import IndexQueryError, query_index, tokenize  # noqa: E402


class TempTree:
    def __init__(self) -> None:
        self._directory = tempfile.TemporaryDirectory(prefix="agent-rag-")
        self.path = Path(self._directory.name)

    def close(self) -> None:
        self._directory.cleanup()


class TokenizerTests(unittest.TestCase):
    def test_tokenizer_handles_identifiers_and_chinese_deterministically(self) -> None:
        tokens = tokenize("HTTPParser parse_json 修复知识库检索")

        self.assertIn("httpparser", tokens)
        self.assertIn("http", tokens)
        self.assertIn("parser", tokens)
        self.assertIn("parse_json", tokens)
        self.assertIn("parse", tokens)
        self.assertIn("json", tokens)
        self.assertIn("知", tokens)
        self.assertIn("知识", tokens)
        self.assertIn("检索", tokens)
        self.assertEqual(tokens, tokenize("HTTPParser parse_json 修复知识库检索"))


class IndexAndQueryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = TempTree()
        self.source = self.temp.path / "语料"
        self.source.mkdir()
        self.index = self.temp.path / "index" / "knowledge.sqlite"

    def tearDown(self) -> None:
        self.temp.close()

    def _write(self, relative: str, content: str) -> Path:
        path = self.source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="")
        return path

    def test_build_and_query_returns_relevant_unicode_and_code_sources(self) -> None:
        self._write(
            "指南/知识库.md",
            "本地知识库使用 BM25 检索。\n结果必须保留稳定来源。\n",
        )
        self._write(
            "src/parser.cpp",
            "bool HttpParser::parse_header() { return true; }\n",
        )
        self._write("notes.txt", "unrelated gardening notes\n")

        summary = build_index(self.source, self.index)
        chinese = query_index(
            self.index, "知识库如何检索", top_k=3, max_total_bytes=32_768
        )
        code = query_index(
            self.index, "parse_header HttpParser", top_k=3, max_total_bytes=32_768
        )

        self.assertEqual(summary.schema_version, 1)
        self.assertEqual(summary.files_indexed, 3)
        self.assertGreaterEqual(summary.chunks_indexed, 3)
        self.assertTrue(chinese)
        self.assertEqual(chinese[0]["metadata"]["path"], "指南/知识库.md")
        self.assertIn("知识库", chinese[0]["content"])
        self.assertRegex(chinese[0]["source_id"], r"^指南/知识库\.md#L\d+-L\d+$")
        self.assertTrue(code)
        self.assertEqual(code[0]["metadata"]["path"], "src/parser.cpp")
        self.assertIn("parse_header", code[0]["content"])
        encoded = json.dumps(chinese, ensure_ascii=False)
        self.assertNotIn(str(self.source), encoded)
        self.assertNotIn("\\", chinese[0]["source_id"])

    def test_tied_scores_use_source_id_order(self) -> None:
        self._write("b.md", "shared token\n")
        self._write("a.md", "shared token\n")
        build_index(self.source, self.index)

        items = query_index(
            self.index, "shared token", top_k=10, max_total_bytes=32_768
        )

        self.assertEqual(
            [item["metadata"]["path"] for item in items], ["a.md", "b.md"]
        )

    def test_complete_rebuild_removes_stale_chunks(self) -> None:
        old = self._write("guide.md", "obsolete sentinel\n")
        build_index(self.source, self.index)
        self.assertTrue(
            query_index(
                self.index, "obsolete sentinel", top_k=5, max_total_bytes=32_768
            )
        )
        old.unlink()
        self._write("guide.md", "replacement topic\n")

        build_index(self.source, self.index)

        self.assertEqual(
            query_index(
                self.index, "obsolete sentinel", top_k=5, max_total_bytes=32_768
            ),
            [],
        )
        self.assertTrue(
            query_index(
                self.index, "replacement topic", top_k=5, max_total_bytes=32_768
            )
        )

    def test_failed_rebuild_preserves_previous_database(self) -> None:
        self._write("old.md", "preserved knowledge\n")
        build_index(self.source, self.index)
        self._write("second.md", "would exceed file budget\n")

        with self.assertRaises(IndexBuildError):
            build_index(
                self.source,
                self.index,
                BuildLimits(max_files=1, max_source_bytes=64 * 1024 * 1024),
            )

        items = query_index(
            self.index, "preserved knowledge", top_k=5, max_total_bytes=32_768
        )
        self.assertTrue(items)

    def test_protected_binary_invalid_oversized_and_links_are_skipped(self) -> None:
        self._write("allowed.md", "visible knowledge\n")
        self._write(".git/config.md", "git secret\n")
        self._write(".agent/control.md", "agent secret\n")
        self._write(".rag/private.md", "rag secret\n")
        self._write("build/generated.md", "build secret\n")
        self._write(".env", "SECRET=value\n")
        (self.source / "binary.txt").write_bytes(b"ok\x00hidden")
        (self.source / "invalid.txt").write_bytes(b"\xff\xfe")
        self._write("oversized.md", "x" * 64)
        link_created = False
        try:
            os.symlink(
                self.source / "allowed.md",
                self.source / "linked.md",
                target_is_directory=False,
            )
            os.symlink(
                self.source / "build",
                self.source / "linked-dir",
                target_is_directory=True,
            )
            link_created = True
        except OSError:
            pass

        summary = build_index(
            self.source,
            self.index,
            BuildLimits(max_file_bytes=32, max_source_bytes=1024),
        )
        items = query_index(
            self.index, "knowledge secret hidden", top_k=20, max_total_bytes=32_768
        )

        self.assertEqual(summary.files_indexed, 1)
        self.assertGreaterEqual(summary.files_skipped, 7)
        self.assertEqual([item["metadata"]["path"] for item in items], ["allowed.md"])
        if link_created:
            self.assertGreaterEqual(summary.links_skipped, 2)

    def test_oversized_unicode_line_uses_stable_part_ids_and_byte_bounds(self) -> None:
        self._write("generated.txt", "知识库" * 20 + "\n")
        limits = BuildLimits(
            max_file_bytes=1024,
            max_source_bytes=1024,
            chunk_bytes=24,
            overlap_lines=0,
        )

        build_index(self.source, self.index, limits)
        items = query_index(
            self.index, "知识库", top_k=20, max_total_bytes=32_768
        )

        self.assertGreater(len(items), 1)
        self.assertEqual(len({item["source_id"] for item in items}), len(items))
        self.assertTrue(
            all(len(item["content"].encode("utf-8")) <= 24 for item in items)
        )
        self.assertTrue(all("#L1-L1-P" in item["source_id"] for item in items))

    def test_query_respects_top_k_total_bytes_and_does_not_modify_index(self) -> None:
        for number in range(4):
            self._write(f"{number}.md", f"bounded result {number}\n")
        build_index(self.source, self.index)
        before = self.index.stat().st_mtime_ns

        items = query_index(
            self.index, "bounded result", top_k=2, max_total_bytes=20
        )
        after = self.index.stat().st_mtime_ns

        self.assertLessEqual(len(items), 2)
        self.assertLessEqual(
            sum(len(item["content"].encode("utf-8")) for item in items), 20
        )
        self.assertEqual(before, after)

    def test_query_rejects_a_linked_index_when_supported(self) -> None:
        self._write("guide.md", "linked database guard\n")
        build_index(self.source, self.index)
        alias = self.temp.path / "alias.sqlite"
        try:
            os.symlink(self.index, alias, target_is_directory=False)
        except OSError:
            self.skipTest("environment cannot create a file symlink")

        with self.assertRaises(IndexQueryError):
            query_index(alias, "guard", top_k=5, max_total_bytes=32_768)

    def test_hardlinked_sources_and_indexes_are_rejected(self) -> None:
        source_file = self._write("linked-source.md", "hard link sentinel\n")
        source_alias = self.source / "linked-alias.md"
        try:
            os.link(source_file, source_alias)
        except OSError:
            self.skipTest("environment cannot create a hard link")

        summary = build_index(self.source, self.index)
        self.assertEqual(summary.files_indexed, 0)
        self.assertGreaterEqual(summary.links_skipped, 2)

        source_alias.unlink()
        source_file.unlink()
        self._write("clean.md", "clean database sentinel\n")
        build_index(self.source, self.index)
        index_alias = self.temp.path / "hardlink-index.sqlite"
        os.link(self.index, index_alias)
        with self.assertRaises(IndexQueryError):
            query_index(
                index_alias,
                "clean database",
                top_k=5,
                max_total_bytes=32_768,
            )

    def test_entry_budget_fails_without_replacing_previous_database(self) -> None:
        self._write("preserved.md", "entry budget preserved\n")
        build_index(self.source, self.index)
        self._write("second.md", "second entry\n")

        with self.assertRaises(IndexBuildError):
            build_index(
                self.source,
                self.index,
                BuildLimits(max_entries=1),
            )

        self.assertTrue(
            query_index(
                self.index,
                "entry budget preserved",
                top_k=5,
                max_total_bytes=32_768,
            )
        )


class ProtocolTests(unittest.TestCase):
    def test_query_request_requires_exact_schema_and_integer_bounds(self) -> None:
        parsed = parse_query_request(
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "修复 parser",
                    "top_k": 5,
                    "max_total_bytes": 32_768,
                },
                ensure_ascii=False,
            )
        )
        self.assertEqual(parsed.query, "修复 parser")
        self.assertEqual(parsed.top_k, 5)

        invalid = [
            "not-json",
            "[]",
            json.dumps({"schema_version": 1}),
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "x",
                    "top_k": 5,
                    "max_total_bytes": 32_768,
                    "extra": True,
                }
            ),
            json.dumps(
                {
                    "schema_version": 2,
                    "query": "x",
                    "top_k": 5,
                    "max_total_bytes": 32_768,
                }
            ),
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "",
                    "top_k": 5,
                    "max_total_bytes": 32_768,
                }
            ),
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "x",
                    "top_k": True,
                    "max_total_bytes": 32_768,
                }
            ),
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "x",
                    "top_k": 21,
                    "max_total_bytes": 32_768,
                }
            ),
            json.dumps(
                {
                    "schema_version": 1,
                    "query": "x",
                    "top_k": 5,
                    "max_total_bytes": 32_769,
                }
            ),
        ]
        for value in invalid:
            with self.subTest(value=value):
                with self.assertRaises(ProtocolError):
                    parse_query_request(value)


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = TempTree()
        self.script = RAG_ROOT / "agent_rag_cli.py"

    def tearDown(self) -> None:
        self.temp.close()

    def _run(self, *arguments: str, stdin: str = "") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                "-E",
                "-s",
                "-X",
                "utf8",
                str(self.script),
                *arguments,
            ],
            input=stdin,
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_cli_build_and_query_round_trip(self) -> None:
        source = self.temp.path / "docs"
        source.mkdir()
        (source / "guide.md").write_text(
            "恢复流程使用事件重放。\n", encoding="utf-8"
        )
        index = self.temp.path / ".rag" / "knowledge.sqlite"

        built = self._run("build", "--source", str(source), "--index", str(index))
        queried = self._run(
            "query",
            "--index",
            str(index),
            stdin=json.dumps(
                {
                    "schema_version": 1,
                    "query": "事件恢复",
                    "top_k": 5,
                    "max_total_bytes": 32_768,
                },
                ensure_ascii=False,
            ),
        )

        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertEqual(built.stderr, "")
        self.assertEqual(json.loads(built.stdout)["schema_version"], 1)
        self.assertEqual(queried.returncode, 0, queried.stderr)
        self.assertEqual(queried.stderr, "")
        response = json.loads(queried.stdout)
        self.assertEqual(response["schema_version"], 1)
        self.assertIn("恢复流程", response["items"][0]["content"])

    def test_cli_failures_are_fixed_and_do_not_emit_tracebacks_or_paths(self) -> None:
        missing = self.temp.path / "SENTINEL_PRIVATE_MISSING"
        bad_build = self._run(
            "build",
            "--source",
            str(missing),
            "--index",
            str(self.temp.path / "x.db"),
        )
        bad_query = self._run(
            "query", "--index", str(missing), stdin="SENTINEL_PRIVATE_BAD_JSON"
        )

        self.assertNotEqual(bad_build.returncode, 0)
        self.assertEqual(bad_build.stdout, "")
        self.assertEqual(bad_build.stderr, "rag build failed\n")
        self.assertNotEqual(bad_query.returncode, 0)
        self.assertEqual(bad_query.stdout, "")
        self.assertEqual(bad_query.stderr, "rag query failed\n")
        combined = bad_build.stderr + bad_query.stderr
        self.assertNotIn("Traceback", combined)
        self.assertNotIn("SENTINEL", combined)


if __name__ == "__main__":
    unittest.main()
