#!/usr/bin/env python3
"""Check where `paykan` finds the runtime (Toolchain.cpp, issue #124).

The binary under test is placed (hard-linked, else copied) into install-like
prefixes next to copies of the runtime, and `run` / `build` are checked on
every backend named with --backend:

* an installed binary (<prefix>/bin/paykan, outside the build tree) uses its
  own prefix's runtime even though the build tree it was built in still
  exists: a broken runtime in the prefix makes `build` fail on it;
* $PAYKAN_RUNTIME_DIR overrides the prefix;
* a binary inside the build tree (below its `.paykan-build-tree` marker) uses
  the build tree's runtime; one outside it with no runtime around does not,
  and reports that it cannot find one (unless the install location configured
  at build time has a runtime, which is then the fallback).

Usage:
    runtime_lookup.py --paykan build/bin/paykan \\
        --runtime-lib build/lib/libpaykan_runtime.a \\
        --runtime-header src/Runtime/Runtime.h --build-dir build \\
        --installed-lib /usr/local/lib/libpaykan_runtime.a \\
        --backend c [--backend llvm]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROGRAM = 'fn main() -> int {\n  println("runtime ok");\n  return 0;\n}\n'
EXPECTED = "runtime ok\n"
BROKEN_MARKER = "paykan-runtime-lookup-broken-prefix"
NOT_FOUND = "cannot find the Paykan runtime"


class Checker:
    def __init__(self, args, program, outdir):
        self.args = args
        self.program = program
        self.outdir = outdir
        self.failures = 0
        self.count = 0

    def place(self, prefix, runtime):
        """<prefix>/bin/paykan, plus a runtime under <prefix> when @p runtime
        is "good" or "broken"."""
        bindir = prefix / "bin"
        bindir.mkdir(parents=True)
        exe = bindir / "paykan"
        try:
            os.link(self.args.paykan, exe)
        except OSError:
            shutil.copy2(self.args.paykan, exe)
        if runtime:
            (prefix / "lib").mkdir()
            (prefix / "include" / "paykan").mkdir(parents=True)
            lib = prefix / "lib" / "libpaykan_runtime.a"
            header = prefix / "include" / "paykan" / "Runtime.h"
            if runtime == "good":
                shutil.copy2(self.args.runtime_lib, lib)
                shutil.copy2(self.args.runtime_header, header)
            else:
                lib.write_text(f"{BROKEN_MARKER}: not an archive\n")
                header.write_text(f"#error \"{BROKEN_MARKER}\"\n")
        return exe

    def paykan(self, exe, cmd, env_dir=None):
        env = dict(os.environ)
        env.pop("PAYKAN_RUNTIME_DIR", None)
        if env_dir is not None:
            env["PAYKAN_RUNTIME_DIR"] = str(env_dir)
        return subprocess.run([str(exe), *cmd], stdin=subprocess.DEVNULL,
                              capture_output=True, text=True,
                              errors="replace", env=env)

    def report(self, label, problem):
        self.count += 1
        if problem:
            self.failures += 1
            print(f"FAIL {label}: {problem}")
        elif self.args.verbose:
            print(f"ok   {label}")

    def expect_works(self, label, exe, backend, env_dir=None):
        """`run` and `build` (and the built executable) print EXPECTED."""
        r = self.paykan(exe, [f"--backend={backend}", "run",
                              str(self.program)], env_dir)
        self.report(f"{label}: {backend} run",
                    None if r.returncode == 0 and r.stdout == EXPECTED else
                    f"exit {r.returncode}\n{r.stdout}{r.stderr}")
        out = Path(tempfile.mkdtemp(dir=self.outdir)) / "prog"
        b = self.paykan(exe, ["build", f"--backend={backend}", "-o", str(out),
                              str(self.program)], env_dir)
        if b.returncode != 0:
            self.report(f"{label}: {backend} build",
                        f"exit {b.returncode}\n{b.stderr}")
            return
        p = subprocess.run([str(out)], stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, errors="replace")
        self.report(f"{label}: {backend} build",
                    None if p.returncode == 0 and p.stdout == EXPECTED else
                    f"built program exit {p.returncode}\n{p.stdout}{p.stderr}")

    def expect_build_fails(self, label, exe, backend, needle):
        """`build` fails with @p needle in its diagnostics."""
        out = Path(tempfile.mkdtemp(dir=self.outdir)) / "prog"
        b = self.paykan(exe, ["build", f"--backend={backend}", "-o", str(out),
                              str(self.program)])
        ok = b.returncode != 0 and needle in b.stderr
        self.report(f"{label}: {backend} build",
                    None if ok else f"expected a failure mentioning {needle}; "
                    f"exit {b.returncode}\n{b.stderr}")


def inside(path, root):
    try:
        Path(path).resolve().relative_to(Path(root).resolve())
        return True
    except ValueError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--runtime-lib", required=True,
                    help="the build tree's libpaykan_runtime.a")
    ap.add_argument("--runtime-header", required=True,
                    help="the runtime's Runtime.h")
    ap.add_argument("--build-dir", required=True,
                    help="the build tree (holds .paykan-build-tree)")
    ap.add_argument("--installed-lib", required=True,
                    help="the runtime archive's configured install location")
    ap.add_argument("--backend", action="append", required=True,
                    help="a backend that builds native programs (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    if not (build_dir / ".paykan-build-tree").is_file():
        print(f"FAIL {build_dir} has no .paykan-build-tree marker")
        return 1
    with tempfile.TemporaryDirectory(prefix="paykan-rtlookup-") as out, \
            tempfile.TemporaryDirectory(prefix="paykan-rtlookup-",
                                        dir=build_dir) as in_tree:
        outdir = Path(out)
        if inside(outdir, build_dir):
            print(f"FAIL the temporary directory {outdir} is inside the "
                  "build tree; set TMPDIR elsewhere")
            return 1
        program = outdir / "prog.pkn"
        program.write_text(PROGRAM)
        c = Checker(args, program, outdir)

        installed = c.place(outdir / "installed", "good")
        stale = c.place(outdir / "stale", "broken")
        bare = c.place(outdir / "bare", None)
        in_build = c.place(Path(in_tree) / "prefix", None)
        broken_lib = str(outdir / "stale" / "lib" / "libpaykan_runtime.a")
        for be in args.backend:
            c.expect_works("installed binary", installed, be)
            # The build tree still exists, but the prefix's own (here
            # broken) runtime wins.
            c.expect_build_fails("installed binary, stale build tree", stale,
                                 be,
                                 BROKEN_MARKER if be == "c" else broken_lib)
            c.expect_works("PAYKAN_RUNTIME_DIR over the prefix", stale, be,
                           env_dir=outdir / "installed")
            c.expect_works("binary inside the build tree", in_build, be)
            if not Path(args.installed_lib).exists():
                c.expect_build_fails("binary outside the build tree, no "
                                     "runtime", bare, be, NOT_FOUND)
        print(f"{c.count - c.failures}/{c.count} runtime lookup checks passed "
              f"on {', '.join(args.backend)}")
        return 1 if c.failures else 0


if __name__ == "__main__":
    sys.exit(main())
