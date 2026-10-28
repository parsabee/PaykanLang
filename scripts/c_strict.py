#!/usr/bin/env python3
"""Check that the C `paykan --emit-c` prints is strict ISO C11 on every C compiler.

For every runnable program of the samples corpus (samples_parity.py's list)
and every compiler named with --cc, the whole-program `paykan --emit-c`
output (the mlir backend's C, through EmitC) is compiled with STRICT_FLAGS at
each --opt level, linked with a copy of the runtime (src/Runtime/*.c) built
by the same compiler with the same flags and with the compiled code of the
modules it imports (`paykan pkm extract --object` of each module file
`paykan --list-modules` names, as docs/c-backend.md builds it: the standard
library's, the program's cache entries and prebuilt modules', as archives),
and run.  One more program imports a module only as a prebuilt module file,
from a --module-path directory: the 13_pkm sample's.  Every executable must
exit like `paykan run --track-heap` does, print the same stdout and stderr
(apart from the heap statistics, and from the warnings `paykan run` prints
while it compiles the program, before running it: the executable, compiled
already, prints none) and end with 0 live heap blocks.

Usage:
    c_strict.py --paykan build/bin/paykan --cc gcc --cc clang \\
        [--opt -O0 --opt -O2] [-v]
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
import samples_parity as sp  # noqa: E402  (shares the corpus and checks)

ROOT = sp.ROOT
RUNTIME = ROOT / "src" / "Runtime"
STRICT_FLAGS = ["-std=c11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror"]
EXTRA = []  # --cflag
# What `paykan run` prints as it compiles the program, before running it:
# its warnings (a hidden copy of a slice's elements, docs/language/
# 02-basics.md "Evaluation order"), each with its notes, source lines and
# carets.
WARNINGS_RE = re.compile(r"\A(?:[^\n]*\d+:\d+: (?:warning|note): [^\n]*\n"
                         r"(?:[ \d]* \| [^\n]*\n)*)+")


def corpus():
    return sp.samples()


def rel(path):
    return Path(path).relative_to(ROOT)


def tool(cmd, **kw):
    return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, errors="replace", cwd=str(ROOT), **kw)


def exit_code(rc):
    # A signal death reads like `paykan run`'s 128 + signal.
    return 128 - rc if rc < 0 else rc


def run_exe(exe, sample, outdir):
    env = dict(os.environ, PAYKAN_TRACK_HEAP="1")
    p = subprocess.run([str(sample), *sp.sample_args(sample, outdir)],
                       executable=str(exe),
                       stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, errors="replace", cwd=str(ROOT), env=env)
    p.returncode = exit_code(p.returncode)
    return sp.normalise(p)


def compare(ref, got):
    rc, out, err, live = got
    problems = []
    if live != ref[3]:
        problems.append(f"live blocks {live}, expected {ref[3]}")
    if rc != ref[0]:
        problems.append(f"exit code {rc}, expected {ref[0]}")
    if out != ref[1]:
        problems.append("stdout differs")
    if err != ref[2]:
        problems.append("stderr differs")
    return problems


def build_runtime(cc, opt, outdir):
    """The runtime's objects built by @p cc with the strict flags."""
    objs = []
    for src in sorted(RUNTIME.glob("*.c")):
        obj = Path(outdir) / f"{src.stem}.o"
        p = tool([cc, *STRICT_FLAGS, *EXTRA, opt, "-c", str(src), "-o",
                  str(obj)])
        if p.returncode != 0:
            raise SystemExit(f"FAIL runtime {src.name} [{cc} {opt}]:\n"
                             f"{p.stderr}")
        objs.append(str(obj))
    return objs


def module_archive(paykan, pkms, outdir, name):
    """An archive of the compiled code of the module files @p pkms (those
    without any are left out: their CODE is all of it), or None."""
    objs = []
    for i, pkm in enumerate(pkms):
        obj = Path(outdir) / f"{name}-{i}.o"
        p = tool([paykan, "pkm", "extract", "--object", str(pkm), "-o",
                  str(obj)])
        if p.returncode == 0:
            objs.append(str(obj))
        elif "has no compiled code" not in p.stderr:
            raise SystemExit(f"FAIL pkm extract {pkm}:\n{p.stderr}")
    if not objs:
        return None
    archive = Path(outdir) / f"lib{name}.a"
    p = tool(["ar", "rcs", str(archive), *objs])
    if p.returncode != 0:
        raise SystemExit(f"FAIL ar {archive}:\n{p.stderr}")
    return str(archive)


def stdlib_dir(paykan):
    """The standard library's directory, as `paykan --version` names it."""
    version = tool([paykan, "--version"])
    for line in version.stdout.splitlines():
        if line.startswith("standard library: "):
            return Path(line[len("standard library: "):]).resolve()
    raise SystemExit("FAIL: `paykan --version` names no standard library")


def stdlib_archive(paykan, outdir):
    """The standard library's compiled code, as one archive."""
    return module_archive(paykan, sorted(stdlib_dir(paykan).glob("**/*.pkm")),
                          outdir, "stdlib")


