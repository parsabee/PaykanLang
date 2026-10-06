#!/usr/bin/env python3
"""Check the relative links in every tracked Markdown file.

Each `[text](target)` link and each `<a href>`/`<img src>` whose target is a
relative path (not `http(s):`, `mailto:` or a bare `#anchor`) must name a file
or directory that exists, relative to the Markdown file.  A `#fragment` on a
link to a Markdown file (or a bare `#fragment`) must match a heading of that
file, using GitHub's anchor rules.  Links inside fenced code blocks and inline
code are ignored.

The Markdown files under docs/ are also the pages of the documentation site
(GitHub Pages builds docs/ with Jekyll; see docs/_config.yml), so for them it
also checks what would break the site but not GitHub's rendering:

- a link to a directory under docs/ must have an index.md there (the site
  serves pages, not directory listings);
- the text must not contain `{{` or `{%`, which Jekyll's Liquid would
  interpret, even inside code blocks;
- every page must have its entry (`- scope: { path: <page> }`) in
  docs/_config.yml, which places it in the site's navigation, and every
  entry must name an existing page.

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
DOCS = ROOT / "docs"
LIQUID_RE = re.compile(r"\{\{|\{%")
NAV_RE = re.compile(r"^\s*-\s*scope:\s*\{\s*path:\s*\"?([^\s\",}]+)", re.M)


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


def strip_code(text, inline=True):
    """The text with fenced code blocks (and, with @p inline, inline code
    spans) blanked out."""
    lines, in_fence = [], False
    for line in text.splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            lines.append("")
            continue
        if in_fence:
            lines.append("")
        else:
            lines.append(INLINE_CODE_RE.sub("", line) if inline else line)
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
        # A heading's code spans are part of its anchor: `## The `x` y` is
        # #the-x-y.
        for line in strip_code(path.read_text(encoding="utf-8"),
                               inline=False):
            m = HEADING_RE.match(line)
            if not m:
                continue
            base = slug(m.group(2))
            n = seen.get(base, 0)
            seen[base] = n + 1
            result.add(base if n == 0 else f"{base}-{n}")
        cache[path] = result
    return cache[path]


def site_pages_in_nav():
    config = DOCS / "_config.yml"
    if not config.exists():
        return None
    return set(NAV_RE.findall(config.read_text(encoding="utf-8")))


def check_site_page(md, nav):
    """The problems that would only show on the documentation site."""
    problems = []
    rel = md.relative_to(ROOT)
    text = md.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        if LIQUID_RE.search(line):
            problems.append(f"{rel}:{lineno}: '{{{{' or '{{%' would be read "
                            "as Liquid by the site's Jekyll build")
    if nav is not None and md.relative_to(DOCS).as_posix() not in nav:
        problems.append(f"{rel}: no navigation entry in docs/_config.yml")
    return problems


def check(md, nav=None):
    problems = []
    site = DOCS in md.parents
    if site:
        problems += check_site_page(md, nav)
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
            elif site and dest.is_dir() and (dest == DOCS or DOCS in
                                             dest.parents) \
                    and not (dest / "index.md").exists():
                problems.append(f"{where}: a directory of the site without "
                                "an index.md (link to a page instead)")
            elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                problems.append(f"{where}: no heading '#{frag}'")
    return problems


def main(argv):
    files = [Path(a).resolve() for a in argv] or tracked_markdown()
    nav = site_pages_in_nav()
    problems = [p for md in files for p in check(md, nav)]
    if not argv and nav is not None:
        problems += [f"docs/_config.yml: navigation entry '{page}' names no "
                     "page" for page in sorted(nav)
                     if page and not (DOCS / page).is_file()]
    for p in problems:
        print(p)
    print(f"{len(files)} Markdown files, {len(problems)} broken links")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
