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

Each such example is written to a temporary directory, run with
`paykan --track-heap` (stdin empty, no arguments) on every backend named on
the command line, and must exit with status 0, print exactly the stated
output (`Foo@0x...` addresses normalised) and leave zero live heap blocks.

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
OUTPUT_RE = re.compile(r"^Output( \([^)]*\))?:\s*$")


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


def examples(md):
    """(line, source, expected stdout) for each example in @p md."""
    lines = md.read_text(encoding="utf-8").splitlines()
    fences = list(blocks(lines))
    found = []
    for k, (info, lineno, body, end) in enumerate(fences):
        if info != "pkn" or not MAIN_RE.search("\n".join(body)):
            continue
        j = end + 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j >= len(lines) or not OUTPUT_RE.match(lines[j]):
            continue
        nxt = fences[k + 1] if k + 1 < len(fences) else None
        if nxt is None:
            continue
        j += 1
        while j < len(lines) and not lines[j].strip():
            j += 1
        if nxt[1] - 1 != j:
            continue  # the Output: line is not followed by a fence
        expected = "".join(line + "\n" for line in nxt[2])
        found.append((lineno, "\n".join(body) + "\n", expected))
    return found


def run(paykan, backend, src, tmp):
    prog = Path(tmp) / "main.pkn"
    prog.write_text(src, encoding="utf-8")
    p = subprocess.run([paykan, f"--backend={backend}", "--track-heap",
                        str(prog)], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, errors="replace",
                       cwd=tmp)
    m = LIVE_RE.search(p.stderr)
    return p.returncode, ADDR_RE.sub("@ADDR", p.stdout), \
        int(m.group(1)) if m else None, p.stderr


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

    total = failures = 0
    for md in files:
        for lineno, src, expected in examples(md):
            total += 1
            where = f"{md.relative_to(ROOT)}:{lineno}"
            failed = False
            for be in backends:
                with tempfile.TemporaryDirectory(prefix="paykan-doc-") as tmp:
                    rc, out, live, err = run(paykan, be, src, tmp)
                problems = []
                if rc != 0:
                    problems.append(f"exit {rc}")
                if out != ADDR_RE.sub("@ADDR", expected):
                    problems.append("stdout differs from Output:")
                if live != 0:
                    problems.append(f"live blocks {live}")
                if problems:
                    failed = True
                    print(f"FAIL {where} ({be}): " + "; ".join(problems))
                    print(f"--- expected\n{expected}--- got\n{out}{err}")
            failures += failed
    print(f"{total - failures}/{total} doc examples ok on "
          f"{', '.join(backends)}")
    if total == 0:
        print("error: no doc examples found")
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
