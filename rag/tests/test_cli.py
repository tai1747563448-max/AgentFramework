from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from agent_rag.cli import main


def test_retired_build_command_cannot_publish_an_old_format_index(tmp_path, capsys):
    source = tmp_path / "source"
    source.mkdir()
    (source / "rule.md").write_text("alpha legal rule", encoding="utf-8")
    index = tmp_path / "old.sqlite3"
    assert main(["build", "--source", str(source), "--index", str(index)]) == 2
    assert not index.exists()
    captured = capsys.readouterr()
    assert captured.out == ""
    assert captured.err == "rag command failed\n"


@pytest.mark.parametrize("command", ["query", "unknown"])
def test_unsupported_commands_fail_without_leaking_paths(command, tmp_path, capsys):
    assert main([command, "--index", str(tmp_path / "SENTINEL")]) == 2
    captured = capsys.readouterr()
    assert captured.out == ""
    assert "SENTINEL" not in captured.err
    assert "Traceback" not in captured.err
