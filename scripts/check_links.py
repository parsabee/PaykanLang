#!/usr/bin/env python3
"""Check the relative links in every tracked Markdown file.

Each `[text](target)` link and each `<a href>`/`<img src>` whose target is a
relative path (not `http(s):`, `mailto:` or a bare `#anchor`) must name a file
or directory that exists, relative to the Markdown file.  A `#fragment` on a
link to a Markdown file (or a bare `#fragment`) must match a heading of that
file, using GitHub's anchor rules.  Links inside fenced code blocks and inline
code are ignored.

Usage:
    scripts/check_links.py [file.md ...]   # default: every tracked *.md

The `MarkdownLinks` ctest runs it over the whole tree.

Exits non-zero and lists each broken link when any is found.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FENCE_RE = re.compile(r"^\s*(```|~~~)")
INLINE_CODE_RE = re.compile(r"`[^`\n]*`")
LINK_RE = re.compile(r"!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_RE = re.compile(r"""<(?:a|img)\s[^>]*?(?:href|src)=["']([^"']+)["']""", re.I)
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


def tracked_markdown():
    """Every tracked Markdown file; outside a git checkout, every one found."""
    try:
        out = subprocess.run(["git", "ls-files", "*.md"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout
        return sorted({ROOT / line for line in out.splitlines() if line})
    except (OSError, subprocess.CalledProcessError):
        skip = ("build", ".git", "third-party", "_deps", ".paykan_cache")
        return sorted(p for p in ROOT.rglob("*.md")
                      if not any(part.startswith(skip)
                                 for part in p.relative_to(ROOT).parts))


def strip_code(text):
    """The text with fenced code blocks and inline code spans blanked out."""
    lines, in_fence = [], False
    for line in text.splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            lines.append("")
            continue
        lines.append("" if in_fence else INLINE_CODE_RE.sub("", line))
    return lines


def slug(heading):
    """GitHub's anchor for a heading."""
    h = re.sub(r"<[^>]+>", "", heading)
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", h)  # [text](url) -> text
    h = h.strip().lower().replace("`", "")
    h = re.sub(r"[^\w\- ]", "", h)
    return h.replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        seen, result = {}, set()
        for line in strip_code(path.read_text(encoding="utf-8")):
            m = HEADING_RE.match(line)
            if not m:
                continue
            base = slug(m.group(2))
            n = seen.get(base, 0)
            seen[base] = n + 1
            result.add(base if n == 0 else f"{base}-{n}")
        cache[path] = result
    return cache[path]


def check(md):
    problems = []
    for lineno, line in enumerate(strip_code(md.read_text(encoding="utf-8")), 1):
        targets = LINK_RE.findall(line) + HTML_RE.findall(line)
        for target in targets:
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                continue  # http:, https:, mailto:, ...
            path_part, _, frag = target.partition("#")
            dest = md if not path_part else (md.parent / path_part).resolve()
            where = f"{md.relative_to(ROOT)}:{lineno}: {target}"
            if not dest.exists():
                problems.append(f"{where}: no such file")
            elif ROOT not in dest.parents and dest != ROOT:
                problems.append(f"{where}: points outside the repository")
            elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                problems.append(f"{where}: no heading '#{frag}'")
    return problems


def main(argv):
    files = [Path(a).resolve() for a in argv] or tracked_markdown()
    problems = [p for md in files for p in check(md)]
    for p in problems:
        print(p)
    print(f"{len(files)} Markdown files, {len(problems)} broken links")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
