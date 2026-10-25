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

    for failure in failures:
        print(f"FAIL: {failure}")
    checked = len(sources) + len(PICK_ALL)
    print(f"{checked - len(failures)}/{checked} paths pick their suites")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
