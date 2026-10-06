#!/usr/bin/env python3
"""Print the test suites a change can affect, for -DPAYKAN_BUILD_TESTS.

    scripts/affected_tests.py --base <commit> [--head <commit>]
    scripts/affected_tests.py --files <path>...
    scripts/affected_tests.py --list

Reads the files changed between <base> and <head> (default HEAD) with
`git diff --name-only`, or takes them from --files, and prints the test
suites those files can affect, as a CMake list (`parser;sema`), or `all`
when every suite is.  An empty line means no suite is affected (a change to
the release workflow, say).  --list prints every suite and what it runs.

The suites and the path rules live in tests/suites.json, which
scripts/gen_test_suites.py generates from the build files and CMake
(cmake/PaykanTests.cmake) reads too.  The rules are deliberately
conservative: a file in a library the whole compiler uses (the AST, the
frontends, the plugin interfaces, the public headers, the build system)
affects every suite, and a file no rule names does too.

CI (.github/workflows/ci.yml) builds only these suites on a pull request,
and every suite on a push.
"""

import argparse
import fnmatch
import json
import os
import subprocess
import sys

SUITES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "tests", "suites.json")
ALL = "all"


def load(path: str = SUITES_FILE):
    """The suites (in order) and the rules, with groups expanded."""
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    suites = data["suites"]
    groups = data.get("groups", {})

    def expand(names, where):
        out = []
        for name in names:
            if name.startswith("@"):
                if name[1:] not in groups:
                    sys.exit(f"error: {path}: unknown group '{name}' in {where}")
                out.extend(expand(groups[name[1:]], f"group '{name}'"))
            elif name in suites:
                out.append(name)
            else:
                sys.exit(f"error: {path}: unknown suite '{name}' in {where}")
        return out

    rules = []
    for pattern, names in data["rules"]:
        if names == ALL:
            rules.append((pattern, ALL))
        else:
            rules.append((pattern, expand(names, f"the rule for '{pattern}'")))
    return suites, rules


def affected(paths, suites, rules):
    selected = set()
    for path in paths:
        hit = next((names for pattern, names in rules
                    if fnmatch.fnmatchcase(path, pattern)), ALL)
        if hit == ALL:
            return ALL  # an unknown path affects everything
        selected.update(hit)
    return [s for s in suites if s in selected]


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
    group.add_argument("--list", action="store_true", help="list the suites")
    parser.add_argument("--head", default="HEAD", help="default: HEAD")
    args = parser.parse_args()

    suites, rules = load()
    if args.list:
        width = max(len(name) for name in suites)
        for name, info in suites.items():
            print(f"{name:<{width}}  {', '.join(info['tests'])}")
        return 0

    paths = args.files if args.files is not None else changed_files(args.base, args.head)
    result = affected(paths, suites, rules)
    print(ALL if result == ALL else ";".join(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
