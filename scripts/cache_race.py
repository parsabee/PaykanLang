#!/usr/bin/env python3
"""Concurrent builds that share a module cache never use each other's
entries (issue #134).

Two `paykan` processes that differ in their flags (-O0 and -O2) build the
same multi-module project at the same time, many times over, for `run` and
`build`, and every run must link and print the right output.  Every other
iteration edits a module, so both processes rebuild its cached bitcode at
the same moment (the cache-miss path); the rest find their entries cached.
(#134 was found in the former C backend's object cache, whose `.key` was
checked and its `.o` linked later: a concurrent build with other flags could
replace the object in between.)

Usage:
    cache_race.py --paykan build/bin/paykan [--iterations 30] \\
        [--backend mlir]
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
  view fn area() -> int { return util::twice(self.w * self.h) / 2; }
}
impl Rect { fn __init__(w: int, h: int) { self.w = w; self.h = h; } }
fn describe(r: Rect) -> Str { return "rect " + Str(r.area()); }
"""
MAIN = """import ::io;
import lib::util;
import geometry::shapes;
fn main() -> int {
  r = shapes::Rect(3, 4);
  io::println(shapes::describe(r));
  io::println(Str(util::tag()));
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
    backends = args.backend or ["mlir"]

    with tempfile.TemporaryDirectory(prefix="paykan_cache_race_") as d:
        tmp = Path(d)
        base = dict(os.environ)
        total = runs = 0
        for backend in backends:
            pairs = [("-O0 vs -O2", [("-O0", ["-O0"], base),
                                     ("-O2", ["-O2"], base)])]
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
