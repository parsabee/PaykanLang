#!/usr/bin/env python3
"""Produce an LLVM source-based coverage report for PaykanLang.

Prerequisites: configure + build a coverage tree, e.g.

    cmake -B build-cov -DPAYKAN_COVERAGE=ON \\
          -DCMAKE_C_COMPILER=build/third-party/llvm/bin/clang \\
          -DCMAKE_CXX_COMPILER=build/third-party/llvm/bin/clang++
    cmake --build build-cov --parallel

This runs the whole ctest suite instrumented (every process writes a
.profraw), merges the profiles into a .profdata, and prints a per-file
line-coverage report restricted to first-party src/ and include/. It also writes an lcov file for CI services.

llvm-cov's "N functions have mismatched data" warning is expected: it counts
the empty (hash 0) records clang emits for inline functions a translation unit
does not use, whose profile carries the real hash from the units (or
binaries) that do; those functions are counted from the real records.

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
import re
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IGNORE_REGEX = (
    r"(/build/|/build-cov/|/third-party/|Parser\.tab|Lexer\.yy|Parser\.ypp|"
    r"Lexer\.lpp|/tests/|/googletest/|/googlemock/)"
)


def cmake_cache_value(build_dir: str, key: str) -> str:
    try:
        with open(os.path.join(build_dir, "CMakeCache.txt")) as fh:
            for line in fh:
                name, _, value = line.partition("=")
                if name.split(":")[0] == key:
                    return value.strip()
    except OSError:
        pass
    return ""


def find_tool(tool: str, build_dir: str) -> str:
    # The tools must match the compiler's profile format: prefer the ones
    # next to the compiler that built the tree.
    compiler = cmake_cache_value(build_dir, "CMAKE_CXX_COMPILER")
    candidates = [
        os.path.join(os.path.dirname(compiler), tool) if compiler else "",
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


def instrumented_objects(bin_dir: str) -> list[str]:
    """llvm-cov -object arguments for every executable the build put in
    bin/: the test binaries (whatever the enabled plugins add) and the
    `paykan` driver the driver tests and script suites run."""
    objects: list[str] = []
    for name in sorted(os.listdir(bin_dir)):
        path = os.path.join(bin_dir, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            objects += ["-object", path]
    return objects


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

    # The whole ctest suite runs instrumented: the gtest binaries, and the
    # script-driven suites (samples parity, frontend differential) that
    # exercise lowering and the backends through `paykan` subprocesses.
    # %m names each profile after its binary's profile signature, and the
    # processes of one binary merge into it online (under a lock), so the
    # many `paykan` subprocesses neither clobber each other nor the test
    # binary that spawned them.  Programs the C backend builds link the
    # instrumented runtime and write their own profiles too: their runtime
    # counts are real coverage, and their generated code is in no -object
    # below, so it is not reported.
    raw_dir = os.path.join(prof_dir, "raw")
    os.makedirs(raw_dir)
    env = dict(
        os.environ,
        LLVM_PROFILE_FILE=os.path.join(raw_dir, "%m.profraw"),
    )
    log = os.path.join(prof_dir, "ctest.log")
    print("Running the instrumented test suite (ctest)...")
    with open(log, "w") as fh:
        result = subprocess.run(
            [
                "ctest",
                "--test-dir",
                build_dir,
                "--output-on-failure",
                "--parallel",
                str(os.cpu_count() or 2),
            ],
            env=env,
            stdout=fh,
            stderr=subprocess.STDOUT,
        )
    if result.returncode != 0:
        # The test output is kept in the log; show, per failed test, its
        # failures and the end of its output (ctest --output-on-failure
        # prints it after the test's status line) so a CI failure can be
        # diagnosed.
        with open(log, errors="replace") as fh:
            lines = fh.read().splitlines()
        failures: list[tuple[str, list[str]]] = []
        for ln in lines:
            if re.match(r"\s*\d+/\d+ Test\s+#\d+:", ln):
                if "Passed" not in ln:
                    failures.append((ln.strip(), []))
                else:
                    failures.append(("", []))
            elif failures:
                failures[-1][1].append(ln)
        for status, out in failures:
            if not status:
                continue
            failed = [ln for ln in out if ln.startswith("[  FAILED  ]")]
            print(f"--- {status} (failures, then last 80 lines; full log: {log})")
            print("\n".join(failed))
            print("...")
            print("\n".join(out[-80:]))
        print("\n".join(ln for ln in lines if "tests failed" in ln))
        sys.exit("error: ctest failed under coverage")

    objects = instrumented_objects(bin_dir)
    if not objects:
        sys.exit(f"error: no executables found in {bin_dir}")
    raw_files = sorted(
        os.path.join(raw_dir, f)
        for f in os.listdir(raw_dir)
        if f.endswith(".profraw")
    )
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
