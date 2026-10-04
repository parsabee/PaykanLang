#!/usr/bin/env python3
"""Concurrent builds that share a module cache never link each other's
objects (issue #134).

Two `paykan` processes that differ in their C compile flags build the same
multi-module project at the same time, many times over, and every run must
link and print the right output.  Before #134 the C backend checked a
module's `.key` and linked its `.o` later; a concurrent build with other
flags could replace the object in between, so a plain build linked an
ASan-instrumented object (`undefined reference to __asan_init`).

Configurations raced against each other, for `run` and `build`:

* -O0 against -O2;
* the plain C compiler against the same compiler with -fsanitize=address
  (through a `CC` wrapper script, so the same `paykan` binary serves both;
  skipped when the C compiler cannot build ASan programs).

Every other iteration edits a module, so both processes rebuild it at the
same moment (the cache-miss path); the rest find their entries cached.  The
llvm backend is raced at -O0 against -O2 too when it is named.

Usage:
    cache_race.py --paykan build/bin/paykan [--iterations 30] \\
        --backend c [--backend llvm]
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# main -> shapes, util; shapes -> util (a shared import, compiled by both
# processes at once).  `salt` changes between iterations.
UTIL = "fn tag() -> int {{ return {salt}; }}\nfn twice(x: int) -> int {{ return x * 2; }}\n"
SHAPES = """import lib::util;
class Rect {
  w: int; h: int;
  fn __init__(w: int, h: int) { self.w = w; self.h = h; }
  fn area() -> int { return util::twice(self.w * self.h) / 2; }
}
fn describe(r: Rect) -> Str { return "rect " + Str(r.area()); }
"""
MAIN = """import lib::util;
import geometry::shapes;
fn main() -> int {
  r = shapes::Rect(3, 4);
  println(shapes::describe(r));
  println(Str(util::tag()));
  return 0;
}
"""


def expected(salt):
    return "rect 12\n%d\n" % salt


def write_project(root, salt):
    (root / "lib").mkdir(parents=True, exist_ok=True)
    (root / "geometry").mkdir(parents=True, exist_ok=True)
    (root / "lib" / "util.pkn").write_text(UTIL.format(salt=salt))
    (root / "geometry" / "shapes.pkn").write_text(SHAPES)
    (root / "main.pkn").write_text(MAIN)


def asan_wrapper(tmp):
    """A `CC` that adds -fsanitize=address, or None when the C compiler
    cannot build and run an ASan program here."""
    cc = os.environ.get("CC") or "cc"
    probe = tmp / "probe.c"
    probe.write_text("int main(void) { return 0; }\n")
    exe = tmp / "probe"
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    try:
        if subprocess.run([cc, "-fsanitize=address", str(probe), "-o", str(exe)],
                          capture_output=True).returncode != 0:
            return None
        if subprocess.run([str(exe)], capture_output=True, env=env).returncode != 0:
            return None
    except OSError:
        return None
    wrapper = tmp / "cc-asan"
    wrapper.write_text('#!/bin/sh\nexec %s -fsanitize=address "$@"\n' % cc)
    wrapper.chmod(0o755)
    return str(wrapper)


def race(args, backend, root, configs, mode, tmp):
    """Run the configurations concurrently args.iterations times; return the
    number of failed runs."""
    failures = 0
    salt = 0
    for i in range(args.iterations):
        if i % 2 == 0:
            salt += 1
            write_project(root, salt)
        procs = []
        for n, (name, flags, env) in enumerate(configs):
            cmd = [args.paykan, "--backend=" + backend] + flags
            exe = None
            if mode == "build":
                exe = tmp / ("prog-%s-%d" % (backend, n))
                cmd += ["-o", str(exe), "build"]
            cmd.append(str(root / "main.pkn"))
            procs.append((name, exe, env, subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                env=env, text=True)))
        for name, exe, env, p in procs:
            out, _ = p.communicate()
            if p.returncode == 0 and exe is not None:
                r = subprocess.run([str(exe)], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, env=env, text=True)
                out = r.stdout
                p.returncode = r.returncode
            if p.returncode != 0 or out != expected(salt):
                failures += 1
                print("FAIL %s %s %s (iteration %d, exit %d):\n%s" %
                      (backend, mode, name, i, p.returncode, out[-2000:]))
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--paykan", required=True)
    ap.add_argument("--backend", action="append", default=[])
    ap.add_argument("--iterations", type=int, default=30)
    args = ap.parse_args()
    backends = args.backend or ["c"]

    with tempfile.TemporaryDirectory(prefix="paykan_cache_race_") as d:
        tmp = Path(d)
        base = dict(os.environ)
        # The ASan-wrapped programs run without LeakSanitizer, which does not
        # work everywhere (e.g. under ptrace); the runtime frees everything
        # anyway (--track-heap checks that elsewhere).
        asan_env = dict(base)
        asan_env["ASAN_OPTIONS"] = ":".join(
            filter(None, [base.get("ASAN_OPTIONS"), "detect_leaks=0"]))
        total = runs = 0
        for backend in backends:
            pairs = [("-O0 vs -O2", [("-O0", ["-O0"], base),
                                     ("-O2", ["-O2"], base)])]
            if backend == "c":
                wrapper = asan_wrapper(tmp)
                if wrapper:
                    pairs.append(("plain vs asan",
                                  [("plain", [], base),
                                   ("asan", [], dict(asan_env, CC=wrapper))]))
                else:
                    print("note: the C compiler cannot build ASan programs; "
                          "racing -O0 against -O2 only")
            for label, configs in pairs:
                for mode in ("run", "build"):
                    root = tmp / ("proj-%s-%s-%s" % (backend, mode,
                                                     label.replace(" ", "")))
                    failed = race(args, backend, root, configs, mode, tmp)
                    n = args.iterations * len(configs)
                    runs += n
                    total += failed
                    print("%s %s %s: %d/%d runs ok" %
                          (backend, mode, label, n - failed, n))
        if total:
            print("%d of %d concurrent runs failed" % (total, runs))
            return 1
        print("all %d concurrent runs linked and printed the right output" % runs)
        return 0


if __name__ == "__main__":
    sys.exit(main())
