#!/usr/bin/env python3
"""Check that every local that is never reassigned is declared with `let`.

A local declared without `let`, by its first assignment (`x = e;`) or with
a type (`x: T = e;`), must need it: it is reassigned or compound-assigned,
passed to an `inout` parameter or arm, or, for a struct, tuple or
fixed-size array, has a field or element assigned or a method that may
change it called on it.  Any other is `let` (docs/language/02-basics.md,
"Variables").  This holds for the samples (samples/**/*.pkn), the examples
of docs/language/*.md and docs/stdlib/*.md, and the standard library
(stdlib/*.pkn).

The compiler is the oracle:

1. The declarations are read from each program's AST (`paykan
   --emit-ast`).  `x = e;` declares `x` only where no variable `x` is in
   scope, as Sema decides: an assignment in an inner block to a variable of
   an enclosing one is not a declaration (`let` there would declare another
   variable).  A declaration whose variable is assigned again is left
   alone.
2. Every other declaration of the program is made `let` at once.  Each one
   the compiler then refuses (`'x' is declared with 'let' and ...`) is taken
   back, and the program compiled again, until it accepts the rest.
3. What is left must change nothing else: the program's diagnostics stay as
   they were, and a program with `fn main` keeps its `paykan --emit-pir`
   byte for byte.  A declaration that changes them is taken back too.

Each declaration left is reported.  The program is compiled with
`--emit-pir` when it has `fn main`, with `--check-only` otherwise; a
program that Sema rejects (an `err_` sample, a docs example with `Error:`)
is checked as well, and a declaration in it counts only where Sema reaches
it (a call of an undeclared function put before it is reported) and when
`let` keeps every line of a docs example's `Error:` block in the
diagnostics.

A sample of samples/imports is the directory of its main.pkn, compiled
with its modules; a docs example is compiled with the modules (`// file:`
blocks) before it, as scripts/doc_examples.py runs it.  The standard
library is compiled from the changed sources ($PAYKAN_STDLIB) for each
program of tests/Stdlib, which instantiate its generic code.

Not covered:

- borrows (`r: inout = k`, `v: view = s`), `weak` locals, destructuring,
  `for` variables and declarations without an initializer, which cannot be
  `let`;
- a declaration in the body of a generic function or type that the program
  never instantiates: Sema checks such a body for each of its
  instantiations only;
- a module of a sample that its program does not import, a docs block
  that is not a whole program and that `--check-only` rejects, and a file
  that does not parse;
- a docs example whose opening fence follows the line
  `<!-- let_check: plain -->`: one that shows the plain declaration itself.

Usage:
    let_check.py --paykan build/bin/paykan [-j N] [--apply] [-v] [path ...]

--apply makes each reported declaration `let` in place, and the docs'
excerpts of a changed sample follow it.  With paths (files
or directories), only the samples, docs pages and standard library under
them are checked.  -v also lists the declarations that could not be
checked, and why.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.dont_write_bytecode = True  # no scripts/__pycache__ in the tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import doc_examples  # noqa: E402  (the docs' fences, modules and labels)

ROOT = Path(__file__).resolve().parent.parent
STDLIB_TESTS = ROOT / "tests" / "Stdlib"
LET = "let "
PROBE = "let_check_probe_"
PLAIN_RE = re.compile(r"^<!--\s*let_check:\s*plain\s*-->\s*$")
DIAG_RE = re.compile(r"^(.+?):(\d+):(\d+): (error|warning|note): (.*)$",
                     re.M)
LET_ERROR_RE = re.compile(r"^'([^']+)' is declared with 'let'")
PROBE_RE = re.compile(r"call to undeclared function '" + PROBE + r"(\d+)'")
FN_RE = re.compile(r'^(?:shared )?fn @(?:"((?:[^"\\]|\\.)*)"|([^\s(]+))\(',
                   re.M)
TOKEN_RE = re.compile(r'\s+|;[^\n]*|\(|\)|"(?:[^"\\]|\\.)*"|[^\s()";]+')
LOC_RE = re.compile(r"^@(\d+):(\d+)-(\d+):(\d+)$")
MAX_ROUNDS = 16


# -- The AST ------------------------------------------------------------------

class Node:
    """A list of the AST interchange format (docs/plugins/ast-format.md):
    its tag, its location (l1, c1, l2, c2) or None, and its fields: nodes,
    atoms (str) and strings (a 1-tuple holding the string's source text)."""

    __slots__ = ("tag", "loc", "args")

    def __init__(self, tag, loc, args):
        self.tag, self.loc, self.args = tag, loc, args

    def child(self, tag):
        return next((a for a in self.args
                     if isinstance(a, Node) and a.tag == tag), None)


def parse_ast(text):
    stack, cur = [], []
    for m in TOKEN_RE.finditer(text):
        tok = m.group(0)
        if tok[0].isspace() or tok[0] == ";":
            continue
        if tok == "(":
            stack.append(cur)
            cur = []
        elif tok == ")":
            items, cur = cur, stack.pop()
            loc = None
            if len(items) > 1 and isinstance(items[1], str):
                lm = LOC_RE.match(items[1])
                if lm:
                    loc = tuple(int(x) for x in lm.groups())
                    del items[1]
            cur.append(Node(items[0], loc, items[1:]))
        elif tok[0] == '"':
            cur.append((tok[1:-1],))
        else:
            cur.append(tok)
    return cur[0]


def name_of(field):
    return field[0] if isinstance(field, tuple) else ""


def end_of(node):
    """The last position any location under @p node reaches."""
    end = node.loc[2:] if node.loc else (0, 0)
    for a in node.args:
        if isinstance(a, Node):
            end = max(end, end_of(a))
    return end


class Scope:
    def __init__(self, parent, start, end):
        self.parent, self.start, self.end = parent, start, end
        self.depth = parent.depth + 1 if parent else 0
        self.names = {}

    def lookup(self, name):
        scope = self
        while scope:
            if name in scope.names:
                return scope.names[name]
            scope = scope.parent
        return None


class Binding:
    """A variable: the scope it is declared in, from the position @p after
    on, and the declaration that could be `let` (or None)."""

    def __init__(self, name, scope, after, decl=None):
        self.name, self.scope, self.after, self.decl = \
            name, scope, after, decl
        self.reassigned = False


class Decl:
    """A declaration that could be `let`, `x = e;` or `x: T = e;`, at
    (@p line, @p col) of its source (a column counts bytes, from 1).  In a
    generic body, @p generic matches the names of the body's instantiations
    in the PIR (`Pair<int, Str>.swap`, `$::m::first<int>`); else it is
    None."""

    def __init__(self, src, line, col, name, generic):
        self.src, self.line, self.col, self.name, self.generic = \
            src, line, col, name, generic

    def where(self):
        return f"{self.src.label(self.line)}:{self.col}"


class Walker:
    """The declarations of one source, with the scopes Sema gives them."""

    def __init__(self, src):
        self.src = src
        self.bindings = {}   # name -> [Binding]
        self.generic = None

    def bind(self, scope, name, after, decl=None):
        b = Binding(name, scope, after, decl)
        scope.names[name] = b
        self.bindings.setdefault(name, []).append(b)
        return b

    def unit(self, unit):
        for d in unit.args:
            if not isinstance(d, Node):
                continue
            if d.tag == "fn":
                self.fn(d, None)
            elif d.tag in ("class", "impl"):
                owner = name_of(d.args[0]) or d.args[0]
                owner = owner.split("::")[-1]
                tps = d.child("type-params")
                generic = owner if tps is not None and tps.args else None
                methods = d.child("methods")
                for m in methods.args if methods else []:
                    self.fn(m, generic)

    def fn(self, f, owner):
        body = f.child("block")
        if body is None:
            return  # a native, pure virtual or trait method
        name = name_of(f.args[0])
        old = self.generic
        if owner:
            self.generic = re.compile(r"(?:^|::)" + re.escape(owner) +
                                      r"<.*>\." + re.escape(name) + "$")
        elif f.child("type-params").args:
            self.generic = re.compile(r"(?:^|::)" + re.escape(name) +
                                      r"<.*>$")
        scope = Scope(None, f.loc[:2], f.loc[2:])
        self.bind(scope, "self", f.loc[:2])
        for p in f.child("params").args:
            self.bind(scope, name_of(p.args[0]), f.loc[:2])
        self.block(body, scope)
        self.generic = old

    def block(self, block, parent):
        scope = Scope(parent, block.loc[:2], end_of(block))
        for st in block.args:
            self.stmt(st, scope)

    def declare(self, scope, st, name):
        decl = Decl(self.src, st.loc[0], st.loc[1], name, self.generic)
        return self.bind(scope, name, st.loc[2:], decl)

    def assigned(self, scope, name):
        b = scope.lookup(name)
        if b:
            b.reassigned = True
        return b

    def stmt(self, st, scope):
        tag = st.tag
        if tag == "block":
            self.block(st, scope)
        elif tag == "decl":
            var = st.args[0]
            name, rest = name_of(var.args[0]), var.args[1:]
            init = rest[1] if len(rest) > 1 else "_"
            self.expr(init, scope)
            if init != "_" and not any(isinstance(a, Node)
                                       for a in rest[2:]):
                self.declare(scope, st, name)  # no mode, `let` or `weak`
            else:
                self.bind(scope, name, st.loc[2:])
        elif tag == "assign":
            name = name_of(st.args[0].args[0])
            self.expr(st.args[1], scope)
            if not self.assigned(scope, name):
                self.declare(scope, st, name)
        elif tag == "destructure":
            self.expr(st.args[1], scope)
            for t in st.args[0].args:
                name = name_of(t.args[0])
                typed = len(t.args) > 1 and t.args[1] != "_"
                if name and (typed or not self.assigned(scope, name)):
                    self.bind(scope, name, st.loc[2:])
        elif tag == "for":
            for e in st.args[1:4]:
                self.expr(e, scope)
            inner = Scope(scope, st.loc[:2], st.loc[2:])
            if name_of(st.args[0].args[0]):
                self.bind(inner, name_of(st.args[0].args[0]), st.loc[:2])
            self.block(st.args[4], inner)
        elif tag == "match":
            self.expr(st.args[0], scope)
            for arm in st.args[1:]:
                inner = Scope(scope, arm.loc[:2], arm.loc[2:])
                if name_of(arm.args[0]):
                    self.bind(inner, name_of(arm.args[0]), arm.loc[:2])
                for a in arm.args[1:]:
                    if isinstance(a, Node) and a.tag == "block":
                        self.block(a, inner)
                    else:
                        self.expr(a, scope)
        elif tag in ("if", "while"):
            self.expr(st.args[0], scope)
            self.block(st.args[1], scope)
            if tag == "if" and isinstance(st.args[2], Node):
                self.stmt(st.args[2], scope)
        else:
            for a in st.args:
                self.expr(a, scope)

    def expr(self, e, scope):
        if not isinstance(e, Node):
            return
        if e.tag == "lambda":
            inner = Scope(scope, e.loc[:2], e.loc[2:])
            for p in e.child("params").args:
                self.bind(inner, name_of(p.args[0]), e.loc[:2])
            self.block(e.child("block"), inner)
            return
        for a in e.args:
            self.expr(a, scope)

    def resolve(self, name, pos):
        """The declaration of the variable @p name names at @p pos (in its
        source's lines), or None."""
        best = None
        for b in self.bindings.get(name, ()):
            sc = b.scope
            if sc.start <= pos <= sc.end and b.after <= pos and \
                    (best is None or sc.depth > best.scope.depth):
                best = b
        return best.decl if best else None


# -- Programs -----------------------------------------------------------------

class Source:
    """One source of a program: its text, where it is written in the
    program's directory (@p rel), and where it comes from: the file
    @p path, from its line @p line0 + 1 on (a docs block)."""

    def __init__(self, rel, text, path, line0=0, plain=False):
        self.rel, self.text, self.path, self.line0 = rel, text, path, line0
        self.plain = plain     # it shows the plain declaration
        self.walker = None

    def label(self, line):
        return f"{self.path.relative_to(ROOT).as_posix()}:{self.line0 + line}"

    def converted(self, decls):
        """The text with each of @p decls (of this source) made `let`."""
        lines = self.text.split("\n")
        for d in sorted(decls, key=lambda d: (d.line, -d.col)):
            raw = lines[d.line - 1].encode()
            lines[d.line - 1] = (raw[:d.col - 1] + LET.encode() +
                                 raw[d.col - 1:]).decode()
        return "\n".join(lines)

    def probed(self, decls):
        """The text with a call of an undeclared function before each of
        @p decls, numbered by their index in @p decls."""
        lines = self.text.split("\n")
        mine = [(i, d) for i, d in enumerate(decls) if d.src is self]
        for i, d in sorted(mine, key=lambda x: (x[1].line, -x[1].col)):
            raw = lines[d.line - 1].encode()
            lines[d.line - 1] = (raw[:d.col - 1] + f"{PROBE}{i}(); ".encode()
                                 + raw[d.col - 1:]).decode()
        return "\n".join(lines)

    def original_col(self, line, col, decls):
        """The column in the source of @p col on @p line of its text with
        @p decls made `let`."""
        shift = 0
        for c in sorted(d.col for d in decls if d.line == line):
            if c + shift + len(LET) > col:
                break
            shift += len(LET)
        return col - shift


class Program:
    """What one compiler run checks: @p sources, written in a directory of
    their own, and the @p entries compiled there.  For the standard library
    the sources are $PAYKAN_STDLIB and the entries the programs of
    tests/Stdlib."""

    def __init__(self, name, sources, entries, stdlib=False,
                 error_lines=None):
        self.name, self.sources, self.entries = name, sources, entries
        self.stdlib = stdlib
        self.error_lines = error_lines  # a docs example's Error: block
        self.mode = None                # --emit-pir or --check-only
        self.notes = []                 # why declarations were not checked

    def module_name(self, src):
        return ("::" if self.stdlib else "") + \
            src.rel[:-len(".pkn")].replace("/", "::")


def sample_programs(paths):
    """The samples: one program per file, and one per directory of
    samples/imports (its main.pkn and its modules)."""
    programs, done = [], set()
    for f in sorted((ROOT / "samples").rglob("*.pkn")):
        if ".paykan_cache" in f.parts or not wanted(f, paths) or f in done:
            continue
        rel = f.relative_to(ROOT / "samples")
        if rel.parts[0] == "imports" and len(rel.parts) > 2:
            top = ROOT / "samples" / rel.parts[0] / rel.parts[1]
            files = [g for g in sorted(top.rglob("*.pkn"))
                     if ".paykan_cache" not in g.parts]
            done.update(files)
            sources = [Source(g.relative_to(top).as_posix(), read(g), g)
                       for g in files]
            entries = ["main.pkn"] if (top / "main.pkn").is_file() else \
                [s.rel for s in sources]
            programs.append(Program(top.relative_to(ROOT).as_posix(),
                                    sources, entries))
        else:
            programs.append(Program(f.relative_to(ROOT).as_posix(),
                                    [Source(f.name, read(f), f)], [f.name]))
    return programs


def docs_programs(md):
    """The pkn blocks of @p md: an example with the modules before it, as
    scripts/doc_examples.py runs it, and each other block on its own.  An
    excerpt (`// from:`) is its sample's text, checked there."""
    lines = md.read_text(encoding="utf-8").splitlines()
    fences = list(doc_examples.blocks(lines))
    programs, modules = [], []
    for k, (info, lineno, body, _) in enumerate(fences):
        if info != "pkn" or not body or doc_examples.FROM_RE.match(body[0]):
            continue
        text = "\n".join(body) + "\n"
        before = next((ln for ln in reversed(lines[:lineno - 1])
                       if ln.strip()), "")
        path = doc_examples.module_path(body)
        src = Source(path or "main.pkn", text, md, lineno,
                     bool(PLAIN_RE.match(before)))
        name = f"{md.relative_to(ROOT).as_posix()}:{lineno}"
        if path and path != "main.pkn":
            modules.append(src)
            continue
        if not doc_examples.MAIN_RE.search(text):
            programs.append(Program(name, [src], [src.rel]))
            continue
        at = k
        m, nxt = doc_examples.label_then_fence(lines, fences, at,
                                               doc_examples.INPUT_RE)
        if m:
            at = nxt
        out, _ = doc_examples.label_then_fence(lines, fences, at,
                                               doc_examples.OUTPUT_RE)
        err, nxt = doc_examples.label_then_fence(lines, fences, at,
                                                 doc_examples.ERROR_RE)
        if not out and not err:
            programs.append(Program(name, [src], [src.rel]))
            continue
        error_lines = [ln.strip() for ln in fences[nxt][2]
                       if ln.strip()] if err else None
        programs.append(Program(name, modules + [src], [src.rel],
                                error_lines=error_lines))
        modules = []
    return programs


def stdlib_program():
    sources = [Source(f.name, read(f), f)
               for f in sorted((ROOT / "stdlib").glob("*.pkn"))]
    tests = sorted(STDLIB_TESTS.glob("*.pkn")) + \
        sorted((STDLIB_TESTS / "panics").glob("*.pkn"))
    return Program("stdlib", sources, tests, stdlib=True)


def read(path):
    return path.read_text(encoding="utf-8")


def wanted(path, paths):
    return not paths or any(path == p or p in path.parents for p in paths)


# -- Compiling ----------------------------------------------------------------

class Result:
    """What a compile of a program printed: its diagnostics, its PIR (or
    None) and its standard error."""

    def __init__(self, diags, pir, stderr):
        self.diags, self.pir, self.stderr = diags, pir, stderr

    def same(self, base, program):
        if self.diags != base.diags or self.pir != base.pir:
            return False
        return all(line in self.stderr for line in program.error_lines or ())


def compile_program(args, program, decls, probes=None):
    """Compile @p program with @p decls made `let` (or, with @p probes, a
    probe before each of those): its diagnostics, each (source index or
    file name, line, column, kind, message) in the sources' own lines, and
    its PIR (None unless compiled with --emit-pir and accepted)."""
    mode = program.mode
    by_src = {}
    for d in decls:
        by_src.setdefault(d.src, []).append(d)
    with tempfile.TemporaryDirectory(prefix="let_check-") as tmp:
        tmp = Path(os.path.realpath(tmp))
        top = tmp / ("stdlib" if program.stdlib else "program")
        files = {}
        for i, src in enumerate(program.sources):
            dest = top / src.rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            text = src.probed(probes) if probes is not None else \
                src.converted(by_src.get(src, ()))
            dest.write_text(text, encoding="utf-8")
            files[str(dest)] = i
        env = dict(os.environ, PAYKAN_NO_PLUGINS="1")
        env.pop("PAYKAN_STDLIB", None)
        entries = program.entries
        if program.stdlib:
            env["PAYKAN_STDLIB"] = str(top)
            (tmp / "tests").mkdir()
            entries = [shutil.copy(t, tmp / "tests" / t.name)
                       for t in program.entries]
        entries = [Path(e) if program.stdlib else top / e for e in entries]

        def run(entry):
            return entry, subprocess.run(
                [args.paykan, mode, "--no-module-cache", "--error-limit=0",
                 entry.name], cwd=entry.parent, env=env,
                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                errors="replace")

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            runs = list(pool.map(run, entries))
        diags, pirs, stderr = [], [], []
        for entry, p in runs:
            stderr.append(p.stderr)
            pirs.append(p.stdout if p.returncode == 0 else None)
            for m in DIAG_RE.finditer(p.stderr):
                path = os.path.realpath(entry.parent / m.group(1))
                line, col = int(m.group(2)), int(m.group(3))
                where = files.get(path)
                if where is not None and probes is None:
                    src = program.sources[where]
                    col = src.original_col(line, col, by_src.get(src, ()))
                elif where is None:
                    where = Path(path).name
                diags.append((where, line, col, m.group(4),
                              m.group(5).replace(str(tmp), "")))
    pir = None
    if mode == "--emit-pir" and all(p is not None for p in pirs):
        pir = "".join(pirs)
    return Result(diags, pir, "".join(stderr))


def ast_of(args, src):
    """The walked AST of @p src, or None if it does not parse."""
    with tempfile.TemporaryDirectory(prefix="let_check-") as tmp:
        dest = Path(tmp) / Path(src.rel).name
        dest.write_text(src.text, encoding="utf-8")
        p = subprocess.run([args.paykan, "--emit-ast", "--no-plugins",
                            dest.name], cwd=tmp, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        return None
    walker = Walker(src)
    walker.unit(parse_ast(p.stdout).args[1])
    return walker


# -- The fixed point ----------------------------------------------------------

def check_program(args, program):
    """The declarations of @p program that could be `let`."""
    decls = []
    for src in program.sources:
        src.walker = ast_of(args, src)
        if src.walker is None:
            program.notes.append(f"{src.label(1)}: does not parse")
        elif src.plain:
            program.notes.append(f"{src.label(1)}: shows plain declarations")
        else:
            decls += [b.decl for bs in src.walker.bindings.values()
                      for b in bs if b.decl and not b.reassigned]
    if not decls:
        return []
    program.mode = "--emit-pir"
    base = compile_program(args, program, [])
    if base.pir is None:
        if program.stdlib:
            program.notes.append("stdlib: a program of tests/Stdlib does "
                                 "not compile")
            return []
        program.mode = "--check-only"
        base = compile_program(args, program, [])
    decls = reached(args, program, base, decls)
    active = list(decls)
    for _ in range(MAX_ROUNDS):
        if not active:
            return []
        r = compile_program(args, program, active)
        if r.same(base, program):
            return active
        refused = refusals(program, base, r)
        if not refused:
            break
        active = [d for d in active if d not in refused]
    # No `let` error says what differs: one declaration at a time.
    active = [d for d in active
              if compile_program(args, program, [d]).same(base, program)]
    if active and not compile_program(args, program,
                                      active).same(base, program):
        program.notes.append(f"{program.name}: the declarations that "
                             "pass one at a time fail together")
        return []
    return active


def refusals(program, base, result):
    """The declarations the new `let` errors of @p result are about."""
    old = list(base.diags)
    refused = set()
    for d in result.diags:
        if d in old:
            old.remove(d)
            continue
        m = LET_ERROR_RE.match(d[4])
        if d[3] == "error" and m and isinstance(d[0], int):
            walker = program.sources[d[0]].walker
            decl = walker.resolve(m.group(1), (d[1], d[2])) if walker \
                else None
            if decl:
                refused.add(decl)
    return refused


def reached(args, program, base, decls):
    """@p decls without those Sema does not check in @p program: in a
    generic body never instantiated, in a module not compiled, or, in a
    program Sema rejects, where it does not get to."""
    if base.pir is not None:
        names = {m.group(1) or m.group(2) for m in FN_RE.finditer(base.pir)}
        modules = set(re.findall(r'^module "([^"]+)"$', base.pir, re.M))
        out = []
        for d in decls:
            if not program.stdlib and d.src.rel not in program.entries and \
                    program.module_name(d.src) not in modules:
                program.notes.append(f"{d.where()}: '{d.name}' is in a "
                                     "module the program does not import")
            elif d.generic and not any(d.generic.search(n) for n in names):
                program.notes.append(f"{d.where()}: '{d.name}' is in a "
                                     "generic body never instantiated")
            else:
                out.append(d)
        return out
    r = compile_program(args, program, [], probes=decls)
    seen = {int(m.group(1)) for m in PROBE_RE.finditer(r.stderr)}
    for i, d in enumerate(decls):
        if i not in seen:
            program.notes.append(f"{d.where()}: '{d.name}' is not checked "
                                 "(Sema does not get to it)")
    return [d for i, d in enumerate(decls) if i in seen]


# -- Applying -----------------------------------------------------------------

def comment_start(line, start):
    """Where the `//` comment of @p line after index @p start begins, or
    -1."""
    quote, i = None, start
    while i < len(line):
        if quote:
            if line[i] == "\\":
                i += 1
            elif line[i] == quote:
                quote = None
        elif line[i] in "\"'":
            quote = line[i]
        elif line.startswith("//", i):
            return i
        i += 1
    return -1


def realign(was, lines, changed):
    """Keep aligned comments aligned: each run of adjacent lines of @p was
    whose `//` comments start in one column, around a line of @p changed,
    has them in one column of @p lines again, the rightmost one's."""
    done = set()
    for n in sorted(changed):
        col = comment_start(was[n], 0)
        if col < 0 or n in done:
            continue
        lo, hi = n, n
        while lo > 0 and comment_start(was[lo - 1], 0) == col:
            lo -= 1
        while hi + 1 < len(was) and comment_start(was[hi + 1], 0) == col:
            hi += 1
        run = range(lo, hi + 1)
        done.update(run)
        to = max(comment_start(lines[k], 0) for k in run)
        for k in run:
            at = comment_start(lines[k], 0)
            lines[k] = lines[k][:at] + " " * (to - at) + lines[k][at:]


def apply(decls):
    """Write @p decls' `let`s into their files.  A comment after one stays
    in its column when there is room, and comments aligned with it stay so;
    the docs' excerpts of a changed file follow it."""
    by_path, old = {}, {}
    for d in decls:
        by_path.setdefault(d.src.path, []).append(d)
    for path, ds in by_path.items():
        old[path] = read(path)
        lines = old[path].split("\n")
        changed = set()
        for d in sorted(ds, key=lambda d: (d.src.line0 + d.line, -d.col)):
            n = d.src.line0 + d.line - 1
            raw = lines[n].encode()
            line = (raw[:d.col - 1] + LET.encode() + raw[d.col - 1:]).decode()
            at = comment_start(line, len(raw[:d.col - 1].decode()))
            if at > 0 and line[at - len(LET) - 1:at].isspace():
                line = line[:at - len(LET)] + line[at:]
            lines[n] = line
            changed.add(n)
        realign(old[path].split("\n"), lines, changed)
        path.write_text("\n".join(lines), encoding="utf-8")
    update_excerpts(old)


def update_excerpts(old):
    """Quote the new text in the docs' excerpts (`// from:` blocks) of the
    files in @p old (path -> text before), whose lines `let` kept."""
    for md in [ROOT / "README.md"] + sorted((ROOT / "docs").rglob("*.md")):
        lines = read(md).split("\n")
        changed = False
        for info, lineno, body, _ in doc_examples.blocks(lines):
            m = doc_examples.FROM_RE.match(body[0]) if body else None
            path = (ROOT / m.group(1)).resolve() if m else None
            if info != "pkn" or path not in old:
                continue
            was, now = old[path].split("\n"), read(path).split("\n")
            k, at = 1, 0
            while k < len(body):
                if body[k].strip() == doc_examples.ELLIPSIS:
                    k += 1
                    continue
                j = k
                while j < len(body) and \
                        body[j].strip() != doc_examples.ELLIPSIS:
                    j += 1
                n = j - k
                hit = next((i for i in range(at, len(was) - n + 1)
                            if was[i:i + n] == body[k:j]), None)
                if hit is None:
                    break
                for t in range(n):
                    if lines[lineno + k + t] != now[hit + t]:
                        lines[lineno + k + t] = now[hit + t]
                        changed = True
                at, k = hit + n, j
        if changed:
            md.write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                    help="programs checked at once")
    ap.add_argument("--apply", action="store_true",
                    help="make each reported declaration 'let'")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="list what was not checked")
    ap.add_argument("paths", nargs="*", type=Path,
                    help="check only what is under these")
    args = ap.parse_args()
    args.paykan = str(Path(args.paykan).resolve())
    paths = [p.resolve() for p in args.paths]

    programs = sample_programs(paths)
    for md in sorted((ROOT / "docs" / "language").glob("*.md")) + \
            sorted((ROOT / "docs" / "stdlib").glob("*.md")):
        if wanted(md, paths):
            programs += docs_programs(md)
    if any(wanted(f, paths) for f in (ROOT / "stdlib").glob("*.pkn")):
        programs.insert(0, stdlib_program())  # the longest: first

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        found = list(pool.map(lambda p: check_program(args, p), programs))

    report = sorted((d for ds in found for d in ds),
                    key=lambda d: (str(d.src.path), d.src.line0 + d.line,
                                   d.col))
    for d in report:
        print(f"{d.where()}: '{d.name}' is never reassigned: declare it "
              "with 'let'")
    notes = [n for p in programs for n in p.notes]
    if args.verbose:
        for n in notes:
            print(f"not checked: {n}")
    print(f"{len(programs)} programs checked: {len(report)} declaration(s) "
          f"to make 'let', {len(notes)} not checked")
    if args.apply:
        apply(report)
        return 0
    return 1 if report else 0


if __name__ == "__main__":
    sys.exit(main())
