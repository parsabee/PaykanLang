#!/usr/bin/env python3
"""Run clang-tidy over PaykanLang's first-party translation units.

This is the authoritative lint entry point shared by CI and the pre-commit
hook. It uses the vendored LLVM 17 clang-tidy (build/third-party/llvm) when
present, falling back to a clang-tidy on PATH otherwise.

Generated sources (Bison Parser.tab / Flex Lexer.yy) and third-party code are
excluded: they are not ours to fix and clang-tidy cannot satisfy its checks on
generated parser tables.

Usage:
    scripts/run_clang_tidy.py [--build-dir DIR] [file ...]

    --build-dir DIR   Directory with compile_commands.json (default: build).
    file ...          Optional explicit files to lint (used by pre-commit).
                      When omitted, every first-party .cpp/.c TU in the compile
                      database is linted.

Exits non-zero if clang-tidy reports any finding (WarningsAsErrors: '*').
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_EXCLUDE_RE = re.compile(
    r"(/build/third-party/|Parser\.tab|Lexer\.yy|Parser\.ypp|Lexer\.lpp)"
)


def is_excluded(path: str) -> bool:
    return bool(_EXCLUDE_RE.search(path))


def find_clang_tidy(build_dir: str) -> str:
    vendored = os.path.join(build_dir, "third-party", "llvm", "bin", "clang-tidy")
    if os.access(vendored, os.X_OK):
        return vendored
    found = shutil.which("clang-tidy")
    if found:
        return found
    sys.exit(
        f"error: no clang-tidy found (looked for {vendored} and PATH)"
    )


def first_party_tus(compile_db: str) -> list[str]:
    with open(compile_db) as fh:
        db = json.load(fh)
    seen: set[str] = set()
    files: list[str] = []
    src_root = os.path.join(REPO_ROOT, "src")
    test_root = os.path.join(REPO_ROOT, "tests")
    for entry in db:
        f = entry["file"]
        if not f.endswith((".cpp", ".c")):
            continue
        if f in seen or is_excluded(f):
            continue
        if f.startswith(src_root) or f.startswith(test_root):
            seen.add(f)
            files.append(f)
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("files", nargs="*")
    args = parser.parse_args()

    build_dir = args.build_dir
    compile_db = os.path.join(build_dir, "compile_commands.json")
    if not os.path.isfile(compile_db):
        sys.exit(
            f"error: {compile_db} not found; configure CMake first "
            f"(cmake -B {build_dir})"
        )

    clang_tidy = find_clang_tidy(build_dir)
    print(f"Using clang-tidy: {clang_tidy}")

    if args.files:
        files = [
            f
            for f in args.files
            if f.endswith((".cpp", ".c")) and not is_excluded(f)
        ]
    else:
        files = first_party_tus(compile_db)

    if not files:
        print("No first-party files to lint.")
        return 0

    print(f"Linting {len(files)} file(s)...")
    status = 0
    for f in files:
        result = subprocess.run([clang_tidy, "-p", build_dir, f])
        if result.returncode != 0:
            status = 1

    if status:
        print("clang-tidy reported findings.", file=sys.stderr)
    else:
        print("clang-tidy: clean.")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
