#!/usr/bin/env python3
"""Check the test suites scripts/affected_tests.py picks for a change.

CI builds and runs only the suites a pull request's changes can affect
(scripts/affected_tests.py, with the rules in tests/suites.json), so a rule
that picks too few lets a change break a suite that CI never runs.  This
runs the script as CI does, one changed path at a time, on paths whose
suites are known:

* each standard library source (stdlib/*.pkn) picks every suite whose
  programs import the copy the build stages: sema, lowering, pir-mlir and
  every suite of the end-to-end group;
* each file of a sample a driver test reads (DriverTests.cpp's
  RunLoadsAnArchiveOfNativeObjects builds samples/imports/14_native and
  checks what it prints) picks the driver suite;
* what LetDecls depends on besides the docs, samples and standard library
  picks the docs suite: scripts/let_check.py, the programs of tests/Stdlib
  it compiles the standard library with, and the AST interchange writer,
  whose `--emit-ast` it reads;
* the standard library's build (stdlib/CMakeLists.txt), the selection's own
  script and rules, and a path no rule names pick every suite.

A pick may be wider than what is checked (`all` always passes).

Usage:
    test_selection.py
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "affected_tests.py"
SUITES = ROOT / "tests" / "suites.json"

# The suites whose programs import the standard library the build stages
# (each fails with an empty one), besides those of the end-to-end group.
IMPORT_THE_STDLIB = ["sema", "lowering", "pir-mlir", "codegen", "driver",
                     "plugin", "samples", "stdlib", "c-strict", "docs",
                     "out-of-tree"]

PICK_ALL = ["stdlib/CMakeLists.txt", "scripts/affected_tests.py",
            "tests/suites.json", "no-rule-names-this/file.txt"]

# The samples the driver tests read (tests/Driver/DriverTests.cpp).
DRIVER_SAMPLES = ["samples/imports/14_native"]

# What scripts/let_check.py (LetDecls, the docs suite) reads or runs.
LET_CHECK_INPUTS = ["scripts/let_check.py", "tests/Stdlib/list_test.pkn",
                    "src/ASTInterchange/Writer.cpp"]


def picked(path):
    """What affected_tests.py prints for a change to @p path alone."""
    result = subprocess.run(
        [sys.executable, str(SCRIPT), "--files", path],
        capture_output=True, text=True, cwd=ROOT, check=False)
    if result.returncode != 0:
        sys.exit(f"error: affected_tests.py --files {path} exited "
                 f"{result.returncode}:\n{result.stderr}")
    return result.stdout.strip()


def main():
    with open(SUITES, encoding="utf-8") as fh:
        data = json.load(fh)
    enabled = {name for name, info in data["suites"].items()
               if not info.get("disabled", False)}
    required = [name for name in IMPORT_THE_STDLIB + data["groups"]["e2e"]
                if name in enabled]

    failures = []
    sources = sorted(p.relative_to(ROOT).as_posix()
                     for p in (ROOT / "stdlib").glob("*.pkn"))
    if not sources:
        failures.append("no stdlib/*.pkn to check")
    for path in sources:
        got = picked(path)
        if got == "all":
            continue
        missing = sorted(set(required) - set(got.split(";")))
        if missing:
            failures.append(f"{path} picks '{got}', without {', '.join(missing)}")
    for path in PICK_ALL:
        got = picked(path)
        if got != "all":
            failures.append(f"{path} picks '{got}', not all")
    driver_files = []
    for sample in DRIVER_SAMPLES:
        files = sorted(p.relative_to(ROOT).as_posix()
                       for p in (ROOT / sample).iterdir() if p.is_file())
        if not files:
            failures.append(f"no files in {sample} to check")
        driver_files += files
    for path in driver_files:
        got = picked(path)
        if got != "all" and "driver" not in got.split(";"):
            failures.append(f"{path} picks '{got}', without driver")
    for path in LET_CHECK_INPUTS:
        if not (ROOT / path).is_file():
            failures.append(f"no {path} to check")
            continue
        got = picked(path)
        if got != "all" and "docs" not in got.split(";"):
            failures.append(f"{path} picks '{got}', without docs")

    for failure in failures:
        print(f"FAIL: {failure}")
    checked = len(sources) + len(PICK_ALL) + len(driver_files) + \
        len(LET_CHECK_INPUTS)
    print(f"{checked - len(failures)}/{checked} paths pick their suites")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
