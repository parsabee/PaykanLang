#!/usr/bin/env python3
"""Differential check of the Paykan frontends.

Runs `paykan --frontend=<name> --dump-ast` with two frontends over every
`.pkn` file below the given directories (default: samples/ and
example_program/) and reports any file for which the two differ in exit
status or in the printed AST.  Both frontends must accept the same inputs,
reject the same inputs and build the same AST (docs/grammar.md); this script
is the CI gate for that rule.

Usage:
    scripts/diff_frontends.py --paykan build/bin/paykan [dir ...]
        [--frontends handwritten,bison]

Exits non-zero when any file differs or a frontend is unavailable.
"""
from __future__ import annotations

import argparse
import difflib
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def dump(paykan: str, frontend: str, path: str) -> tuple[int, str, str]:
    r = subprocess.run(
        [paykan, f"--frontend={frontend}", "--dump-ast", path],
        capture_output=True,
        text=True,
        errors="replace",
    )
    return r.returncode, r.stdout, r.stderr


def collect(dirs: list[str]) -> list[str]:
    files: list[str] = []
    for d in dirs:
        for root, _, names in os.walk(d):
            files.extend(os.path.join(root, n) for n in names if n.endswith(".pkn"))
    return sorted(files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--paykan", required=True, help="path to the paykan binary")
    ap.add_argument(
        "--frontends",
        default="handwritten,bison",
        help="comma-separated pair of frontend names (default: handwritten,bison)",
    )
    ap.add_argument("dirs", nargs="*", help="directories to scan for .pkn files")
    args = ap.parse_args()

    names = [n.strip() for n in args.frontends.split(",") if n.strip()]
    if len(names) != 2:
        sys.exit("error: --frontends needs exactly two names")
    dirs = args.dirs or [
        os.path.join(REPO_ROOT, "samples"),
        os.path.join(REPO_ROOT, "example_program"),
    ]

    listed = subprocess.run(
        [args.paykan, "--list-frontends"], capture_output=True, text=True
    ).stdout
    for n in names:
        if n not in listed.split():
            sys.exit(f"error: frontend '{n}' is not built into {args.paykan} "
                     f"(available: {listed.split()})")

    files = collect(dirs)
    if not files:
        sys.exit("error: no .pkn files found")

    failures = 0
    accepted = rejected = 0
    for path in files:
        a = dump(args.paykan, names[0], path)
        b = dump(args.paykan, names[1], path)
        if a[0] != b[0] or a[1] != b[1]:
            failures += 1
            rel = os.path.relpath(path, REPO_ROOT)
            print(f"DIFF {rel}: {names[0]} exit={a[0]}, {names[1]} exit={b[0]}")
            if a[0] == 0 and b[0] == 0:
                for line in list(difflib.unified_diff(
                        a[1].splitlines(), b[1].splitlines(),
                        fromfile=names[0], tofile=names[1], lineterm=""))[:20]:
                    print("   ", line)
            else:
                for n, r in ((names[0], a), (names[1], b)):
                    first = r[2].strip().splitlines()[:1]
                    print(f"    {n}: {first[0] if first else '(no diagnostic)'}")
            continue
        if a[0] == 0:
            accepted += 1
        else:
            rejected += 1

    print(f"{len(files)} files: {accepted} accepted and {rejected} rejected "
          f"identically by {names[0]} and {names[1]}; {failures} differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
