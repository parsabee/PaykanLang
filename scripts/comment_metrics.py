#!/usr/bin/env python3
"""Comment metrics of PaykanLang's first-party C/C++ sources.

    scripts/comment_metrics.py [path ...]

For every tracked .c/.cpp/.h file under the given paths (default: src,
include, tests), the number of lines, the number of comment lines, the
longest run of comment lines, and how many runs of eight or more lines there
are; then the totals.  A line is a comment line when it starts with `//` or
lies inside a `/* ... */` block.  Sorted by comment lines, most first.
"""
import re
import subprocess
import sys

paths = sys.argv[1:] or ["src", "include", "tests"]
files = subprocess.run(["git", "ls-files", "--", *paths], capture_output=True,
                       text=True, check=True).stdout.split()
rows = []
for path in files:
    if not re.search(r"\.(c|cpp|h)$", path):
        continue
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    comments = run = longest = blocks = 0
    in_block = False
    for line in lines:
        s = line.strip()
        is_comment = in_block or s.startswith("//") or s.startswith("/*")
        if s.startswith("/*") and "*/" not in s:
            in_block = True
        elif in_block and "*/" in s:
            in_block = False
        if is_comment:
            comments += 1
            run += 1
            longest = max(longest, run)
        else:
            blocks += run >= 8
            run = 0
    rows.append((path, len(lines), comments, longest, blocks))

rows.sort(key=lambda r: -r[2])
print(f"{'file':<56}{'lines':>7}{'cmt':>6}{'%':>5}{'long':>6}{'>=8':>5}")
for path, n, c, longest, blocks in rows:
    print(f"{path:<56}{n:>7}{c:>6}{100 * c // max(n, 1):>4}%{longest:>6}"
          f"{blocks:>5}")
total = sum(r[1] for r in rows)
comments = sum(r[2] for r in rows)
print(f"TOTAL {total} lines, {comments} comment lines "
      f"({100 * comments // max(total, 1)}%)")
