#!/usr/bin/env python3
"""Print the test suites a change can affect, for -DPAYKAN_BUILD_TESTS.

    scripts/affected_tests.py --base <commit> [--head <commit>]
    scripts/affected_tests.py --files <path>...

Reads the files changed between <base> and <head> (default HEAD) with
`git diff --name-only`, or takes them from --files, and prints the test
suites (cmake/PaykanTests.cmake) those files can affect, as a CMake list
(`parser;sema`), or `all` when every suite is.  An empty line means no
suite is affected (a change to the release workflow, say).

The rules are deliberately conservative: a file in a library the whole
compiler uses (the AST, the frontends, the plugin interfaces, the public
headers, the build system) affects every suite, and a file no rule names
does too.  Everything that runs the `paykan` binary end to end (the
codegen, driver, plugin, samples, c-strict, docs and out-of-tree suites)
depends on every stage of the pipeline.

CI (.github/workflows/ci.yml) builds only these suites on a pull request,
and every suite on a push.
"""

import argparse
import fnmatch
import subprocess
import sys

SUITES = [
    "parser", "sema", "codegen", "pir-llvm", "pir", "lowering",
    "ast-interchange", "plugin", "runtime", "driver", "frontend", "samples",
    "c-strict", "docs", "configure", "out-of-tree",
]

ALL = "all"

# The suites that run the paykan binary (or the whole pipeline) end to end.
E2E = ["codegen", "driver", "plugin", "samples", "c-strict", "docs", "out-of-tree"]

# (pattern, suites): the first pattern a path matches decides.  `*` matches
# across directories (fnmatch), so "src/Sema/*" covers the whole subtree.
RULES = [
    # The test harness itself.
    ("scripts/affected_tests.py", ALL),
    (".github/workflows/ci.yml", ALL),
    ("tests/CMakeLists.txt", ALL),
    ("cmake/PaykanTests.cmake", ALL),
    ("cmake/GTestSetup.cmake", ALL),

    # Tests, by suite.
    ("tests/Parser/*", ["parser"]),
    ("tests/Sema/*", ["sema"]),
    ("tests/CodeGen/*", ["codegen"]),
    ("tests/CodeGenTestUtils.h", ["codegen"]),
    ("tests/Backends/*", ["pir-llvm"]),
    ("tests/PIR/*", ["pir"]),
    ("tests/Lowering/*", ["lowering"]),
    ("tests/AST/*", ["ast-interchange"]),
    ("tests/Plugin/*", ["plugin"]),
    ("tests/Runtime/*", ["runtime"]),
    ("tests/Driver/*", ["driver"]),
    ("tests/Frontend/*", ["frontend", "out-of-tree"]),
    ("tests/OutOfTree/*", ["out-of-tree"]),
    ("tests/DefaultConfigure.cmake", ["configure"]),
    ("tests/*", ALL),  # shared helpers (TestUtils.h, ...)

    # The compiler, by stage.
    ("src/Sema/*", ["sema", "lowering"] + E2E),
    ("src/Lowering/*", ["lowering", "pir-llvm"] + E2E),
    ("src/PIR/*", ["pir", "lowering", "pir-llvm"] + E2E),
    ("src/ASTInterchange/*", ["ast-interchange", "driver", "plugin", "out-of-tree"]),
    ("src/Runtime/*", ["runtime", "pir-llvm"] + E2E),
    ("src/Backends/LLVM/*", ["pir-llvm"] + E2E),
    ("src/Backends/C/*", E2E),
    ("src/Backends/Toolchain/*", E2E),
    ("src/Backend/*", ["pir-llvm"] + E2E),
    ("src/Driver/*", E2E),
    # The example plugins, built against an installation.
    ("src/Backends/PrintPIR/*", ["out-of-tree"]),
    ("src/Frontends/ASTText/*", ["out-of-tree"]),
    # Everything else in src/ and include/ (AST, Diag, the frontends, the
    # plugin interfaces and host, ...) is used by every suite.
    ("src/*", ALL),
    ("include/*", ALL),

    # Inputs of the script-driven suites.
    ("samples/*", ["samples", "c-strict", "docs", "lowering", "ast-interchange",
                   "frontend", "plugin", "out-of-tree"]),
    ("scripts/samples_parity.py", ["samples"]),
    ("scripts/cache_race.py", ["samples"]),
    ("scripts/runtime_lookup.py", ["samples"]),
    ("scripts/c_strict.py", ["c-strict"]),
    ("scripts/check_links.py", ["docs"]),
    ("scripts/doc_examples.py", ["docs"]),
    ("docs/*", ["docs"]),
    ("*.md", ["docs"]),
    ("cmake/PaykanTestSupport.cmake", ["frontend", "out-of-tree"]),

    # Not covered by any test suite (CI's lint and release jobs check them).
    ("scripts/clang_format.py", []),
    ("scripts/run_clang_tidy.py", []),
    ("scripts/coverage.py", []),
    ("scripts/build_apt_repo.sh", []),
    (".github/*", []),
    ("Formula/*", []),
    (".clang-format", []),
    (".clang-tidy", ALL),  # clang-tidy checks the test sources too
    (".pre-commit-config.yaml", []),
    (".gitignore", []),
    ("LICENSE", []),

    # The build system.
    ("cmake/*", ALL),
    ("CMakeLists.txt", ALL),
]


def suites_for(path: str):
    for pattern, suites in RULES:
        if fnmatch.fnmatchcase(path, pattern):
            return suites
    return ALL  # unknown: assume it affects everything


def affected(paths):
    selected = set()
    for path in paths:
        suites = suites_for(path)
        if suites == ALL:
            return ALL
        selected.update(suites)
    return [s for s in SUITES if s in selected]


def changed_files(base: str, head: str):
    out = subprocess.run(
        ["git", "diff", "--name-only", base, head],
        check=True, capture_output=True, text=True,
    ).stdout
    return [line for line in out.splitlines() if line]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--base", help="the commit to diff against")
    group.add_argument("--files", nargs="*", help="the changed files")
    parser.add_argument("--head", default="HEAD", help="default: HEAD")
    args = parser.parse_args()

    paths = args.files if args.files is not None else changed_files(args.base, args.head)
    result = affected(paths)
    print(ALL if result == ALL else ";".join(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
