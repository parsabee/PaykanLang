#!/usr/bin/env python3
"""Produce an LLVM source-based coverage report for PaykanLang.

Prerequisites: configure + build a coverage tree, e.g.

    cmake -B build-cov -DPAYKAN_COVERAGE=ON \\
          -DCMAKE_C_COMPILER=build/third-party/llvm/bin/clang \\
          -DCMAKE_CXX_COMPILER=build/third-party/llvm/bin/clang++
    cmake --build build-cov --parallel

This runs the instrumented test binaries (one .profraw per binary), merges
them into a .profdata, and prints a per-file line-coverage report restricted to
first-party src/ and include/. It also writes an lcov file for CI services.

It exits non-zero if total line coverage is below the floor (--min, default 80).

Note on the floor: the project's aspiration is ~100% line coverage. The current
first-party total is ~82% (the driver entry point and some defensive branches
are not yet exercised). The floor is set just below that so CI fails on a
*regression* without blocking the v0.0 release; ratchet it up toward 100% as
gaps are closed.

Usage:
    scripts/coverage.py [--build-dir DIR] [--min PERCENT]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST_BINS = [
    "parser_tests",
    "sema_tests",
    "codegen_tests",
    "runtime_tests",
    "driver_tests",
]

IGNORE_REGEX = (
    r"(/build/|/build-cov/|/third-party/|Parser\.tab|Lexer\.yy|Parser\.ypp|"
    r"Lexer\.lpp|/tests/|/googletest/|/googlemock/)"
)


def find_tool(tool: str, build_dir: str) -> str:
    candidates = [
        os.path.join(build_dir, "third-party", "llvm", "bin", tool),
        os.path.join(REPO_ROOT, "build", "third-party", "llvm", "bin", tool),
    ]
    for cand in candidates:
        if os.access(cand, os.X_OK):
            return cand
    found = shutil.which(tool)
    if found:
        return found
    sys.exit(f"error: {tool} not found")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-cov")
    parser.add_argument(
        "--min",
        type=float,
        default=float(os.environ.get("PAYKAN_COVERAGE_MIN", "80")),
    )
    args = parser.parse_args()

    build_dir = args.build_dir
    bin_dir = os.path.join(build_dir, "bin")
    if not os.path.isdir(bin_dir):
        sys.exit(
            f"error: {bin_dir} not found; build with -DPAYKAN_COVERAGE=ON first"
        )

    llvm_cov = find_tool("llvm-cov", build_dir)
    llvm_profdata = find_tool("llvm-profdata", build_dir)

    prof_dir = os.path.join(build_dir, "coverage")
    if os.path.isdir(prof_dir):
        shutil.rmtree(prof_dir)
    os.makedirs(prof_dir)

    print("Running instrumented test binaries...")
    raw_files: list[str] = []
    objects: list[str] = []
    for name in TEST_BINS:
        binary = os.path.join(bin_dir, name)
        if not os.access(binary, os.X_OK):
            print(f"warning: {binary} missing, skipping", file=sys.stderr)
            continue
        raw = os.path.join(prof_dir, f"{name}.profraw")
        log = os.path.join(prof_dir, f"{name}.log")
        env = dict(os.environ, LLVM_PROFILE_FILE=raw)
        with open(log, "w") as fh:
            result = subprocess.run(
                [binary], env=env, stdout=fh, stderr=subprocess.STDOUT
            )
        if result.returncode != 0:
            # The test output is kept in the log; show the failures and the
            # end of it so a CI failure can be diagnosed.
            with open(log, errors="replace") as fh:
                lines = fh.read().splitlines()
            failed = [ln for ln in lines if ln.startswith("[  FAILED  ]")]
            print(f"--- {name} output (failures, then last 80 lines; full log: {log})")
            print("\n".join(failed))
            print("...")
            print("\n".join(lines[-80:]))
            sys.exit(f"error: {name} exited non-zero under coverage")
        raw_files.append(raw)
        objects += ["-object", binary]

    if not raw_files:
        sys.exit("error: no .profraw files produced")

    merged = os.path.join(prof_dir, "merged.profdata")
    print(f"Merging {len(raw_files)} profile(s)...")
    subprocess.run(
        [llvm_profdata, "merge", "-sparse", *raw_files, "-o", merged], check=True
    )

    print("\n==================== Coverage report (first-party) ====================")
    subprocess.run(
        [
            llvm_cov,
            "report",
            *objects,
            f"-instr-profile={merged}",
            f"-ignore-filename-regex={IGNORE_REGEX}",
        ],
        check=True,
    )

    lcov_path = os.path.join(prof_dir, "coverage.lcov")
    with open(lcov_path, "w") as fh:
        subprocess.run(
            [
                llvm_cov,
                "export",
                *objects,
                f"-instr-profile={merged}",
                f"-ignore-filename-regex={IGNORE_REGEX}",
                "-format=lcov",
            ],
            check=True,
            stdout=fh,
        )
    print(f"lcov written to {lcov_path}")

    summary = subprocess.run(
        [
            llvm_cov,
            "export",
            *objects,
            f"-instr-profile={merged}",
            f"-ignore-filename-regex={IGNORE_REGEX}",
            "-summary-only",
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    total = json.loads(summary)["data"][0]["totals"]["lines"]["percent"]

    print(f"\nTotal line coverage: {total:.2f}% (floor: {args.min}%)")
    if total + 1e-9 < args.min:
        print(
            f"FAIL: line coverage {total:.2f}% is below the {args.min}% floor.",
            file=sys.stderr,
        )
        return 1
    print("PASS: coverage floor met.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
