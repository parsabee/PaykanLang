#!/usr/bin/env python3
"""Check that the C backend's output is strict ISO C11 on every C compiler.

For every runnable program of the samples corpus (samples_parity.py's list
plus example_program/*/main.pkn) and every compiler named with --cc:

* emit-c: the whole-program `paykan --backend=c --emit-c` output is compiled
  with STRICT_FLAGS at each --opt level, linked with a copy of the runtime
  (src/Runtime/*.c) built by the same compiler with the same flags, and run;
* build: `paykan --backend=c build` is run with $CC set to a wrapper that adds
  STRICT_FLAGS, so every per-module translation unit `build` and `run`
  compile is checked too (compilers named with --build-cc only: these link
  against the build tree's runtime, which a sanitizer build instruments for
  its own compiler).

Every executable must exit like `paykan --backend=c run --track-heap` does,
print the same stdout and stderr (apart from the heap statistics) and end
with 0 live heap blocks.

Usage:
    c_strict.py --paykan build/bin/paykan --cc gcc --cc clang \\
        [--build-cc gcc --build-cc clang] [--opt -O0 --opt -O2] [-v]
"""

import argparse
import os
import shlex
import stat
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import samples_parity as sp  # noqa: E402  (shares the corpus and checks)

ROOT = sp.ROOT
RUNTIME = ROOT / "src" / "Runtime"
STRICT_FLAGS = ["-std=c11", "-pedantic-errors", "-Wall", "-Wextra", "-Werror"]
ARGS = ["a", "b"]
EXTRA = []  # --cflag


def corpus():
    files = sp.samples()
    for d in sorted((ROOT / "example_program").iterdir()):
        if (d / "main.pkn").is_file():
            files.append(d / "main.pkn")
    return files


def rel(path):
    return Path(path).relative_to(ROOT)


def tool(cmd, **kw):
    return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, errors="replace", cwd=str(ROOT), **kw)


def exit_code(rc):
    # A signal death reads like `paykan run`'s 128 + signal.
    return 128 - rc if rc < 0 else rc


def run_exe(exe, sample):
    env = dict(os.environ, PAYKAN_TRACK_HEAP="1")
    p = subprocess.run([str(sample)] + ARGS, executable=str(exe),
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


def wrapper(cc, outdir, index):
    """An executable `$CC` that runs @p cc with the strict flags."""
    path = Path(outdir) / f"strict-cc{index}"
    path.write_text("#!/bin/sh\nexec " +
                    " ".join(shlex.quote(a) for a in [cc, *STRICT_FLAGS]) +
                    ' "$@"\n')
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP |
               stat.S_IXOTH)
    return str(path)


def check_sample(args, sample, idx, runtimes, wrappers, outdir):
    """[(label, problems)] for one sample."""
    results = []
    ref_p = tool([args.paykan, "--backend=c", "--track-heap", str(sample),
                  *ARGS])
    ref = sp.normalise(ref_p)
    if ref[3] not in (0, None):  # None: a panic aborts before the report
        return [("reference run", [f"live blocks {ref[3]} (exit {ref[0]})"])]
    emitted = tool([args.paykan, "--backend=c", "--emit-c", str(sample)])
    if emitted.returncode != 0:
        return [("emit-c", [emitted.stderr.strip()])]
    c_file = Path(outdir) / f"s{idx}.c"
    c_file.write_text(emitted.stdout)
    for ri, ((cc, opt), objs) in enumerate(runtimes.items()):
        label = f"emit-c {cc} {opt}"
        exe = Path(outdir) / f"s{idx}-rt{ri}"
        p = tool([cc, *STRICT_FLAGS, *EXTRA, opt, "-I", str(RUNTIME),
                  str(c_file), *objs, "-lm", "-o", str(exe)])
        if p.returncode != 0:
            results.append((label, ["compile failed:\n" + p.stderr]))
            continue
        results.append((label, compare(ref, run_exe(exe, sample))))
    for wi, (cc, wrap) in enumerate(wrappers.items()):
        label = f"build {cc}"
        exe = Path(outdir) / f"s{idx}-build{wi}"
        p = tool([args.paykan, "build", "--backend=c", "-o", str(exe),
                  str(sample)], env=dict(os.environ, CC=wrap))
        if p.returncode != 0:
            results.append((label, ["build failed:\n" + p.stderr]))
            continue
        results.append((label, compare(ref, run_exe(exe, sample))))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--cc", action="append", required=True,
                    help="a C compiler for the emit-c check (repeatable)")
    ap.add_argument("--build-cc", action="append", default=[],
                    help="a C compiler for the build check (repeatable)")
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
        wrappers = {cc: wrapper(cc, outdir, wi)
                    for wi, cc in enumerate(args.build_cc)}
        files = corpus()
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            all_results = list(pool.map(
                lambda t: check_sample(args, t[1], t[0], runtimes, wrappers,
                                       outdir),
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
