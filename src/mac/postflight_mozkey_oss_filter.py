#!/usr/bin/env python3
"""Fail-closed OSS-only macOS postinstall filter.

This is applied to *generated* Mozc postinstall during Bazel packaging.
Do not modify upstream templates; never run a package or install anything.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys
import tempfile

# Fixed to the reviewed Mozkey IbG template (src/mac/installer/
# postflight_template.sh). Changes must be reviewed, not guessed.  Every kept
# line names a Mozkey IbG process only: an upstream Mozkey installed side by
# side must never be signalled by this package.
UNSAFE_LINE = (
    b"/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall llama-server > /dev/null 2>&1\n"
)
KEEP_LINES = (
    b"/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbGConverter > /dev/null\n",
    b"/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbGRenderer > /dev/null\n",
    (
        b"/bin/ps -axo pid=,comm= | /usr/bin/awk "
        b"'/\\/Library\\/Input Methods\\/MozkeyIbG\\.app\\/.*\\/mozc_zenz_scorer$/"
        b" { print $1 }' | /usr/bin/xargs /bin/kill > /dev/null 2>&1\n"
    ),
    b"/usr/bin/sudo -u $CONSOLE_USER /usr/bin/killall MozkeyIbG > /dev/null\n",
)
TAIL_MARKER = b"# Mozkey OSS-only postinstall tail."


def transform(base: bytes, tail: bytes) -> bytes:
    if not base.startswith(b"#!/bin/sh\n") or b"\r" in base:
        raise ValueError("UNEXPECTED_GENERATED_POSTFLIGHT_FORMAT")
    lines = base.splitlines(keepends=True)
    if lines.count(UNSAFE_LINE) != 1:
        raise ValueError("UNSAFE_LLAMA_KILLALL_NOT_EXACTLY_ONCE")
    if any(lines.count(line) != 1 for line in KEEP_LINES):
        raise ValueError("EXPECTED_MOZC_PROCESS_LINES_CHANGED")
    if b"\nexit 0\n" in base or TAIL_MARKER in base:
        raise ValueError("UNEXPECTED_PREMATURE_EXIT_OR_DUPLICATE_TAIL")
    if b"/usr/bin/true\n" not in lines:
        raise ValueError("UNEXPECTED_GENERATED_POSTFLIGHT_TERMINATOR")
    if not tail.startswith(TAIL_MARKER) or not tail.endswith(b"\nexit 0\n"):
        raise ValueError("UNEXPECTED_OSS_CLEANUP_TAIL_FORMAT")
    # Detect additional llama-server kills using other syntax instead of
    # silently declaring the postinstall safe.
    result = b"".join(line for line in lines if line != UNSAFE_LINE) + tail
    # A basename-wide scorer kill would also stop an upstream Mozkey scorer.
    if re.search(
        rb"(?m)^[^\n#]*(?:killall|pkill)[ \t]+[^\n]*\bmozc_zenz_scorer\b", result
    ):
        raise ValueError("UNSCOPED_ZENZ_SCORER_STOP_REMAINS")
    if re.search(rb"(?m)^[^\n#]*(?:killall|pkill)[ \t]+[^\n]*\bllama-server\b", result):
        raise ValueError("UNSAFE_LLAMA_SERVER_STOP_REMAINS")
    if result.count(TAIL_MARKER) != 1:
        raise ValueError("MISSING_OR_DUPLICATE_CLEANUP_TAIL")
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True, type=Path)
    ap.add_argument("--tail", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    a = ap.parse_args()
    source = a.input.resolve()
    tail = a.tail.resolve()
    output = a.output.absolute()
    if output.resolve() in (source, tail):
        raise ValueError("REFUSE_OVERWRITE_INPUT")
    result = transform(a.input.read_bytes(), a.tail.read_bytes())
    # Only after all checks pass; the output does not exist on failure.
    fd, tmpname = tempfile.mkstemp(prefix=".postflight_filter_", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(result)
        os.chmod(tmpname, 0o644)
        os.replace(tmpname, output)
    finally:
        if os.path.exists(tmpname):
            os.unlink(tmpname)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as exc:
        print(f"MOZKEY_OSS_POSTFLIGHT_FILTER=BLOCKED {exc}", file=sys.stderr)
        raise SystemExit(1)
