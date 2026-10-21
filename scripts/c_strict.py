#!/usr/bin/env python3
"""Check that the C `paykan --emit-c` prints is strict ISO C11 on every C compiler.

For every runnable program of the samples corpus (samples_parity.py's list)
and every compiler named with --cc, the whole-program `paykan --emit-c`
output (the mlir backend's C, through EmitC) is compiled with STRICT_FLAGS at
each --opt level, linked with a copy of the runtime (src/Runtime/*.c) built
by the same compiler with the same flags, and run.  Every executable must
exit like `paykan run --track-heap` does, print the same stdout and stderr
(apart from the heap statistics) and end with 0 live heap blocks.

Usage:
    c_strict.py --paykan build/bin/paykan --cc gcc --cc clang \\
        [--opt -O0 --opt -O2] [-v]
"""

import argparse
import os
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


def check_sample(args, sample, idx, runtimes, outdir):
    """[(label, problems)] for one sample."""
    results = []
    objects = sp.object_flags(sample, outdir)
    ref_p = tool([args.paykan, "--track-heap", *objects, str(sample),
                  *sp.sample_args(sample, outdir)])
    ref = sp.normalise(ref_p)
    if ref[3] not in (0, None):  # None: a panic aborts before the report
        return [("reference run", [f"live blocks {ref[3]} (exit {ref[0]})"])]
    emitted = tool([args.paykan, "--emit-c", str(sample)])
    if emitted.returncode != 0:
        return [("emit-c", [emitted.stderr.strip()])]
    c_file = Path(outdir) / f"s{idx}.c"
    c_file.write_text(emitted.stdout)
    # A sample's native C (#198) is held to the same flags, compiled with the
    # generated C.
    natives = [str(c) for c in sorted(sample.parent.glob("*.c"))
               if sample.parent.parent.name == "imports"]
    for ri, ((cc, opt), objs) in enumerate(runtimes.items()):
        label = f"emit-c {cc} {opt}"
        exe = Path(outdir) / f"s{idx}-rt{ri}"
        p = tool([cc, *STRICT_FLAGS, *EXTRA, opt, "-I", str(RUNTIME),
                  str(c_file), *natives, *objs, "-lm", "-o", str(exe)])
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
        runtimes = {}
        for ci, cc in enumerate(args.cc):
            for opt in opts:
                d = Path(outdir) / f"rt{ci}{opt}"
                d.mkdir()
                runtimes[(cc, opt)] = build_runtime(cc, opt, d)
        files = corpus()
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            all_results = list(pool.map(
                lambda t: check_sample(args, t[1], t[0], runtimes, outdir),
                enumerate(files)))

    failures = 0
    tally = {}
    for sample, results in zip(files, all_results):
        for label, problems in results:
            ok, total = tally.get(label, (0, 0))
            tally[label] = (ok + (not problems), total + 1)
            if problems:
                failures += 1
                print(f"FAIL {rel(sample)} [{label}]: " + "; ".join(problems))
            elif args.verbose:
                print(f"ok   {rel(sample)} [{label}]")
    for label, (ok, total) in tally.items():
        print(f"{ok}/{total} {label}")
    print(f"{len(files)} programs, flags: {' '.join(STRICT_FLAGS)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
