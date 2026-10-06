#!/usr/bin/env python3
"""Generate tests/suites.json from the build files.

    scripts/gen_test_suites.py            write tests/suites.json
    scripts/gen_test_suites.py --check    fail if it is out of date

tests/suites.json lists the test suites and the rules that map a changed file
to the suites it can affect.  It is generated, never edited, from:

  * tests/CMakeLists.txt: every `paykan_test_suite(<name> <var>)` starts a
    suite's section (the `if()` block after it).  The section's add_test()s
    and paykan_add_python_test()s give the suite's tests; gtest_main (or a
    GoogleTest source dir) in it marks a GoogleTest suite; the files it
    names (test sources, -P scripts, scripts/*.py) and the headers those
    sources include are the suite's own files.
  * src/**/CMakeLists.txt: add_library()/add_executable() say which
    directory defines which target, and target_link_libraries() and
    paykan_link_plugins() give the link graph.  A suite depends on the
    targets its section links (transitively); one that runs `paykan`
    ($<TARGET_FILE:paykan>), or installs the tree, depends on the whole
    compiler and the runtime its programs link.  A change in a src/
    directory affects the suites that depend on a target it defines.
  * the scripts a suite runs: one that reads samples/ (or Markdown files)
    depends on samples/ (or the docs); a -P script that configures a src/
    directory depends on it, and one that installs the tree depends on what
    cmake/PaykanTestSupport.cmake installs.

The rules are conservative: the build system, the public headers, a src/
or tests/ file no target or suite claims, and any path no rule names affect
every suite.  cmake/PaykanTests.cmake reads the suites; scripts/
affected_tests.py reads the rules.  The pre-commit and pre-push hooks
(.pre-commit-config.yaml) regenerate the file, and CI checks it is current.
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "tests", "suites.json")
ALL = "all"

# Files that change how every suite is selected, built or run.
EVERY_SUITE = [
    "scripts/gen_test_suites.py", "scripts/affected_tests.py",
    "tests/suites.json", "tests/CMakeLists.txt",
    ".github/workflows/ci.yml", ".clang-tidy",
]
# Files no test suite builds or runs (CI's lint and release jobs check them).
NO_SUITE = [
    "scripts/*", ".github/*", "Formula/*", ".clang-format",
    ".pre-commit-config.yaml", ".gitignore", "LICENSE",
]

# --- A small CMake reader ----------------------------------------------------

_COMMENT = re.compile(r"(^|\s)#(?!\[).*$", re.M)


def commands(path):
    """The (name, args) commands of a CMake file, in order."""
    with open(path, encoding="utf-8") as fh:
        text = _COMMENT.sub(r"\1", fh.read())
    out, i, n = [], 0, len(text)
    ident = re.compile(r"[A-Za-z_]\w*")
    while i < n:
        m = ident.match(text, i)
        if not m:
            i += 1
            continue
        j = m.end()
        while j < n and text[j] in " \t":
            j += 1
        if j >= n or text[j] != "(" or (m.start() > 0 and text[m.start() - 1] not in " \t\n("):
            i = m.end()
            continue
        depth, k, quote = 0, j, False
        while k < n:
            c = text[k]
            if c == '"' and text[k - 1] != "\\":
                quote = not quote
            elif not quote and c == "(":
                depth += 1
            elif not quote and c == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out.append((m.group(0).lower(), text[j + 1:k]))
        i = k + 1
    return out


def words(args):
    return re.findall(r'"[^"]*"|\[=*\[.*?\]=*\]|[^\s"]+', args, re.S)


# --- The target graph (src/) -------------------------------------------------

def target_graph():
    defined, edges, plugins = {}, {}, []
    for dirpath, dirs, files in sorted(os.walk(os.path.join(ROOT, "src"))):
        dirs.sort()
        if "CMakeLists.txt" not in files:
            continue
        rel = os.path.relpath(dirpath, ROOT).replace(os.sep, "/")
        for name, args in commands(os.path.join(dirpath, "CMakeLists.txt")):
            w = words(args)
            if not w:
                continue
            if name in ("add_library", "add_executable") or name.startswith("paykan_add_") and name.endswith("_plugin"):
                defined.setdefault(w[0], rel)
            # Links only: add_dependencies() and $<TARGET_FILE:...> order
            # the build but put no code into the target.
            if name == "target_link_libraries":
                edges.setdefault(w[0], set()).update(re.findall(r"\bpaykan\w*", " ".join(w[1:])))
            if name == "paykan_add_plugin":
                plugins.append(w[0])
            if name == "paykan_link_plugins":
                edges.setdefault(w[0], set()).add("@plugins")
    for deps in edges.values():
        if "@plugins" in deps:
            deps.discard("@plugins")
            deps.update(plugins)
    return defined, edges, plugins


def closure(roots, edges):
    seen, todo = set(), list(roots)
    while todo:
        t = todo.pop()
        if t in seen:
            continue
        seen.add(t)
        todo.extend(edges.get(t, ()))
    return seen


# --- The suites (tests/CMakeLists.txt) ---------------------------------------

def sections():
    """[(suite, [commands])]: each paykan_test_suite() and its if() block."""
    cmds = commands(os.path.join(ROOT, "tests", "CMakeLists.txt"))
    out, i = [], 0
    while i < len(cmds):
        name, args = cmds[i]
        if name != "paykan_test_suite":
            i += 1
            continue
        suite = words(args)[0]
        j, depth, body = i + 1, 0, []
        while j < len(cmds):
            body.append(cmds[j])
            if cmds[j][0] == "if":
                depth += 1
            elif cmds[j][0] == "endif":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((suite, body))
        i = j + 1
    return out


def resolve(token, base):
    token = token.strip('"')
    for var, root in (("${CMAKE_CURRENT_SOURCE_DIR}/", base), ("${CMAKE_SOURCE_DIR}/", ""),
                      ("${PROJECT_SOURCE_DIR}/", ""), ("${PAYKAN_SOURCE_DIR}/", "")):
        if token.startswith(var):
            return (root + "/" if root else "") + token[len(var):]
    if token.startswith("$") or token.startswith("/"):
        return None
    return base + "/" + token


def reads(path, *needles):
    try:
        with open(os.path.join(ROOT, path), encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return False
    return any(n in text for n in needles)


def includes(path):
    """The tests/ headers a test source includes."""
    out = set()
    try:
        with open(os.path.join(ROOT, path), encoding="utf-8") as fh:
            for inc in re.findall(r'#include\s+"([^"]+)"', fh.read()):
                for cand in (os.path.join(os.path.dirname(path), inc), os.path.join("tests", inc)):
                    if os.path.isfile(os.path.join(ROOT, cand)):
                        out.add(os.path.normpath(cand).replace(os.sep, "/"))
    except OSError:
        pass
    return out


def suite_info(body, defined, edges, plugins):
    tests, files, targets, inputs = [], set(), set(), set()
    gtest, runs_compiler = False, False
    for name, args in body:
        w = words(args)
        if name == "add_test" and "NAME" in w:
            tests.append(w[w.index("NAME") + 1])
        if name == "paykan_add_python_test" and len(w) > 1:
            tests.append(w[0])
            files.add("scripts/" + w[1])
        if re.search(r"\bgtest_main\b|googletest_SOURCE_DIR", args):
            gtest = True
        if name == "target_link_libraries":
            targets.update(t for t in re.findall(r"\bpaykan\w*", args) if t in defined)
        if name == "paykan_link_plugins":
            targets.update(plugins)
        if re.search(r"TARGET_FILE:paykan>", args):
            runs_compiler = True
        if "PAYKAN_SAMPLES_DIR" in args or "/samples" in args:
            inputs.add("samples/*")
        for tok in w:
            if not re.search(r"\.(cpp|c|h|cmake)\"?$", tok):
                continue
            path = resolve(tok, "tests")
            if not path:
                continue
            if "${" in path:  # Plugin/modules/${source}: the directory
                path = path.split("${")[0].rstrip("/") + "/*"
                if os.path.isdir(os.path.join(ROOT, path[:-2])):
                    files.add(path)
            elif os.path.isfile(os.path.join(ROOT, path)):
                files.add(path)
    installs = False
    for f in sorted(files):
        if f.endswith((".cpp", ".c", ".h")):
            files.update(includes(f))
        elif f.endswith(".py"):
            if reads(f, "samples"):
                inputs.add("samples/*")
            if reads(f, ".md"):
                inputs.update(["docs/*", "*.md"])
        elif f.endswith(".cmake"):
            text = open(os.path.join(ROOT, f), encoding="utf-8").read()
            if "--install" in text:
                installs = runs_compiler = True
            if "/samples/" in text:
                inputs.add("samples/*")
            for d in re.findall(r"\$\{PAYKAN_SOURCE_DIR\}/(src/[\w/]+)", text):
                if os.path.isdir(os.path.join(ROOT, d)):
                    inputs.add(d.rstrip("/") + "/*")
    if installs:
        # What `cmake --install` puts in the tree from the sources.
        support = "cmake/PaykanTestSupport.cmake"
        inputs.add(support)
        text = open(os.path.join(ROOT, support), encoding="utf-8").read()
        for path in re.findall(r"\$\{PROJECT_SOURCE_DIR\}/([\w./]+)", text):
            path = path.rstrip("/")
            if os.path.isdir(os.path.join(ROOT, path)):
                inputs.add(path + "/*")
            elif os.path.isfile(os.path.join(ROOT, path)):
                inputs.add(path)
    if runs_compiler:
        targets.update(["paykan", "paykan_runtime"])
    return {
        "gtest": gtest,
        "tests": [t.replace("${fe}", "<frontend>").replace("${be}", "<backend>") for t in tests],
        "files": files, "inputs": inputs,
        "depends": closure(targets, edges),
    }


def own_pattern(path):
    """tests/Parser/ArithTests.cpp -> tests/Parser/*; tests/X.cmake stays."""
    parts = path.split("/")
    if parts[0] == "tests" and len(parts) > 2:
        return "/".join(parts[:2]) + "/*"
    return path


# --- Putting it together -----------------------------------------------------

def generate():
    defined, edges, plugins = target_graph()
    infos, order = {}, []
    for suite, body in sections():
        info = suite_info(body, defined, edges, plugins)
        if suite in infos:  # a suite with several sections
            prev = infos[suite]
            prev["gtest"] |= info["gtest"]
            prev["tests"] += info["tests"]
            for key in ("files", "inputs", "depends"):
                prev[key] |= info[key]
        else:
            infos[suite] = info
            order.append(suite)

    def ordered(names):
        return [s for s in order if s in names]

    by_path = {}
    for suite in order:
        info = infos[suite]
        for f in info["files"] | info["inputs"]:
            if f.startswith("tests/"):
                pat = own_pattern(f)
            elif f.startswith("src/") and not f.endswith("/*"):
                pat = os.path.dirname(f) + "/*"  # merges with its directory's rule
            else:
                pat = f
            by_path.setdefault(pat, set()).add(suite)
    dirs = {}
    for target, d in defined.items():
        dirs.setdefault(d, set()).add(target)
    for d, targets in dirs.items():
        users = {s for s in order if infos[s]["depends"] & targets}
        by_path.setdefault(d + "/*", set()).update(users)

    def specificity(pattern):
        return (-pattern.count("/"), "*" in pattern, pattern)

    rules = [[p, ALL] for p in EVERY_SUITE]
    for pattern in sorted(by_path, key=specificity):
        if pattern in EVERY_SUITE:
            continue
        rules.append([pattern, ordered(by_path[pattern])])
    rules += [["tests/*", ALL], ["src/*", ALL], ["include/*", ALL]]
    rules += [[p, []] for p in NO_SUITE]
    rules += [["cmake/*", ALL], ["CMakeLists.txt", ALL]]

    suites = {}
    for suite in order:
        info = infos[suite]
        suites[suite] = {"gtest": info["gtest"], "tests": info["tests"]}
    return {
        "_comment": "Generated by scripts/gen_test_suites.py from the build files: "
                    "do not edit. The pre-commit hook regenerates it.",
        "suites": suites,
        "rules": rules,
    }


def render(data):
    lines = ["{", f'  "_comment": {json.dumps(data["_comment"])},', '  "suites": {']
    items = list(data["suites"].items())
    for i, (name, info) in enumerate(items):
        comma = "," if i + 1 < len(items) else ""
        lines.append(f"    {json.dumps(name)}: {json.dumps(info)}{comma}")
    lines += ["  },", '  "rules": [']
    for i, rule in enumerate(data["rules"]):
        comma = "," if i + 1 < len(data["rules"]) else ""
        lines.append(f"    {json.dumps(rule)}{comma}")
    lines += ["  ]", "}", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="fail if tests/suites.json is not up to date")
    args = parser.parse_args()
    text = render(generate())
    try:
        with open(OUT, encoding="utf-8") as fh:
            current = fh.read()
    except OSError:
        current = None
    if current == text:
        return 0
    if args.check:
        print("error: tests/suites.json is out of date; run scripts/gen_test_suites.py",
              file=sys.stderr)
        return 1
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("tests/suites.json updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