def prebuilt_import(paykan, outdir):
    """The 13_pkm sample with geometry::shapes only as a prebuilt module file
    in a --module-path directory, which the C build must find as well as the
    cache entries: its main.pkn and the flags it is compiled with."""
    sample = ROOT / "samples" / "imports" / "13_pkm"
    root = Path(outdir) / "prebuilt"
    (root / "lib" / "geometry").mkdir(parents=True)
    (root / "prog").mkdir()
    shutil.copy(sample / "main.pkn", root / "prog" / "main.pkn")
    shutil.copy(sample / "geometry" / "shapes.pkn", root / "shapes.pkn")
    p = tool([paykan, "--emit-pkm", "--module-name=geometry::shapes",
              str(root / "shapes.pkn"), "-o",
              str(root / "lib" / "geometry" / "shapes.pkm")])
    if p.returncode != 0:
        raise SystemExit(f"FAIL --emit-pkm geometry::shapes:\n{p.stderr}")
    return root / "prog" / "main.pkn", [f"--module-path={root / 'lib'}"]


def check_sample(args, sample, idx, runtimes, outdir, flags):
    """[(label, problems)] for one sample, compiled with paykan @p flags."""
    results = []
    objects = sp.object_flags(sample, outdir)
    ref_p = tool([args.paykan, "--track-heap", *flags, *objects, str(sample),
                  *sp.sample_args(sample, outdir)])
    ref_p.stderr = WARNINGS_RE.sub("", ref_p.stderr, count=1)
    ref = sp.normalise(ref_p)
    if ref[3] not in (0, None):  # None: a panic aborts before the report
        return [("reference run", [f"live blocks {ref[3]} (exit {ref[0]})"])]
    emitted = tool([args.paykan, *flags, "--emit-c", str(sample)])
    if emitted.returncode != 0:
        return [("emit-c", [emitted.stderr.strip()])]
    c_file = Path(outdir) / f"s{idx}.c"
    c_file.write_text(emitted.stdout)
    # The compiled code of the modules the C declares, from the files
    # --list-modules names: the standard library's from its archive (made
    # once), the others' (cache entries, which --emit-c has just written,
    # and prebuilt modules) extracted here.
    listed = tool([args.paykan, *flags, "--list-modules", str(sample)])
    if listed.returncode != 0:
        return [("list-modules", [listed.stderr.strip()])]
    own = [f for f in map(Path, listed.stdout.splitlines())
           if args.stdlib_dir not in (ROOT / f).resolve().parents]
    local = module_archive(args.paykan, own, outdir, f"s{idx}")
    modules = [a for a in (local, args.stdlib) if a]
    # A sample's native C (#198) is held to the same flags, compiled with the
    # generated C.
    natives = [str(c) for c in sorted(sample.parent.glob("*.c"))
               if sample.parent.parent.name == "imports"]
    for ri, ((cc, opt), objs) in enumerate(runtimes.items()):
        label = f"emit-c {cc} {opt}"
        exe = Path(outdir) / f"s{idx}-rt{ri}"
        p = tool([cc, *STRICT_FLAGS, *EXTRA, opt, "-I", str(RUNTIME),
                  str(c_file), *natives, *objs, *modules, "-lm", "-o",
                  str(exe)])
        if p.returncode != 0:
            results.append((label, ["compile failed:\n" + p.stderr]))
            continue
        results.append((label, compare(ref, run_exe(exe, sample, outdir))))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--cc", action="append", required=True,
                    help="a C compiler for the emit-c check (repeatable)")
    ap.add_argument("--opt", action="append",
                    help="an optimization level for the emit-c check "
                         "(repeatable; default -O0 and -O2)")
    ap.add_argument("--cflag", action="append", default=[],
                    help="an extra flag for the emit-c check's compiles and "
                         "links, e.g. -fsanitize=function (repeatable)")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    opts = args.opt or ["-O0", "-O2"]
    EXTRA.extend(args.cflag)

    with tempfile.TemporaryDirectory(prefix="paykan-c-strict-") as outdir:
        args.stdlib_dir = stdlib_dir(args.paykan)
        args.stdlib = stdlib_archive(args.paykan, outdir)
        runtimes = {}
        for ci, cc in enumerate(args.cc):
            for opt in opts:
                d = Path(outdir) / f"rt{ci}{opt}"
                d.mkdir()
                runtimes[(cc, opt)] = build_runtime(cc, opt, d)
        # (name, main file, paykan flags)
        cases = [(str(rel(f)), f, []) for f in corpus()]
        prebuilt, flags = prebuilt_import(args.paykan, outdir)
        cases.append(("samples/imports/13_pkm/main.pkn (geometry::shapes "
                      "prebuilt, --module-path)", prebuilt, flags))
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            all_results = list(pool.map(
                lambda t: check_sample(args, t[1][1], t[0], runtimes, outdir,
                                       t[1][2]),
                enumerate(cases)))

    failures = 0
    tally = {}
    for (name, _, _), results in zip(cases, all_results):
        for label, problems in results:
            ok, total = tally.get(label, (0, 0))
            tally[label] = (ok + (not problems), total + 1)
            if problems:
                failures += 1
                print(f"FAIL {name} [{label}]: " + "; ".join(problems))
            elif args.verbose:
                print(f"ok   {name} [{label}]")
    for label, (ok, total) in tally.items():
        print(f"{ok}/{total} {label}")
    print(f"{len(cases)} programs, flags: {' '.join(STRICT_FLAGS)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
