#!/usr/bin/env python3
"""Check or apply clang-format on PaykanLang's first-party C/C++ sources.

Uses the vendored LLVM 17 clang-format (build/third-party/llvm) when present,
falling back to a clang-format on PATH. Third-party code is excluded.

Usage:
    scripts/clang_format.py [--apply] [file ...]

    --apply     Reformat files in place. Without it, the script only checks
                and prints a unified diff for any file that needs formatting.
    file ...    Optional explicit files (used by pre-commit). When omitted, all
                tracked first-party sources/headers are processed.

In check mode, exits non-zero if any file is not formatted.
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_EXCLUDE_RE = re.compile(r"(/build/|/third-party/)")
_SRC_GLOBS = [
    "src/*.cpp",
    "src/*.c",
    "src/*.h",
    "include/*.h",
    "tests/*.cpp",
    "tests/*.h",
]


def is_excluded(path: str) -> bool:
    return bool(_EXCLUDE_RE.search(path))


def find_clang_format() -> str:
    vendored = os.path.join(
        REPO_ROOT, "build", "third-party", "llvm", "bin", "clang-format"
    )
    if os.access(vendored, os.X_OK):
        return vendored
    found = shutil.which("clang-format")
    if found:
        return found
    sys.exit(f"error: no clang-format found (looked for {vendored} and PATH)")


def tracked_sources() -> list[str]:
    # git ls-files honours .gitignore and only returns tracked files.
    result = subprocess.run(
        ["git", "ls-files", *_SRC_GLOBS],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def select_files(explicit: list[str]) -> list[str]:
    if explicit:
        out = []
        for f in explicit:
            if not f.endswith((".cpp", ".c", ".h", ".hpp")):
                continue
            if is_excluded(f):
                continue
            if os.path.isfile(os.path.join(REPO_ROOT, f)) or os.path.isfile(f):
                out.append(f)
        return out
    return [f for f in tracked_sources() if not is_excluded(f)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()

    clang_format = find_clang_format()
    print(f"Using clang-format: {clang_format}")

    files = select_files(args.files)
    if not files:
        print("No files to process.")
        return 0

    if args.apply:
        subprocess.run([clang_format, "-i", *files], cwd=REPO_ROOT, check=True)
        print(f"Formatted {len(files)} file(s).")
        return 0

    status = 0
    for f in files:
        path = f if os.path.isabs(f) else os.path.join(REPO_ROOT, f)
        with open(path) as fh:
            original = fh.read()
        formatted = subprocess.run(
            [clang_format, f],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        if original != formatted:
            status = 1
            print(f"needs formatting: {f}")
            diff = difflib.unified_diff(
                original.splitlines(keepends=True),
                formatted.splitlines(keepends=True),
                fromfile=f,
                tofile=f"{f} (formatted)",
            )
            sys.stdout.writelines(diff)

    if status:
        print(
            "clang-format check failed. Run: scripts/clang_format.py --apply",
            file=sys.stderr,
        )
    else:
        print("clang-format: clean.")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
