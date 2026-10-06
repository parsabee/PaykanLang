#!/usr/bin/env python3
"""Run the documentation's examples that state their output.

An example is a fenced ```pkn block that is a whole program (it declares
`fn main`) and is followed, after at most blank lines, by a line reading
`Output:` (or `Output (<note>):`) and a fenced block holding exactly what the
program prints on stdout:

    ```pkn
    fn main() -> int { println("hi"); return 0; }
    ```

    Output:

    ```
    hi
    ```

Each such example is written to a temporary directory as `main.pkn`, run with
`paykan --track-heap` on every backend named on the command line, and must
exit with status 0, print exactly the stated output (`Foo@0x...` addresses
normalised) and leave zero live heap blocks.  By default the program gets no
arguments and an empty standard input.  Two optional additions change that:

- `Output (arguments: a b c):` passes `a`, `b` and `c` (split on whitespace)
  after the source file, so `main(args: Str[])` sees them as `args[1..]`;
- an `Input:` line and a fenced block between the program and its `Output:`
  is fed to the program's standard input.

Two more kinds of block make multi-file and error examples testable:

- A module: a ```pkn block whose first line is `// file: <path>.pkn` (and that
  has no other `// file:` line) is written to `<path>.pkn` in the temporary
  directory of the next example of the same Markdown file, so that example
  can `import` it.  Modules accumulate until an example uses them.
- An error example: a program followed by `Error:` (instead of `Output:`) and
  a fenced block.  `paykan --check-only` must reject it, and every non-empty
  line of the block must appear in its diagnostics.
- An excerpt: a ```pkn block whose first line is `// from: <path>` quotes a
  file of the repository (a sample, typically, which its own tests run).  Its
  other lines must appear in that file verbatim and in order; a line reading
  `// ...` (indented or not) separates pieces that need not be adjacent.

In the files under docs/manual/ and in docs/index.md every ```pkn block must
be one of these (an example with `Output:` or `Error:`, a module an example
uses, or an excerpt), so the manual has no untested code.  Elsewhere a ```pkn block that is
none of them (a fragment) is skipped.

Usage:
    doc_examples.py --paykan build/bin/paykan [--backend c ...] [file.md ...]

Without files it scans README.md and every Markdown file under docs/.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FENCE_RE = re.compile(r"^(```|~~~)\s*(\S*)\s*$")
ADDR_RE = re.compile(r"@0x[0-9a-fA-F]+")
LIVE_RE = re.compile(r"^  live blocks\s*:\s*(-?\d+)$", re.M)
MAIN_RE = re.compile(r"^\s*fn\s+main\s*\(", re.M)
OUTPUT_RE = re.compile(r"^Output( \(([^)]*)\))?:\s*$")
ERROR_RE = re.compile(r"^Error:\s*$")
INPUT_RE = re.compile(r"^Input:\s*$")
FILE_RE = re.compile(r"^\s*//\s*file:\s*(\S+\.pkn)\s*$")
ARGS_RE = re.compile(r"^arguments:\s*(.*)$")
FROM_RE = re.compile(r"^//\s*from:\s*(\S+)\s*$")
ELLIPSIS = "// ..."

# Markdown files in which every ```pkn block must be tested.
STRICT = ("docs/manual/", "docs/index.md")


class Example:
    """One runnable (or rejected) program and what it must do."""

    def __init__(self, lineno, src, modules):
        self.lineno = lineno
        self.src = src
        self.modules = modules  # [(relative path, source)]
        self.expected = None    # stdout, for an Output: example
        self.errors = None      # diagnostic lines, for an Error: example
        self.args = []
        self.stdin = ""


def blocks(lines):
    """Yield (info, first_line_no, body_lines, end_index) for each fence."""
    i = 0
    while i < len(lines):
        m = FENCE_RE.match(lines[i])
        if not m:
            i += 1
            continue
        fence, info, start = m.group(1), m.group(2), i
        j = i + 1
        while j < len(lines) and not lines[j].startswith(fence):
            j += 1
        yield info, start + 1, lines[i + 1:j], j
        i = j + 1


def label_then_fence(lines, fences, k, regex):
    """After fence @p k: the match of @p regex on the next non-blank line and
    the index of the fence right after it, or (None, None)."""
    j = fences[k][3] + 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    m = regex.match(lines[j]) if j < len(lines) else None
    if not m or k + 1 >= len(fences):
        return None, None
    j += 1
    while j < len(lines) and not lines[j].strip():
        j += 1
    if fences[k + 1][1] - 1 != j:
        return None, None  # the label is not followed by a fence
    return m, k + 1


def module_path(body):
    """The path of a module block, or None."""
    if not body:
        return None
    m = FILE_RE.match(body[0])
    if not m or any(FILE_RE.match(line) for line in body[1:]):
        return None
    return m.group(1)


def excerpt_problem(body):
    """Why the excerpt @p body does not match its file, or None."""
    rel = FROM_RE.match(body[0]).group(1)
    path = (ROOT / rel).resolve()
    if ROOT not in path.parents or not path.is_file():
        return f"no such file '{rel}'"
    text = path.read_text(encoding="utf-8").splitlines()
    pieces, cur = [], []
    for line in body[1:]:
        if line.strip() == ELLIPSIS:
            pieces.append(cur)
            cur = []
        else:
            cur.append(line)
    pieces.append(cur)
    at = 0
    for piece in (p for p in pieces if p):
        n = len(piece)
        hit = next((i for i in range(at, len(text) - n + 1)
                    if text[i:i + n] == piece), None)
        if hit is None:
            return f"lines not found in '{rel}' (in order): {piece[0]!r}..."
        at = hit + n
    return None


def count_excerpts(md):
    lines = md.read_text(encoding="utf-8").splitlines()
    return sum(1 for info, _, body, _ in blocks(lines)
               if info == "pkn" and body and FROM_RE.match(body[0]))


def examples(md):
    """(examples, untested pkn block line numbers, excerpt problems) for
    @p md."""
    lines = md.read_text(encoding="utf-8").splitlines()
    fences = list(blocks(lines))
    found, untested, modules, bad = [], [], [], []
    pending = []  # line numbers of modules not used by an example yet
    for k, (info, lineno, body, end) in enumerate(fences):
        if info != "pkn":
            continue
        if body and FROM_RE.match(body[0]):
            problem = excerpt_problem(body)
            if problem:
                bad.append((lineno, problem))
            continue
        src = "\n".join(body) + "\n"
        path = module_path(body)
        if path and path != "main.pkn":
            modules.append((path, src))
            pending.append(lineno)
            continue
        if not MAIN_RE.search(src):
            untested.append(lineno)
            continue
        ex = Example(lineno, src, modules)
        at = k
        m, nxt = label_then_fence(lines, fences, at, INPUT_RE)
        if m:
            ex.stdin = "".join(line + "\n" for line in fences[nxt][2])
            at = nxt
        m, nxt = label_then_fence(lines, fences, at, OUTPUT_RE)
        if m:
            ex.expected = "".join(line + "\n" for line in fences[nxt][2])
            note = ARGS_RE.match(m.group(2) or "")
            if note:
                ex.args = note.group(1).split()
        else:
            m, nxt = label_then_fence(lines, fences, at, ERROR_RE)
            if not m:
                untested.append(lineno)
                continue
            ex.errors = [line.strip() for line in fences[nxt][2]
                         if line.strip()]
        found.append(ex)
        modules, pending = [], []
    return found, untested + pending, bad


def write_project(ex, tmp):
    for rel, src in ex.modules:
        dest = Path(tmp) / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(src, encoding="utf-8")
    prog = Path(tmp) / "main.pkn"
    prog.write_text(ex.src, encoding="utf-8")
    return prog


def run(paykan, backend, ex, tmp):
    prog = write_project(ex, tmp)
    p = subprocess.run([paykan, f"--backend={backend}", "--track-heap",
                        str(prog)] + ex.args, input=ex.stdin,
                       capture_output=True, text=True, errors="replace",
                       cwd=tmp)
    m = LIVE_RE.search(p.stderr)
    return p.returncode, ADDR_RE.sub("@ADDR", p.stdout), \
        int(m.group(1)) if m else None, p.stderr


def check_output(paykan, backends, ex, where):
    failed = False
    for be in backends:
        with tempfile.TemporaryDirectory(prefix="paykan-doc-") as tmp:
            rc, out, live, err = run(paykan, be, ex, tmp)
        problems = []
        if rc != 0:
            problems.append(f"exit {rc}")
        if out != ADDR_RE.sub("@ADDR", ex.expected):
            problems.append("stdout differs from Output:")
        if live != 0:
            problems.append(f"live blocks {live}")
        if problems:
            failed = True
            print(f"FAIL {where} ({be}): " + "; ".join(problems))
            print(f"--- expected\n{ex.expected}--- got\n{out}{err}")
    return failed


def check_error(paykan, ex, where):
    with tempfile.TemporaryDirectory(prefix="paykan-doc-") as tmp:
        prog = write_project(ex, tmp)
        p = subprocess.run([paykan, "--check-only", str(prog)],
                           stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, errors="replace", cwd=tmp)
    missing = [e for e in ex.errors if e not in p.stderr]
    if p.returncode == 0 or missing:
        print(f"FAIL {where}: " + ("accepted by --check-only"
                                   if p.returncode == 0 else
                                   "diagnostics differ from Error:"))
        print("--- expected (each line)\n" + "\n".join(ex.errors) +
              f"\n--- got\n{p.stderr}")
        return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--backend", action="append",
                    help="a backend to run (repeatable; default: c)")
    ap.add_argument("files", nargs="*", help="Markdown files to scan")
    args = ap.parse_args()
    backends = args.backend or ["c"]
    paykan = str(Path(args.paykan).resolve())
    files = [Path(f).resolve() for f in args.files] or \
        [ROOT / "README.md"] + sorted((ROOT / "docs").rglob("*.md"))

    total = failures = excerpts = 0
    for md in files:
        rel = md.relative_to(ROOT).as_posix()
        found, untested, bad = examples(md)
        excerpts += count_excerpts(md)
        for lineno, problem in bad:
            failures += 1
            print(f"FAIL {rel}:{lineno}: excerpt: {problem}")
        if rel.startswith(STRICT):
            for lineno in untested:
                failures += 1
                print(f"FAIL {rel}:{lineno}: a pkn block in the manual that "
                      "is not tested: make it an example (Output: or "
                      "Error:), a module an example imports (// file:) "
                      "or an excerpt (// from:)")
        for ex in found:
            total += 1
            where = f"{rel}:{ex.lineno}"
            if ex.errors is not None:
                failures += check_error(paykan, ex, where)
            else:
                failures += check_output(paykan, backends, ex, where)
    print(f"{total} doc examples and {excerpts} excerpts checked on "
          f"{', '.join(backends)}: {failures} failure(s)")
    if total == 0:
        print("error: no doc examples found")
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
