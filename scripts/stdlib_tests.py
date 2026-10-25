#!/usr/bin/env python3
"""Run the standard library's tests (tests/Stdlib) on every enabled backend.

Each tests/Stdlib/*.pkn is a program that imports standard-library modules
(`import ::collections;`).  It is run with `--track-heap` on each backend
named on the command line, with $PAYKAN_STDLIB unset, so `paykan` finds the
standard library the build staged next to it.  A program passes when it
exits 0, prints exactly its `.expected` file and leaves zero live heap
blocks.

Each tests/Stdlib/panics/*.pkn starts with `// expect-panic: <message>`: it
passes when it exits non-zero and its stderr has the line
`paykan: <message>`.

Every program runs at each -O level (default: the compiler's, -O2), with
`paykan run`, or on one of the other two paths:

* --build: built with `paykan build` on each backend, and the executable
  run with PAYKAN_TRACK_HEAP=1;
* --cc <compiler> (repeatable): the C `paykan --emit-c` prints (the mlir
  backend's), compiled at that level with the runtime's sources built by
  the same compiler with the same flags (scripts/c_strict.py) and linked
  with the standard library's compiled code, and run with
  PAYKAN_TRACK_HEAP=1.

Each run works in a directory of its own, on a copy of the program there
(`paykan build` keeps the compiled standard library next to the program).

Usage:
    stdlib_tests.py --paykan build/bin/paykan --backend mlir [--backend <plugin>]
        [-O 0 -O 2] [--build | --cc cc [--cc clang]] [-j 4]
    stdlib_tests.py --paykan build/bin/paykan --backend mlir --update

--update rewrites the .expected files from the first backend's `paykan run`
output.
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
import c_strict  # noqa: E402  (builds the runtime and the stdlib's code)

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests" / "Stdlib"
LIVE_RE = re.compile(r"^  live blocks\s*:\s*(-?\d+)$", re.M)
PANIC_RE = re.compile(r"^// expect-panic: (.*)$", re.M)


def test_env(**extra):
    env = dict(os.environ, PAYKAN_NO_PLUGINS="1", **extra)
    env.pop("PAYKAN_STDLIB", None)
    return env


def sh(cmd, workdir, env=None):
    return subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, errors="replace", cwd=workdir,
                          env=env or test_env())


def failed(step, proc):
    """A run that never started: @p step failed with @p proc."""
    return subprocess.CompletedProcess(
        proc.args, proc.returncode or 1, "",
        f"{step} failed (exit {proc.returncode}):\n{proc.stderr}")


def run_exe(exe, workdir):
    return sh([str(exe)], workdir, test_env(PAYKAN_TRACK_HEAP="1"))


def run(args, path, program, opt, workdir):
    """@p program on @p path at @p opt ([] or ["-O<n>"]): `paykan run` on a
    backend, `paykan build` on one and the executable, or the C path."""
    kind, tool = path
    if kind == "run":
        return sh([args.paykan, f"--backend={tool}", *opt, "--track-heap",
                   "--no-module-cache", "run", str(program)], workdir)
    exe = Path(workdir) / program.stem
    if kind == "build":
        b = sh([args.paykan, f"--backend={tool}", *opt, "--no-module-cache",
                "-o", str(exe), "build", str(program)], workdir)
        return run_exe(exe, workdir) if b.returncode == 0 else failed("build", b)
    emitted = sh([args.paykan, *opt, "--no-module-cache", "--emit-c",
                  str(program)], workdir)
    if emitted.returncode != 0:
        return failed("--emit-c", emitted)
    c_file = Path(workdir) / f"{program.stem}.c"
    c_file.write_text(emitted.stdout)
    level = opt[0] if opt else "-O2"
    cc = sh([tool, *c_strict.STRICT_FLAGS, level, "-I", str(c_strict.RUNTIME),
             str(c_file), *args.runtimes[(tool, level)], args.stdlib, "-lm",
             "-o", str(exe)], workdir)
    return run_exe(exe, workdir) if cc.returncode == 0 else failed(tool, cc)


def check_program(proc, program, update):
    problems = []
    if proc.returncode != 0:
        problems.append(f"exit code {proc.returncode}")
    m = LIVE_RE.search(proc.stderr)
    if m is None:
        problems.append("no heap statistics on stderr")
    elif int(m.group(1)) != 0:
        problems.append(f"{m.group(1)} live heap blocks")
    expected = program.with_suffix(".expected")
    if update and proc.returncode == 0:
        expected.write_text(proc.stdout)
    if not expected.is_file():
        problems.append(f"no {expected.name}")
    elif expected.read_text() != proc.stdout:
        problems.append(f"stdout differs from {expected.name}")
    return problems


def check_panic(proc, message):
    problems = []
    if proc.returncode == 0:
        problems.append("exited 0, expected a panic")
    if f"paykan: {message}" not in proc.stderr.splitlines():
        problems.append(f"stderr has no line 'paykan: {message}'")
    return problems


def label(path, opt):
    kind, tool = path
    level = " " + opt[0] if opt else ""
    if kind == "run":
        return tool + level
    return f"{tool} {kind}{level}" if kind == "build" else f"C {tool}{level}"


def check(args, program, workdir):
    """[(label, problems, stderr)] for @p program on every path and level."""
    panic = PANIC_RE.search(program.read_text())
    results = []
    for path in args.paths:
        for opt in args.opts:
            rundir = tempfile.mkdtemp(prefix=program.stem + "-", dir=workdir)
            copy = Path(shutil.copy(program, rundir))
            proc = run(args, path, copy, opt, rundir)
            if panic:
                problems = check_panic(proc, panic.group(1))
            else:
                problems = check_program(
                    proc, program,
                    args.update and path == args.paths[0] and opt == args.opts[0])
            results.append((label(path, opt), problems, proc.stderr))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--paykan", required=True, help="the paykan binary")
    ap.add_argument("--backend", action="append", required=True,
                    help="a backend to run the tests on (repeatable)")
    ap.add_argument("-O", dest="opt", type=int, choices=range(4),
                    action="append",
                    help="an optimization level (repeatable; default: the "
                         "compiler's, -O2)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--build", action="store_true",
                      help="build executables with `paykan build` and run "
                           "those")
    mode.add_argument("--cc", action="append",
                      help="run the C `paykan --emit-c` prints, compiled "
                           "with this C compiler (repeatable)")
    ap.add_argument("-j", "--jobs", type=int, default=4)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the .expected files")
    args = ap.parse_args()
    if args.update and (args.build or args.cc):
        ap.error("--update rewrites the .expected files from `paykan run`")
    args.opts = [[f"-O{o}"] for o in args.opt] if args.opt else [[]]
    if args.cc:
        args.paths = [("c", cc) for cc in args.cc]
    else:
        args.paths = [("build" if args.build else "run", b)
                      for b in args.backend]

    programs = sorted(TESTS.glob("*.pkn")) + sorted(
        (TESTS / "panics").glob("*.pkn"))
    with tempfile.TemporaryDirectory(prefix="paykan-stdlib-") as workdir:
        if args.cc:
            args.stdlib = c_strict.stdlib_archive(args.paykan, workdir)
            args.runtimes = {}
            for cc in args.cc:
                for opt in args.opts:
                    level = opt[0] if opt else "-O2"
                    d = Path(workdir) / f"rt-{len(args.runtimes)}"
                    d.mkdir()
                    args.runtimes[(cc, level)] = c_strict.build_runtime(
                        cc, level, d)
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            all_results = list(pool.map(
                lambda program: check(args, program, workdir), programs))

    failures = 0
    total = 0
    for program, results in zip(programs, all_results):
        rel = program.relative_to(ROOT)
        for what, problems, stderr in results:
            total += 1
            if problems:
                failures += 1
                print(f"FAIL {rel} [{what}]: " + "; ".join(problems))
                for line in stderr.strip().splitlines()[:20]:
                    print("  " + line)
            else:
                print(f"ok   {rel} [{what}]")
    print(f"{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
