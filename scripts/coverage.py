#!/usr/bin/env python3
"""Produce an LLVM source-based coverage report for PaykanLang.

Prerequisites: a Clang with llvm-profdata / llvm-cov, and a coverage tree.
The vendored LLVM 17 under build/third-party/llvm is downloaded only by a
configure that lists the llvm backend; a plain configure (recursive-descent +
c) does not download it, so pass the full list, e.g.

    cmake -B build -DPAYKAN_BUILD_ALL_TESTS=ON "-DPAYKAN_BACKENDS=llvm;c"
    cmake -B build-cov -DPAYKAN_BUILD_ALL_TESTS=ON -DPAYKAN_COVERAGE=ON \\
          "-DPAYKAN_BACKENDS=llvm;c" \\
          -DCMAKE_C_COMPILER=build/third-party/llvm/bin/clang \\
          -DCMAKE_CXX_COMPILER=build/third-party/llvm/bin/clang++
    cmake --build build-cov --parallel "$(getconf _NPROCESSORS_ONLN)"

This runs every ctest test instrumented (every process it starts, `paykan`
subprocesses included, writes its own .profraw), checks that each test binary
wrote a profile, merges the profiles into a .profdata, and prints a per-file
line-coverage report restricted to first-party src/ and include/. It also
writes an lcov file for CI services.

llvm-cov's "N functions have mismatched data" warning is expected: it counts
the empty (hash 0) records clang emits for inline functions a translation unit
does not use, whose profile carries the real hash from the units (or
binaries) that do; those functions are counted from the real records.

It exits non-zero if total line coverage is below the floor (--min, default 80).

Note on the floor: the project's aspiration is ~100% line coverage. The
first-party total was ~82% when the floor was set and ~90% at the audit for the first release
(some defensive branches are still not exercised). The floor stays below that so
CI fails on a large *regression* without blocking the v0.1 release; ratchet it
up toward 100% as gaps are closed.

Usage:
    scripts/coverage.py [--build-dir DIR] [--min PERCENT]
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IGNORE_REGEX = (
    r"(/build/|/build-cov/|/third-party/|/tests/|/googletest/|/googlemock/)"
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
    """llvm-cov object arguments for every executable the build put in
    bin/: the test binaries (whatever the enabled plugins add) and the
    `paykan` driver the driver tests and script suites run.  The first is
    positional, the rest -object."""
    objects: list[str] = []
    for name in sorted(os.listdir(bin_dir)):
        path = os.path.join(bin_dir, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            objects += [path] if not objects else ["-object", path]
    return objects


class TestResult:
    def __init__(self, name: str, command: list[str], log: str) -> None:
        self.name = name
        self.command = command
        self.log = log
        self.cwd = ""
        self.env: dict[str, str] = {}  # what the test sets on top of ours
        self.raw_dir = ""
        self.raw_listing: list[str] = []
        self.returncode = 0
        self.seconds = 0.0
        self.problem = ""  # why the run does not count, if it does not
        self.profdata = ""


def ctest_tests(build_dir: str) -> list[dict]:
    """The tests ctest would run, with their command and properties."""
    out = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [t for t in json.loads(out)["tests"] if t.get("command")]


GTEST_RAN = re.compile(r"^\[==========\] (\d+) tests? from .* ran\.")


def run_test(
    test: dict, prof_dir: str, bin_dir: str, llvm_profdata: str
) -> TestResult:
    """Run one ctest test as ctest would (command, ENVIRONMENT,
    WORKING_DIRECTORY, TIMEOUT), every process it starts writing its own
    profile into a directory of the test's, and merge them into
    <test>.profdata.

    The profiles are named by process id (%p) rather than merged online by
    module signature (%m): the profile runtime overwrites, with only a
    warning on the process's stderr, a pool file whose signature matches
    but whose layout does not.  Programs the C backend builds link the
    instrumented runtime and write profiles too: their runtime counts are
    real coverage, and their generated code is in no reported object.

    All paths are absolute: the test runs in its WORKING_DIRECTORY, where a
    relative LLVM_PROFILE_FILE would land somewhere else."""
    name = test["name"]
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", name)
    raw_dir = os.path.join(prof_dir, "raw", safe)
    os.makedirs(raw_dir)
    props = {p["name"]: p["value"] for p in test.get("properties", [])}
    command = test["command"]
    result = TestResult(name, command, os.path.join(prof_dir, f"{safe}.log"))
    result.raw_dir = raw_dir
    result.cwd = props.get("WORKING_DIRECTORY") or os.getcwd()
    for assignment in props.get("ENVIRONMENT", []):
        key, _, value = assignment.partition("=")
        result.env[key] = value
    result.env["LLVM_PROFILE_FILE"] = os.path.join(raw_dir, "%p.profraw")
    timeout = props.get("TIMEOUT")
    start = time.monotonic()
    with open(result.log, "w") as fh:
        try:
            proc = subprocess.Popen(
                command,
                cwd=result.cwd,
                env=dict(os.environ, **result.env),
                stdout=fh,
                stderr=subprocess.STDOUT,
            )
        except OSError as err:
            result.returncode = -1
            result.problem = f"could not start: {err}"
            return result
        try:
            result.returncode = proc.wait(timeout=float(timeout) if timeout else None)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            fh.write(f"\n*** timed out after {timeout}s\n")
            result.returncode = -1
    result.seconds = time.monotonic() - start
    if result.returncode != 0:
        result.problem = f"exit code {result.returncode}"
    elif os.path.basename(command[0]).endswith("_tests"):
        # A gtest binary that exits 0 having run nothing (a filter, a wrong
        # binary) would pass silently.
        with open(result.log, errors="replace") as fh:
            ran = [int(m.group(1)) for m in map(GTEST_RAN.match, fh) if m]
        if not ran or ran[-1] == 0:
            result.problem = "the gtest binary ran no tests"
    result.raw_listing = sorted(os.listdir(raw_dir))
    raws = [
        os.path.join(raw_dir, f) for f in result.raw_listing if f.endswith(".profraw")
    ]
    if not raws:
        # Every test runs instrumented code; without a profile llvm-cov
        # would report what only this test runs as never executed.
        result.problem = result.problem or "it wrote no .profraw file"
        return result
    # Merge per test and drop the raw files: a test that runs the driver
    # hundreds of times would otherwise leave a gigabyte behind.
    listing = os.path.join(prof_dir, f"{safe}.inputs")
    with open(listing, "w") as fh:
        fh.write("\n".join(raws) + "\n")
    result.profdata = os.path.join(prof_dir, f"{safe}.profdata")
    subprocess.run(
        [
            llvm_profdata,
            "merge",
            "-sparse",
            f"-input-files={listing}",
            "-o",
            result.profdata,
        ],
        check=True,
    )
    shutil.rmtree(raw_dir)
    return result


def report_problem(r: TestResult) -> None:
    """Everything needed to diagnose a test run that does not count."""
    try:
        with open(r.log, errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        lines = []
    failures = [ln for ln in lines if ln.startswith("[  FAILED  ]")]
    print(f"--- {r.name}: {r.problem} ({r.seconds:.1f}s)")
    print(f"command: {' '.join(r.command)}")
    print(f"cwd:     {r.cwd}")
    for key, value in r.env.items():
        print(f"env:     {key}={value}")
    print(f"raw dir: {r.raw_dir}: {' '.join(r.raw_listing) or '(empty)'}")
    print(f"output (failures, then last 80 lines; full log: {r.log}):")
    print("\n".join(failures))
    print("...")
    print("\n".join(lines[-80:]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-cov")
    parser.add_argument(
        "--min",
        type=float,
        default=float(os.environ.get("PAYKAN_COVERAGE_MIN", "80")),
    )
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)
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

    tests = ctest_tests(build_dir)
    if not tests:
        sys.exit(f"error: ctest lists no tests in {build_dir}")
    print(f"Running the {len(tests)} instrumented ctest test(s)...")
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        results = list(
            pool.map(
                lambda t: run_test(t, prof_dir, bin_dir, llvm_profdata), tests
            )
        )

    for r in results:
        print(f"  {r.name}: {r.seconds:.1f}s{', ' + r.problem if r.problem else ''}")
    bad = [r for r in results if r.problem]
    for r in bad:
        report_problem(r)
    ran = {os.path.basename(r.command[0]) for r in results}
    missing = [
        name
        for name in sorted(os.listdir(bin_dir))
        if name.endswith("_tests") and name not in ran
    ]
    if missing:
        print(f"error: no test runs bin/{', bin/'.join(missing)}", file=sys.stderr)
    if bad or missing:
        names = ", ".join(r.name for r in bad)
        sys.exit(f"error: test run(s) that do not count under coverage: {names}")

    objects = instrumented_objects(bin_dir)
    if not objects:
        sys.exit(f"error: no executables found in {bin_dir}")
    profiles = [r.profdata for r in results if r.profdata]
    if not profiles:
        sys.exit("error: no .profraw files produced")

    merged = os.path.join(prof_dir, "merged.profdata")
    print(f"Merging the profiles of {len(profiles)} test(s)...")
    subprocess.run(
        [llvm_profdata, "merge", "-sparse", *profiles, "-o", merged], check=True
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
