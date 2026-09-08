from __future__ import annotations

import json
import getpass
from pathlib import Path
import subprocess
import sys
import time


def write_line(value: str) -> None:
    sys.stdout.buffer.write(value.encode("utf-8") + b"\n")
    sys.stdout.buffer.flush()


def ready(arguments: list[str]) -> None:
    write_line(
        json.dumps(
            {"op": "ready", "arguments": arguments, "pid": str(Path.cwd())},
            ensure_ascii=False,
            separators=(",", ":"),
        )
    )


def echo_loop(*, delay_ms: int = 0, stderr_bytes: int = 0, oversized: int = 0) -> int:
    for raw in sys.stdin.buffer:
        if delay_ms:
            time.sleep(delay_ms / 1000.0)
        if stderr_bytes:
            sys.stderr.buffer.write(b"E" * stderr_bytes)
            sys.stderr.buffer.flush()
        if oversized:
            write_line("x" * oversized)
        else:
            sys.stdout.buffer.write(raw.rstrip(b"\r\n") + b"\n")
            sys.stdout.buffer.flush()
    return 0


def main(arguments: list[str]) -> int:
    if not arguments:
        return 64
    mode = arguments[0]
    if mode == "delayed-ready" and len(arguments) == 2:
        time.sleep(int(arguments[1]) / 1000.0)
        ready(arguments[2:])
        return echo_loop()
    if mode == "tree-parent" and len(arguments) == 4:
        ready_marker, survival_marker, delay_ms = arguments[1:]
        script = (
            "from pathlib import Path; import sys,time; "
            "Path(sys.argv[1]).write_text('ready'); "
            "time.sleep(int(sys.argv[3])/1000); "
            "Path(sys.argv[2]).write_text('alive')"
        )
        subprocess.Popen(
            [sys.executable, "-c", script, ready_marker, survival_marker, delay_ms],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        ready(arguments[1:])
        return echo_loop()
    if mode == "getpass":
        try:
            getpass.getuser()
        except Exception:
            return 66
        ready([])
        return echo_loop()
    ready(arguments[1:])
    if mode == "echo":
        return echo_loop()
    if mode == "query-delay" and len(arguments) == 2:
        return echo_loop(delay_ms=int(arguments[1]))
    if mode == "stderr-spam" and len(arguments) == 2:
        return echo_loop(stderr_bytes=int(arguments[1]))
    if mode == "progress":
        progress = {
            "schema_version": 1,
            "type": "progress",
            "phase": "sidecar-model",
            "mode": "sidecar",
            "status": "running",
            "completed": 1,
            "total": 2,
            "percent": 50.0,
            "elapsed_seconds": 3.0,
            "throughput_items_per_second": 0.333,
            "eta_seconds": 3.0,
        }
        sys.stderr.write(json.dumps(progress, separators=(",", ":")) + "\n")
        progress["secret"] = "SENTINEL"
        sys.stderr.write(json.dumps(progress, separators=(",", ":")) + "\n")
        sys.stderr.flush()
        return echo_loop()
    if mode == "invalid-progress":
        progress = {
            "schema_version": 1,
            "type": "progress",
            "phase": "sidecar-model",
            "mode": "sidecar",
            "status": "completed",
            "completed": 1,
            "total": 2,
            "percent": 73.0,
            "elapsed_seconds": 3.0,
            "throughput_items_per_second": 0.333,
            "eta_seconds": 3.0,
        }
        sys.stderr.write(json.dumps(progress, separators=(",", ":")) + "\n")
        sys.stderr.flush()
        return echo_loop()
    if mode == "oversized-progress-prefix":
        progress = {
            "schema_version": 1,
            "type": "progress",
            "phase": "sidecar-model",
            "mode": "sidecar",
            "status": "running",
            "completed": 1,
            "total": 2,
            "percent": 50.0,
            "elapsed_seconds": 3.0,
            "throughput_items_per_second": 0.333,
            "eta_seconds": 3.0,
        }
        sys.stderr.write("x" * 2049 + json.dumps(progress, separators=(",", ":")) + "\n")
        sys.stderr.flush()
        return echo_loop()
    if mode == "oversized" and len(arguments) == 2:
        return echo_loop(oversized=int(arguments[1]))
    if mode == "unexpected-exit":
        return 7
    return 65


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
