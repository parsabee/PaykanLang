#!/usr/bin/env python3
"""Run the standard library's tests (tests/Stdlib) on every enabled backend.

Each tests/Stdlib/*.pkn is a program that imports standard-library modules
(`import ::collections;`).  It is run with `--track-heap` on each backend
named on the command line, with $PAYKAN_STDLIB unset, so `paykan` finds the
standard library the build staged next to it.  A program passes when it
exits 0, prints exactly its `.expected` file and leaves zero live heap
blocks.

Each tests/Stdlib/panics/*.pkn starts with `// expect-panic: <message>`: it
passes when it exits non-zero and its stderr has the line
`paykan: <message>`.

Usage:
    stdlib_tests.py --paykan build/bin/paykan --backend mlir [--backend <plugin>]
    stdlib_tests.py --paykan build/bin/paykan --backend mlir --update

--update rewrites the .expected files from the first backend's output.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests" / "Stdlib"
LIVE_RE = re.compile(r"^  live blocks\s*:\s*(-?\d+)$", re.M)
PANIC_RE = re.compile(r"^// expect-panic: (.*)$", re.M)


def run(paykan, backend, program, workdir):
    env = dict(os.environ)
    env.pop("PAYKAN_STDLIB", None)
    env["PAYKAN_NO_PLUGINS"] = "1"
    cmd = [paykan, f"--backend={backend}", "--track-heap", "--no-module-cache",
           "run", str(program)]
    return subprocess.run(cmd, capture_output=True, text=True, env=env,
                          cwd=workdir)


def check_program(proc, program, update):
    problems = []
    if proc.returncode != 0:
        problems.append(f"exit code {proc.returncode}")
    m = LIVE_RE.search(proc.stderr)
    if m is None:
        problems.append("no heap statistics on stderr")
    elif int(m.group(1)) != 0:
        problems.append(f"{m.group(1)} live heap blocks")
    expected = program.with_suffix(".expected")
    if update and proc.returncode == 0:
        expected.write_text(proc.stdout)
    if not expected.is_file():
        problems.append(f"no {expected.name}")
    elif expected.read_text() != proc.stdout:
        problems.append(f"stdout differs from {expected.name}")
    return problems


def check_panic(proc, message):
    problems = []
    if proc.returncode == 0:
        problems.append("exited 0, expected a panic")
    if f"paykan: {message}" not in proc.stderr.splitlines():
        problems.append(f"stderr has no line 'paykan: {message}'")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--backend", action="append", required=True,
                    help="a backend to run the tests on (repeatable)")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the .expected files")
    args = ap.parse_args()

    programs = sorted(TESTS.glob("*.pkn"))
    panics = sorted((TESTS / "panics").glob("*.pkn"))
    failures = 0
    total = 0
    with tempfile.TemporaryDirectory(prefix="paykan-stdlib-") as workdir:
        for backend in args.backend:
            for program in programs + panics:
                total += 1
                proc = run(args.paykan, backend, program, workdir)
                panic = PANIC_RE.search(program.read_text())
                if panic:
                    problems = check_panic(proc, panic.group(1))
                else:
                    problems = check_program(
                        proc, program, args.update and backend == args.backend[0])
                rel = program.relative_to(ROOT)
                if problems:
                    failures += 1
                    print(f"FAIL {rel} [{backend}]: " + "; ".join(problems))
                    for line in proc.stderr.strip().splitlines()[:20]:
                        print("  " + line)
                else:
                    print(f"ok   {rel} [{backend}]")
    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
