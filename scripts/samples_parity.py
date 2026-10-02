#!/usr/bin/env python3
"""Run the samples corpus on every enabled backend and compare the results.

Every runnable sample (samples/codegen, samples/leak-check, and each
samples/imports/<dir>/main.pkn) is run with `--track-heap` on each backend
named on the command line.  The backends must agree on stdout, on the exit
code and on the number of live heap blocks (which must be zero), and must
produce the same stderr apart from the heap statistics.  Printed object
addresses (`Foo@0x...`) are normalised before comparing.

Usage:
    samples_parity.py --paykan build/bin/paykan --backend llvm --backend c

With a single backend the script only checks that every sample runs
successfully with zero live blocks.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADDR_RE = re.compile(r"@0x[0-9a-fA-F]+")
LIVE_RE = re.compile(r"^  live blocks\s*:\s*(-?\d+)$", re.M)
STATS_RE = re.compile(r"^paykan heap stats:\n(?:  .*\n?)*", re.M)


def samples():
    files = sorted((ROOT / "samples" / "codegen").glob("*.pkn"))
    files += sorted((ROOT / "samples" / "leak-check").glob("*.pkn"))
    for d in sorted((ROOT / "samples" / "imports").iterdir()):
        main = d / "main.pkn"
        if main.is_file() and not d.name.startswith("err_"):
            files.append(main)
    return files


def run(paykan, backend, sample):
    cmd = [paykan, f"--backend={backend}", "--track-heap", str(sample), "a", "b"]
    p = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, errors="replace", cwd=str(ROOT))
    out = ADDR_RE.sub("@ADDR", p.stdout)
    m = LIVE_RE.search(p.stderr)
    live = int(m.group(1)) if m else None
    err = ADDR_RE.sub("@ADDR", STATS_RE.sub("", p.stderr))
    return p.returncode, out, err, live


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--backend", action="append", required=True,
                    help="a backend to run (repeatable)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    failures = 0
    for sample in samples():
        rel = sample.relative_to(ROOT)
        results = {b: run(args.paykan, b, sample) for b in args.backend}
        problems = []
        for b, (rc, out, err, live) in results.items():
            if live is None:
                problems.append(f"{b}: no heap report (exit {rc})")
            elif live != 0:
                problems.append(f"{b}: {live} live blocks")
        ref_name = args.backend[0]
        ref = results[ref_name]
        for b in args.backend[1:]:
            rc, out, err, _ = results[b]
            if rc != ref[0]:
                problems.append(f"exit code {ref_name}={ref[0]} {b}={rc}")
            if out != ref[1]:
                problems.append(f"stdout differs between {ref_name} and {b}")
            if err != ref[2]:
                problems.append(f"stderr differs between {ref_name} and {b}")
        if problems:
            failures += 1
            print(f"FAIL {rel}: " + "; ".join(problems))
            if args.verbose:
                for b, (rc, out, err, live) in results.items():
                    print(f"--- {b} (exit {rc}, live {live})\n{out}{err}")
        elif args.verbose:
            print(f"ok   {rel}")
    total = len(samples())
    print(f"{total - failures}/{total} samples at parity on {', '.join(args.backend)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
