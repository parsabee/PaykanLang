#!/usr/bin/env python3
"""Run the ownership prototype's samples (samples/ownership) on every backend.

Every `samples/ownership/*.pkn` runs with `--ownership --track-heap` on each
backend named on the command line.  It must exit with status 0, print exactly
its `// expect-stdout: <line>` lines, leave zero live heap blocks, and print
the same on every backend.  Every `samples/ownership/errors/*.pkn` must be
rejected by `--ownership --check-only` with the diagnostic of its
`// expect-error: <line>:<col>: error: <message>` comment.

These samples stay out of samples_parity.py's corpus: they need
`--ownership`, without which the new keywords are not reserved.

With --demo the script prints a transcript instead (each sample's source,
its output on each backend, and each error case with its message), and
still fails when a check fails.

Usage:
    ownership_demo.py --paykan build/bin/paykan --backend c --backend llvm
    ownership_demo.py --paykan build/bin/paykan --backend c --backend llvm --demo
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIR = ROOT / "samples" / "ownership"
LIVE_RE = re.compile(r"^  live blocks\s*:\s*(-?\d+)$", re.M)
STDOUT_RE = re.compile(r"^// expect-stdout:(?: (.*))?$", re.M)
ERROR_RE = re.compile(r"^// expect-error: (.*)$", re.M)
DIAG_RE = re.compile(r"^\S+:\d+:\d+: error: .*$", re.M)


def run(paykan, backend, sample):
    """(exit code, stdout, stderr, live blocks or None) of one run."""
    p = subprocess.run([paykan, "--ownership", f"--backend={backend}",
                        "--track-heap", str(sample.relative_to(ROOT))],
                       stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, errors="replace", cwd=str(ROOT))
    m = LIVE_RE.search(p.stderr)
    return p.returncode, p.stdout, p.stderr, int(m.group(1)) if m else None


def check_sample(paykan, backends, sample):
    """The problems of one runnable sample, and its output per backend."""
    text = sample.read_text()
    expected = "".join(line + "\n" for line in STDOUT_RE.findall(text))
    problems, outs = [], {}
    if not expected:
        problems.append("no // expect-stdout: lines")
    for b in backends:
        rc, out, err, live = run(paykan, b, sample)
        outs[b] = out
        if rc != 0:
            problems.append(f"{b}: exit {rc}\n{err}")
        if live != 0:
            problems.append(f"{b}: live blocks {live}")
        if out != expected:
            problems.append(f"{b}: stdout differs from expect-stdout:\n{out}")
    if len(set(outs.values())) > 1:
        problems.append("stdout differs between backends")
    return problems, outs


def check_error(paykan, sample):
    """The problems of one error sample, and the diagnostic it got."""
    rel = sample.relative_to(ROOT).as_posix()
    m = ERROR_RE.search(sample.read_text())
    if not m:
        return ["no // expect-error: line"], ""
    p = subprocess.run([paykan, "--ownership", "--check-only", rel],
                       stdin=subprocess.DEVNULL, capture_output=True,
                       text=True, errors="replace", cwd=str(ROOT))
    want = f"{rel}:{m.group(1)}"
    got = DIAG_RE.findall(p.stderr)
    problems = []
    if p.returncode == 0:
        problems.append("accepted by --check-only")
    if want not in got:
        problems.append(f"expected\n  {want}\ngot\n{p.stderr}")
    return problems, "\n".join(got)


def body(sample):
    """The sample's source without its leading comment and expect lines."""
    lines = sample.read_text().splitlines()
    while lines and (lines[0].startswith("//") or not lines[0].strip()):
        lines.pop(0)
    while lines and (lines[-1].startswith("// expect-") or
                     not lines[-1].strip()):
        lines.pop()
    return lines


def header(sample):
    lines = []
    for line in sample.read_text().splitlines():
        if not line.startswith("//"):
            break
        lines.append(line)
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--backend", action="append",
                    help="a backend to run (repeatable; default: c and llvm)")
    ap.add_argument("--demo", action="store_true",
                    help="print a transcript of every sample and error case")
    args = ap.parse_args()
    paykan = str(Path(args.paykan).resolve())
    backends = args.backend or ["c", "llvm"]
    samples = sorted(DIR.glob("*.pkn"))
    errors = sorted((DIR / "errors").glob("*.pkn"))
    failures = 0
    rule = "=" * 72

    for sample in samples:
        rel = sample.relative_to(ROOT).as_posix()
        problems, outs = check_sample(paykan, backends, sample)
        failures += bool(problems)
        if args.demo:
            print(f"{rule}\n{rel}\n{rule}")
            print("\n".join(header(sample) + [""] + body(sample)))
            for b in backends:
                print(f"\n--- paykan --ownership --backend={b} {rel}")
                print(outs[b], end="")
            print()
        for p in problems:
            print(f"FAIL {rel}: {p}")

    if args.demo:
        print(f"{rule}\nCompile errors the model catches "
              f"(paykan --ownership --check-only)\n{rule}")
    for sample in errors:
        rel = sample.relative_to(ROOT).as_posix()
        problems, diag = check_error(paykan, sample)
        failures += bool(problems)
        if args.demo:
            about = " ".join(line[3:] for line in header(sample))
            print(f"\n{sample.name}: {about}")
            print("\n".join("  | " + line for line in body(sample)))
            print("  " + diag)
        for p in problems:
            print(f"FAIL {rel}: {p}")

    total = len(samples) + len(errors)
    print(f"\n{total - failures}/{total} ownership samples ok "
          f"({len(samples)} programs on {', '.join(backends)}, "
          f"{len(errors)} error cases)")
    if not samples or not errors:
        print("error: no ownership samples found")
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
