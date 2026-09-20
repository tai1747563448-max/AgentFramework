#!/usr/bin/env python3
"""Repair ninja's msvc dependency-prefix line in a generated rules.ninja.

Why this is needed
------------------
Ninja tracks header dependencies for MSVC by parsing the output of
``cl /showIncludes``. It only recognises the lines that start with the byte
string in the ninja variable ``msvc_deps_prefix``.

CMake normally detects that prefix by compiling a probe file. On a
Chinese-localised MSVC install cl.exe emits the prefix in GBK:

    \\xd7\\xa2\\xd2\\xe2: \\xb0\\xfc\\xba\\xac\\xce\\xc4\\xbc\\xfe:    ("\u6ce8\u610f: \u5305\u542b\u6587\u4ef6: " in CP936)

CMake decodes that probe output as UTF-8, so every byte of the two GBK
characters becomes U+FFFD, and the generated line ends up as

    msvc_deps_prefix = \u6ce8<U+FFFD><U+FFFD>: <U+FFFD>...

which can never match what cl.exe printed. Ninja then records zero header
dependencies for every object file (``ninja -t deps <obj>`` reports
``#deps 0``). The build still "succeeds", but editing any header leaves
every object that includes it stale, so a binary ends up mixing two
different layouts of the same struct. The symptom is heap corruption,
``bad_variant_access`` or a segfault in a test that has nothing to do with
the header that was edited -- which is exactly the "baseline drift" that
keeps getting misdiagnosed as flaky tests.

What this does
--------------
Reads the real prefix back out of cl.exe (so it works on any locale, not
just CP936), then rewrites the ``msvc_deps_prefix`` line in rules.ninja with
those exact bytes. Idempotent: safe to run after every build.

Usage:
    python scripts/patch_ninja_deps_prefix.py <rules.ninja>

Exit codes (the build wrapper acts on these):
    0   the line was already correct - the existing object graph is trustworthy
    2   the line was rewritten - a reconfigure had clobbered it, so any object
        files already on disk were compiled while ninja was tracking nothing
        and must be treated as stale
    1   error (missing file, or cl.exe could not be probed)
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# A uniquely named local header, so exactly one /showIncludes line in the
# probe output can be attributed to a path we know, on any locale.
PROBE_HEADER = "zz_deps_prefix_probe.h"
PROBE_SOURCE = f'#include "{PROBE_HEADER}"\nint main() {{ return 0; }}\n'
PROBE_HEADER_SOURCE = "/* probe */\n"

VS_DEV_SHELL = (
    r"E:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    r"\VC\Auxiliary\Build\vcvars64.bat"
)


def probe_prefix() -> bytes:
    """Return the exact byte prefix cl.exe puts in front of an included path."""
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "probe.cpp"
        source.write_text(PROBE_SOURCE, encoding="ascii")
        (Path(tmp) / PROBE_HEADER).write_text(PROBE_HEADER_SOURCE, encoding="ascii")
        # Go through a batch file: `cmd /c call "C:\...\vcvars64.bat" && cl ...`
        # mis-parses the quoted path, and the environment vcvars sets up is
        # what gives cl.exe its INCLUDE/LIB paths.
        driver = Path(tmp) / "probe.cmd"
        driver.write_text(
            "@echo off\r\n"
            f'call "{VS_DEV_SHELL}" >nul\r\n'
            f'cl /nologo /showIncludes /c /Fo"{tmp}\\probe.obj" "{source}"\r\n',
            encoding="ascii",
        )
        completed = subprocess.run(
            ["cmd.exe", "/c", str(driver)],
            capture_output=True,
            check=False,
            cwd=tmp,
        )
    output = completed.stdout.replace(b"\r\n", b"\n") + b"\n"
    output += completed.stderr.replace(b"\r\n", b"\n")
    marker = PROBE_HEADER.encode("ascii")
    for raw_line in output.split(b"\n"):
        index = raw_line.find(marker)
        if index <= 0:
            continue
        # The line is "<prefix><padding><path to the probe header>". The
        # padding is what cl right-aligns to the include depth, so the
        # prefix ends at the last ": " before the path.
        head = raw_line[:index]
        cut = head.rfind(b": ")
        if cut < 0:
            continue
        return head[:cut + 2]
    raise SystemExit(
        "could not read the /showIncludes prefix out of cl.exe; "
        f"probe output was:\n{output.decode('utf-8', 'replace')}"
    )


def patch(rules_path: Path, prefix: bytes) -> bool:
    """Rewrite msvc_deps_prefix. Returns True when the file changed."""
    data = rules_path.read_bytes()
    replacement = b"msvc_deps_prefix = " + prefix + b"\n"
    pattern = re.compile(rb"(?m)^msvc_deps_prefix[^\n]*\n")
    if not pattern.search(data):
        raise SystemExit(f"{rules_path}: no msvc_deps_prefix line found")
    updated, count = pattern.subn(replacement, data, count=1)
    if count != 1:
        raise SystemExit(f"{rules_path}: expected exactly one prefix line")
    if updated == data:
        return False
    rules_path.write_bytes(updated)
    return True


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    rules_path = Path(argv[1])
    if not rules_path.is_file():
        print(f"{rules_path}: not found", file=sys.stderr)
        return 1

    prefix = probe_prefix()
    changed = patch(rules_path, prefix)
    printable = prefix.decode("cp936", "replace")
    print(
        f"msvc_deps_prefix {'rewritten' if changed else 'already correct'}"
        f" ({printable!r}, {len(prefix)} bytes) in {rules_path}"
    )
    return 2 if changed else 0


if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent.parent)
    raise SystemExit(main(sys.argv))
