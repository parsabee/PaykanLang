#!/usr/bin/env python3
"""Run clang-tidy over PaykanLang's first-party translation units.

This is the authoritative lint entry point shared by CI and the pre-commit
hook. It uses the vendored LLVM 17 clang-tidy (build/third-party/llvm) when
present, falling back to a clang-tidy on PATH otherwise.

Third-party code is excluded: it is not ours to fix.

Usage:
    scripts/run_clang_tidy.py [--build-dir DIR] [file ...]

    --build-dir DIR   Directory with compile_commands.json (default: build).
    file ...          Optional explicit files to lint (used by pre-commit).
                      When omitted, every first-party .cpp/.c TU in the compile
                      database is linted.

On macOS the vendored clang-tidy is given the SDK from `xcrun --show-sdk-path`
and the libc++ headers bundled with the vendored LLVM.  If the default SDK is
newer than that clang supports, point SDKROOT at an older installed one, e.g.
    SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk \
        scripts/run_clang_tidy.py

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
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

_EXCLUDE_RE = re.compile(r"/build/third-party/")


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


# Options a GCC-configured tree records in the compile database (LLVM's
# HandleLLVMOptions adds them for GCC) that clang rejects: an unknown argument
# is a hard error, and an unknown -W option is one under -Werror.  Either
# failed every C++ translation unit and, being a compiler error, also kept the
# static analyzer from running on it.
_GCC_ONLY_FLAGS = ("-fno-lifetime-dse", "-Wno-class-memaccess")
_GCC_ONLY_RE = re.compile(
    r"(?<=\s)(?:" + "|".join(map(re.escape, _GCC_ONLY_FLAGS)) + r")(?=\s|$)"
)


def clang_compile_db_dir(build_dir: str, scratch: str) -> str:
    """The directory of a compile database clang-tidy accepts: build_dir
    itself, or, for a GCC-configured tree, a copy in scratch without the
    GCC-only options."""
    with open(os.path.join(build_dir, "compile_commands.json")) as fh:
        db = json.load(fh)
    changed = False
    for entry in db:
        if "arguments" in entry:
            kept = [a for a in entry["arguments"] if a not in _GCC_ONLY_FLAGS]
            changed |= len(kept) != len(entry["arguments"])
            entry["arguments"] = kept
        else:
            command = _GCC_ONLY_RE.sub("", entry["command"])
            changed |= command != entry["command"]
            entry["command"] = command
    if not changed:
        return build_dir
    with open(os.path.join(scratch, "compile_commands.json"), "w") as fh:
        json.dump(db, fh)
    return scratch


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
            f"(cmake -B {build_dir} -DPAYKAN_BUILD_TESTS=ON)"
        )

    # Absolute paths throughout: clang-tidy runs each TU from the directory
    # recorded in the compile database (the build dir), so a relative
    # -isystem/-p would be resolved from there, not from the repository root.
    build_dir = os.path.abspath(build_dir)
    clang_tidy = os.path.abspath(find_clang_tidy(build_dir))
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

    # The vendored clang-tidy is a plain LLVM build: unlike Apple's clang it
    # does not locate the macOS SDK on its own, and CMake's compile commands
    # do not always carry -isysroot (Xcode toolchains on the CI runners do
    # not), so every <cassert>/<stdio.h> came back "file not found".  Hand it
    # the SDK explicitly (a later -isysroot overrides any earlier one), and
    # use the libc++ headers bundled with the vendored LLVM rather than the
    # SDK's: a newer SDK's libc++ relies on builtins this clang lacks.  The C
    # headers still come from the SDK, which must itself be one this clang
    # understands -- set SDKROOT to an older installed SDK if the default one
    # is too new (xcrun honours it).
    # Report findings in our own headers only.  .clang-tidy's
    # HeaderFilterRegex matches any ".../src/..." path, which also catches
    # headers generated under a build tree; anchoring the filter at the
    # repository root keeps those out of the gate.
    header_filter = "^" + re.escape(REPO_ROOT) + r"/(src|include)/.*\.h$"
    extra_args: list[str] = [f"--header-filter={header_filter}"]
    if sys.platform == "darwin":
        sdk = subprocess.run(
            ["xcrun", "--show-sdk-path"], capture_output=True, text=True
        )
        if sdk.returncode == 0 and sdk.stdout.strip():
            extra_args.append(f"--extra-arg=-isysroot{sdk.stdout.strip()}")
        bundled_libcxx = os.path.join(
            os.path.dirname(os.path.dirname(clang_tidy)), "include", "c++", "v1"
        )
        if os.path.isdir(bundled_libcxx):
            extra_args += [
                "--extra-arg=-nostdinc++",
                f"--extra-arg=-isystem{bundled_libcxx}",
            ]

    print(f"Linting {len(files)} file(s)...")
    status = 0
    with tempfile.TemporaryDirectory(prefix="paykan-tidy-") as scratch:
        db_dir = clang_compile_db_dir(build_dir, scratch)
        for f in files:
            result = subprocess.run([clang_tidy, "-p", db_dir, *extra_args, f])
            if result.returncode != 0:
                status = 1

    if status:
        print("clang-tidy reported findings.", file=sys.stderr)
    else:
        print("clang-tidy: clean.")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
