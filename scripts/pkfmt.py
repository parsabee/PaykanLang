#!/usr/bin/env python3
"""Check or apply the layout of Paykan source (.pkn).

The layout:

- A line is at most WIDTH (80) characters.  A statement that does not fit
  spills onto the next rows, CONT deeper.
- A function's declaration is on one line (one that does not fit has its
  parameters aligned after its `(`), its body on the lines that follow.
- In a class, struct, impl or trait, a method whose whole definition fits
  on one line is written there, its body aligned with the others' at the
  column after the longest such declaration.  One whose body would fit
  there but not beside its declaration has the body on the next line, at
  that column.  Any other method is written out on the lines that follow.
- A class's or struct's fields are on lines of their own.
- An enum is on one line when it fits in COL (40) characters, else one
  variant per line.

Only whitespace changes: a file whose tokens a rewrite would change is left
as it is and reported.

Usage:
    scripts/pkfmt.py [--apply] [file ...]

    --apply     Reformat files in place.  Without it, the script only checks
                and prints a unified diff for any file that needs formatting.
    file ...    Optional explicit files.  When omitted, all tracked .pkn
                files are processed.

In check mode, exits non-zero if any file is not formatted.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WIDTH = 80    # a line
COL = 40      # an enum on one line
STEP = 2      # block indentation
CONT = 4      # continuation indentation of a spilled statement


# -- Tokens -------------------------------------------------------------------

LEX = re.compile(r'''
    (?P<nl>\n)
  | (?P<ws>[ \t\r]+)
  | (?P<com>//[^\n]*)
  | (?P<str>"(?:[^"\\\n]|\\.)*")
  | (?P<chr>'(?:[^'\\\n]|\\.)*')
  | (?P<word>[A-Za-z_]\w*)
  | (?P<num>\d\w*)
  | (?P<op>::|->|==|!=|<=|>=|&&|\|\||.)
''', re.X)


class Tok:
    __slots__ = ("kind", "text", "nl", "ws", "line", "col")

    def __init__(self, kind, text, nl, ws, line, col):
        self.kind, self.text, self.nl, self.ws = kind, text, nl, ws
        self.line, self.col = line, col


def lex(text):
    """The tokens of @p text, each with the newlines (`nl`) or the spaces
    (`ws`) before it, its line and its column."""
    out = []
    nl, ws, line, col = 0, "", 0, 0
    for m in LEX.finditer(text):
        kind, t = m.lastgroup, m.group()
        if kind == "nl":
            nl, ws, line, col = nl + 1, "", line + 1, 0
            continue
        if kind == "ws":
            ws += t
            col += len(t)
            continue
        if kind == "com":
            t = t.rstrip()
        out.append(Tok(kind, t, nl, ws, line, col))
        col += len(m.group())
        nl, ws = 0, ""
    return out


def significant(text):
    """What a rewrite must keep: the tokens of @p text, comments included."""
    return [t.text for t in lex(text)]


def line_tokens(code):
    """(kind, text) pairs of one line of code, spaces included."""
    return [(m.lastgroup, m.group()) for m in LEX.finditer(code)]


NO_SPACE_AFTER = {"(", "["}
NO_SPACE_BEFORE = {")", "]", ",", ";", "."}


def flatten(toks):
    """@p toks (no comments) on one line: a single space where they had
    spaces, or a line break between them where one belongs."""
    out = []
    prev = None
    for t in toks:
        if prev is not None:
            if t.nl:
                if prev.text not in NO_SPACE_AFTER and \
                        t.text not in NO_SPACE_BEFORE:
                    out.append(" ")
            elif t.ws:
                out.append(" ")
        out.append(t.text)
        prev = t
    return "".join(out)


def verbatim(toks, ind):
    """@p toks as they were written, moved to start at column @p ind."""
    delta = ind - toks[0].col
    lines = [" " * ind]
    for k, t in enumerate(toks):
        if k and t.nl:
            lines.extend([""] * (t.nl - 1))
            lines.append(" " * max(0, t.col + delta))
        elif k:
            lines[-1] += t.ws
        lines[-1] += t.text
    return lines


# -- Parsing ------------------------------------------------------------------

class Block:
    """`{ ... }`: its tokens (`first`/`last` index the braces) and its
    contents as nodes."""

    def __init__(self, first, last, nodes):
        self.first, self.last, self.nodes = first, last, nodes


class Item:
    """A declaration or a statement: parts that are runs of token indices
    or blocks, and a trailing comment (a token index) or None."""

    def __init__(self, parts, trail, first, last):
        self.parts, self.trail = parts, trail
        self.first, self.last = first, last   # token indices
        self.blanks = 0


class Comment:
    def __init__(self, tok):
        self.tok = tok
        self.blanks = 0


CHAIN = {"else", "elif", ";", "."}


class Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else None

    def nodes(self, in_block):
        """Nodes up to a `}` (left unread) or the end."""
        out = []
        while True:
            t = self.peek()
            if t is None or (in_block and t.text == "}"):
                return out
            blanks = max(0, t.nl - 1)
            if t.kind == "com":
                node = Comment(self.i)
                self.i += 1
            elif t.text == ";":     # a stray `;`
                node = Item([[self.i]], None, self.i, self.i)
                self.i += 1
            else:
                node = self.item()
            node.blanks = blanks
            out.append(node)

    def block(self):
        first = self.i
        self.i += 1
        nodes = self.nodes(True)
        last = self.i
        if self.peek() is not None:
            self.i += 1
        return Block(first, last, nodes)

    def raw_block(self, run):
        """A `{ ... }` inside brackets (a function value's body): its tokens
        in @p run."""
        depth = 0
        while self.peek() is not None:
            t = self.peek()
            run.append(self.i)
            self.i += 1
            if t.kind == "com":
                continue
            if t.text == "{":
                depth += 1
            elif t.text == "}":
                depth -= 1
                if depth == 0:
                    return

    def item(self):
        first = self.i
        parts, run = [], []
        depth = 0
        while True:
            t = self.peek()
            if t is None:
                break
            if t.kind != "com" and depth <= 0 and t.text == "}":
                break
            if t.kind != "com" and t.text == "{":
                if depth > 0:
                    self.raw_block(run)
                    continue
                # A lambda's body at depth 0 (`fn() -> int { ... }`): the
                # item goes on when a call or an index follows it,
                # `n = fn() -> int { return 41; }() + 1;`.
                lam = any(self.toks[i].text == "fn" for i in run)
                if run:
                    parts.append(run)
                run = []
                parts.append(self.block())
                nxt = self.peek()
                if nxt is None or (nxt.text not in CHAIN and
                                   not (lam and nxt.text in ("(", "["))):
                    break
                continue
            run.append(self.i)
            self.i += 1
            if t.kind == "com":
                continue
            if t.text in "([":
                depth += 1
            elif t.text in ")]":
                depth -= 1
            elif t.text == ";" and depth <= 0:
                break
        if run:
            parts.append(run)
        last = self.i - 1
        trail = None
        t = self.peek()
        if t is not None and t.kind == "com" and t.nl == 0:
            trail = self.i
            self.i += 1
        return Item(parts, trail, first, last)


# -- Spilling a long line -------------------------------------------------------

BINARY = {"+", "-", "*", "/", "%", "&&", "||", "==", "!=", "<", ">", "<=",
          ">=", "then", "else"}
AFTER = {"=", "->", ","}
LAST_RESORT = {"return", "if", "elif", "while", "match", "fn", "let", "view",
               "inout", ":", "native", "static", "pub"}


def break_points(code):
    """(index, depth, priority) of each space of @p code where it may
    break: 0 after a comma or `=`/`->`, 1 before a binary operator, 2 any
    other space, 9 after a keyword or a type annotation's `:`."""
    toks = []   # (start, text, kind, depth)
    pos = depth = 0
    for kind, text in line_tokens(code):
        toks.append((pos, text, kind, depth))
        if kind == "op" and text in ("(", "[", "{"):
            depth += 1
        elif kind == "op" and text in (")", "]", "}"):
            depth -= 1
        pos += len(text)
    out = []
    for k, (start, text, kind, d) in enumerate(toks):
        if kind != "ws":
            continue
        prev = next((t for t in reversed(toks[:k]) if t[2] != "ws"), None)
        nxt = next((t for t in toks[k + 1:] if t[2] != "ws"), None)
        if not prev or not nxt:
            continue
        if prev[1] in LAST_RESORT or prev[1] == ":":
            prio = 9
        elif prev[1] in AFTER:
            prio = 0
        elif nxt[1] in BINARY:
            prio = 1
        else:
            prio = 2
        out.append((start, d, prio))
    return out


def best_rows(code, lead, cont, memo=None):
    """@p code in rows, the first at column @p lead, the rest at @p cont,
    each fitting in a line where it can.  Of the breaks that fit, the one
    giving the fewest rows; then the shallowest; then the best kind (after
    a comma or `=`, before an operator, any space, after a keyword); then
    the rightmost."""
    if memo is None:
        memo = {}
    key = (code, lead)
    if key in memo:
        return memo[key]
    if lead + len(code) <= WIDTH:
        memo[key] = [" " * lead + code]
        return memo[key]
    cands = [c for c in break_points(code) if c[0] > 0]
    fitting = [c for c in cands if lead + c[0] <= WIDTH]
    if not fitting:
        if not cands:
            memo[key] = [" " * lead + code]
            return memo[key]
        fitting = [min(cands, key=lambda c: (c[2] == 9, c[0]))]
    kind = lambda c: 0 if c[2] <= 1 else (1 if c[2] == 2 else 2)
    best = None
    for c in fitting:
        rest = best_rows(code[c[0] + 1:].lstrip(), cont, cont, memo)
        rows = [" " * lead + code[:c[0]].rstrip()] + rest
        score = (len(rows), c[1], kind(c), -c[0])
        if best is None or score < best[0]:
            best = (score, rows)
    memo[key] = best[1]
    return memo[key]


def declaration_rows(code, ind):
    """A function's declaration @p code at @p ind: on one line, or with its
    parameters a row at a time, aligned after its `(`."""
    if ind + len(code) <= WIDTH:
        return [" " * ind + code]
    depth, open_at, close_at, commas = 0, None, None, []
    pos = 0
    for kind, text in line_tokens(code):
        if kind == "op" and text in ("(", "["):
            if text == "(" and depth == 0 and open_at is None:
                open_at = pos
            depth += 1
        elif kind == "op" and text in (")", "]"):
            depth -= 1
            if depth == 0 and open_at is not None and close_at is None:
                close_at = pos
        elif text == "," and depth == 1 and close_at is None:
            commas.append(pos)
        pos += len(text)
    if open_at is None or close_at is None:
        return best_rows(code, ind, ind + CONT)
    cuts = [open_at] + commas + [close_at]
    params = [code[a + 1:b + 1].strip() for a, b in zip(cuts, cuts[1:])]
    align = ind + open_at + 1
    rows = [" " * ind + code[:open_at + 1]]
    for prm in params:
        row = rows[-1]
        sep = "" if row.endswith("(") else " "
        if len(row) + len(sep) + len(prm) <= WIDTH or row.endswith("("):
            rows[-1] = row + sep + prm
        else:
            rows.append(" " * align + prm)
    tail = code[close_at + 1:].strip()
    if tail:
        if len(rows[-1]) + 1 + len(tail) <= WIDTH:
            rows[-1] += " " + tail
        else:
            rows.extend(best_rows(tail, align, align))
    return rows


# -- Layout -------------------------------------------------------------------

MODIFIERS = {"pub", "view", "static", "native"}


class Formatter:
    def __init__(self, toks):
        self.toks = toks

    # Helpers over token indices.

    def tok(self, i):
        return self.toks[i]

    def span(self, item):
        return self.toks[item.first:item.last + 1]

    def has_comment(self, first, last):
        return any(t.kind == "com" for t in self.toks[first:last + 1])

    def kind(self, item):
        """fn, type (struct/class), enum, impl (impl/trait) or stmt."""
        for p in item.parts:
            if isinstance(p, Block):
                break
            for i in p:
                w = self.tok(i).text
                if w in MODIFIERS:
                    continue
                if w == "fn":
                    return "fn"
                if w in ("struct", "class"):
                    return "type"
                if w == "enum":
                    return "enum"
                if w in ("impl", "trait"):
                    return "impl"
                return "stmt"
        return "stmt"

    def flat_run(self, run):
        return flatten([self.tok(i) for i in run])

    def empty(self, b):
        """An empty block as it was written: `{}` or `{ }`."""
        return "{}" if self.tok(b.last).nl == 0 and \
            not self.tok(b.last).ws else "{ }"

    def body_flat(self, b):
        """@p b's contents on one line, `{ ... }`, or None when they hold a
        comment or a blank line."""
        if not b.nodes:
            return self.empty(b)
        inner = self.toks[b.first + 1:b.last]
        if any(t.kind == "com" or t.nl > 1 for t in inner):
            return None
        return "{ " + flatten(inner) + " }"

    def with_trail(self, lines, item):
        if item.trail is not None:
            t = self.tok(item.trail)
            lines[-1] += (t.ws or " ") + t.text
        return lines

    # Nodes.

    def nodes(self, nodes, ind, ctx):
        aligned = self.alignment(nodes, ind) if ctx == "members" else None
        out = []
        for k, n in enumerate(nodes):
            if k and n.blanks:
                out.extend([""] * min(n.blanks, 2))
            if isinstance(n, Comment):
                out.append(" " * ind + self.tok(n.tok).text)
            else:
                out.extend(self.item(n, ind, aligned))
        return out

    def methods(self, nodes):
        """(item, declaration, body on one line) of each method of @p nodes
        that has a body."""
        out = []
        for n in nodes:
            if not isinstance(n, Item) or self.kind(n) != "fn":
                continue
            if len(n.parts) != 2 or not isinstance(n.parts[1], Block):
                continue
            if self.has_comment(n.first, n.parts[1].first):
                continue
            out.append((n, self.flat_run(n.parts[0]),
                        self.body_flat(n.parts[1])))
        return out

    def alignment(self, nodes, ind):
        """The column at which the bodies of @p nodes' methods start:
        after the longest declaration of those that fit on one line."""
        cols = [ind + len(d) + 1 for _, d, b in self.methods(nodes)
                if b is not None and ind + len(d) + 1 + len(b) <= WIDTH]
        if not cols:
            return None
        col = max(cols)
        return {id(n): (d, b, col) for n, d, b in self.methods(nodes)
                if b is not None}

    def item(self, item, ind, aligned=None):
        kind = self.kind(item)
        blocks = [p for p in item.parts if isinstance(p, Block)]
        runs = [p for p in item.parts if not isinstance(p, Block)]
        # A comment inside a declaration or statement (not in a block of
        # it): as it was written.
        if any(self.tok(i).kind == "com" for r in runs for i in r):
            return self.with_trail(verbatim(self.span(item), ind), item)
        if kind == "enum" and blocks:
            return self.with_trail(self.enum(item, ind), item)
        if not blocks:
            return self.with_trail(self.simple(item, ind, kind), item)
        if kind == "fn" and aligned and id(item) in aligned:
            decl, body, col = aligned[id(item)]
            if ind + len(decl) + 1 <= col and col + len(body) <= WIDTH:
                line = (" " * ind + decl).ljust(col) + body
                return self.with_trail([line], item)
            if col + len(body) <= WIDTH:
                rows = declaration_rows(decl, ind) + [" " * col + body]
                return self.with_trail(rows, item)
        if kind == "stmt" and self.tok(item.first).line == \
                self.tok(item.last).line:
            # A statement written on one line stays there if it fits.
            flat = flatten(self.span(item))
            if ind + len(flat) <= WIDTH and \
                    not self.has_comment(item.first, item.last):
                return self.with_trail([" " * ind + flat], item)
        ctx = "members" if kind in ("type", "impl") else "body"
        return self.with_trail(self.expanded(item, ind, kind, ctx), item)

    def simple(self, item, ind, kind):
        flat = flatten(self.span(item))
        if ind + len(flat) <= WIDTH:
            return [" " * ind + flat]
        if kind == "fn":
            return declaration_rows(flat, ind)
        return self.closure_rows(list(range(item.first, item.last + 1)),
                                 ind)

    def closure_rows(self, run, ind):
        """The tokens @p run of a statement that does not fit on a line:
        spilled, but a function value's body written out on the lines
        between its `{` and its `}`, as a block's."""
        flat = flatten([self.tok(i) for i in run])
        k = next((k for k, i in enumerate(run) if self.tok(i).text == "{"),
                 None)
        if k is None or ind + len(flat) <= WIDTH:
            return best_rows(flat, ind, ind + CONT)
        parser = Parser(self.toks)
        parser.i = run[k]
        b = parser.block()
        head = flatten([self.tok(i) for i in run[:k + 1]])
        rows = best_rows(head, ind, ind + CONT)
        rows += self.nodes(b.nodes, ind + STEP, "body")
        return rows + self.closure_rows([i for i in run if i >= b.last], ind)

    def expanded(self, item, ind, kind, ctx):
        """@p item written out: each block's contents on the lines between
        its `{` and its `}`."""
        out = []
        cur = ""
        first_head = True

        def emit(text):
            nonlocal first_head
            if kind == "fn" and first_head:
                out.extend(declaration_rows(text, ind))
            else:
                out.extend(best_rows(text, ind, ind + CONT))
            first_head = False

        for p in item.parts:
            if isinstance(p, Block):
                if not p.nodes:
                    cur = (cur + " " if cur else "") + self.empty(p)
                    continue
                if kind == "fn" and first_head:
                    rows = declaration_rows(cur, ind)
                    if len(rows[-1]) + 2 <= WIDTH or len(rows) == 1:
                        rows[-1] += " {"
                    else:
                        rows.append(" " * ind + "{")
                    out.extend(rows)
                    first_head = False
                else:
                    emit((cur + " {") if cur else "{")
                out.extend(self.nodes(p.nodes, ind + STEP, ctx))
                cur = "}"
            else:
                text = self.flat_run(p)
                if cur and not text.startswith((";", ".")):
                    cur += " "
                cur += text
        if cur:
            emit(cur)
        return out

    def enum(self, item, ind):
        """One line when it fits in a column and holds no comment, else a
        variant per line."""
        b = next(p for p in item.parts if isinstance(p, Block))
        head = flatten([self.tok(i) for r in item.parts
                        if not isinstance(r, Block) for i in r])
        inner = self.toks[b.first + 1:b.last]
        if not any(t.kind == "com" for t in inner):
            one = " " * ind + head + " { " + flatten(inner) + " }"
            if not inner:
                one = " " * ind + head + " " + self.empty(b)
            if len(one) < COL:
                return [one]
        lines = [" " * ind + head + " {"]
        cur = None
        for t in inner:
            if t.kind == "com":
                if cur is not None and t.nl == 0:
                    lines.append(cur + (t.ws or " ") + t.text)
                    cur = None
                else:
                    if cur is not None:
                        lines.append(cur)
                        cur = None
                    if t.nl > 1 and len(lines) > 1:
                        lines.append("")
                    lines.append(" " * (ind + STEP) + t.text)
                continue
            if t.text == ",":
                cur = (cur or " " * (ind + STEP)) + ","
                continue
            if cur is not None and not cur.rstrip().endswith(","):
                cur += " " + t.text if t.ws or t.nl else t.text
                continue
            if cur is not None:
                lines.append(cur)
            if t.nl > 1 and len(lines) > 1:
                lines.append("")
            cur = " " * (ind + STEP) + t.text
        if cur is not None:
            lines.append(cur)
        lines.append(" " * ind + "}" + self.after_block(item, b))
        return lines

    def after_block(self, item, b):
        rest = []
        seen = False
        for p in item.parts:
            if p is b:
                seen = True
            elif seen and not isinstance(p, Block):
                rest.extend(p)
        return self.flat_run(rest) if rest else ""


def fmt(text):
    toks = lex(text)
    nodes = Parser(toks).nodes(False)
    lines = Formatter(toks).nodes(nodes, 0, "top")
    return "\n".join(l.rstrip() for l in lines) + "\n"


# -- Driver -------------------------------------------------------------------

def tracked_sources():
    out = subprocess.run(["git", "ls-files", "*.pkn"], cwd=REPO_ROOT,
                         check=True, capture_output=True, text=True).stdout
    return [os.path.join(REPO_ROOT, p) for p in out.split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--apply", action="store_true",
                    help="reformat files in place")
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    paths = args.files or tracked_sources()
    changed = skipped = 0
    for path in paths:
        with open(path) as f:
            src = f.read()
        new = fmt(src)
        if significant(new) != significant(src) or fmt(new) != new:
            print("SKIP (tokens would change, or not stable):", path,
                  file=sys.stderr)
            skipped += 1
            continue
        if new == src:
            continue
        changed += 1
        if args.apply:
            with open(path, "w") as f:
                f.write(new)
        else:
            rel = os.path.relpath(path, REPO_ROOT)
            sys.stdout.writelines(difflib.unified_diff(
                src.splitlines(True), new.splitlines(True),
                "a/" + rel, "b/" + rel))
    verb = "reformatted" if args.apply else "need formatting"
    print(f"{changed} of {len(paths)} files {verb}; {skipped} skipped",
          file=sys.stderr)
    return 1 if changed and not args.apply else 0


if __name__ == "__main__":
    sys.exit(main())
