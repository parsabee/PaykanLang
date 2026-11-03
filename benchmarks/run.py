#!/usr/bin/env python3
"""Runs the `parallel for` benchmarks over a range of thread counts.

    python3 benchmarks/run.py                 # build, run everything, write results/
    python3 benchmarks/run.py sum compute     # some of them
    python3 benchmarks/run.py --threads 1,4,16 --no-build

Each program times its own loop (benchmarks/bench.pkn, a monotonic clock)
and prints one line: `name k=v ... ns_min=N ns_mean=M check=C`.  This script
builds the programs with `paykan build`, runs each under OMP_NUM_THREADS =
1, 2, 4, ... and records, per run:

- ns_min, ns_mean: the loop's best and mean time (the program's own clock);
- wall_ns, cpu_ns: the timed loops' wall time and the process's CPU time
  over them (clock(): user + system, every thread), so cpu / (wall *
  threads) is how busy the threads were during the loops -- with a thread's
  spinning after a loop (KMP_BLOCKTIME, --blocktime) counted as busy.  The
  process's whole wall and CPU time (wait4's rusage) are recorded too;
- speedup = ns_min(1 thread) / ns_min(t threads), efficiency = speedup / t.

Results go to benchmarks/results/<host>.json and <host>.md (a table).
"""
import argparse
import json
import os
import platform
import re
import resource
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BENCH = ROOT / "benchmarks"
BIN = BENCH / "bin"
PAYKAN = ROOT / "build" / "bin" / "paykan"

# name -> (arguments, what the row says)
PROGRAMS = {
    "sum": (["16777216", "5"], "reduction, 16M loads of a 128 KB array"),
    "dot": (["16777216", "5"], "reduction, two loads per iteration"),
    "sum_buf": (["67108864", "3"], "reduction over a frozen 512 MB buffer, one `at` per iteration"),
    "sum_buf_1m": (["1048576", "5"], "the same over 8 MB, in L2"),
    "dot_buf": (["33554432", "3"], "dot product over two frozen 256 MB buffers"),
    "compute": (["16384", "5", "2000"], "2000 multiply-adds per element"),
    "saxpy": (["16384", "1000"], "a 16K-element loop, a store per element"),
    "overhead": (["4096", "2000"], "a 4K-element loop: fork, chunk, join"),
    "reduce_counter": (["4194304", "5", "1"], "count by reduction"),
    "atomic_counter": (["4194304", "5", "1"], "count on one atomic cell"),
    "atomic_counter_16": (["4194304", "5", "16"], "one atomic add per 16 iterations"),
    "atomic_counter_256": (["4194304", "5", "256"], "one atomic add per 256 iterations"),
}
# a row's program: the name up to a trailing `_<n>` or `_<n>m`
def program_of(name):
    m = re.match(r"(.*?)(_\d+m?)?$", name)
    return m.group(1)


def sysctl(key):
    try:
        return subprocess.run(["sysctl", "-n", key], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def machine():
    return {
        "cpu": sysctl("machdep.cpu.brand_string") or platform.processor(),
        "cores": os.cpu_count(),
        "perf_cores": sysctl("hw.perflevel0.physicalcpu"),
        "eff_cores": sysctl("hw.perflevel1.physicalcpu"),
        "cache_line": sysctl("hw.cachelinesize"),
        "l1d": sysctl("hw.l1dcachesize"),
        "l2_perf": sysctl("hw.perflevel0.l2cachesize"),
        "os": platform.platform(),
    }


def build(names):
    BIN.mkdir(exist_ok=True)
    for n in names:
        src = BENCH / f"{n}.pkn"
        out = BIN / n
        if out.exists() and out.stat().st_mtime > max(src.stat().st_mtime, (BENCH / "bench.pkn").stat().st_mtime):
            continue
        print(f"building {n}", file=sys.stderr)
        r = subprocess.run([str(PAYKAN), "build", "-o", str(out), str(src)], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"{n}: {r.stderr or r.stdout}")


def run(name, args, threads, blocktime):
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    if blocktime is not None:
        env["KMP_BLOCKTIME"] = blocktime
    exe = BIN / program_of(name)
    r, w = os.pipe()
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        os.dup2(w, 1)
        os.close(r)
        os.execve(str(exe), [str(exe)] + args, env)
    os.close(w)
    out = b""
    while True:
        chunk = os.read(r, 65536)
        if not chunk:
            break
        out += chunk
    os.close(r)
    _, status, ru = os.wait4(pid, 0)
    wall = time.perf_counter() - t0
    if status != 0:
        sys.exit(f"{name} exited with status {status}: {out.decode(errors='replace')}")
    fields = dict(kv.split("=", 1) for kv in out.decode().split()[1:])
    return {
        "name": name, "threads": threads, "blocktime": blocktime,
        "ns_min": int(fields["ns_min"]), "ns_mean": int(fields["ns_mean"]),
        "check": fields["check"], "wall_s": wall,
        "cpu_s": ru.ru_utime + ru.ru_stime,
        # the timed loops alone: the program's clock and its CPU time
        "loop_wall_s": int(fields["wall_ns"]) / 1e9,
        "loop_cpu_s": int(fields["cpu_ns"]) / 1e9,
    }


def table(rows, threads_list):
    by = {}
    for r in rows:
        by.setdefault(r["name"], {})[r["threads"]] = r
    lines = []
    head = "| program | " + " | ".join(f"{t} thr" for t in threads_list) + " |"
    lines.append(head)
    lines.append("|" + "---|" * (len(threads_list) + 1))
    for name, per in by.items():
        base = per[threads_list[0]]["ns_min"]
        cells = []
        for t in threads_list:
            r = per.get(t)
            if not r:
                cells.append("-")
                continue
            ms = r["ns_min"] / 1e6
            sp = base / r["ns_min"]
            util = r["loop_cpu_s"] / (r["loop_wall_s"] * t) if r["loop_wall_s"] > 0 else 0
            cells.append(f"{ms:.3f} ms, {sp:.1f}x, eff {sp / t:.2f}, busy {util:.2f}")
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--threads", default="1,2,4,8,12,16")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--blocktime", help="KMP_BLOCKTIME for the runs (ms, or infinite): how long a "
                    "thread spins after a loop before it sleeps; libomp's default is 0 on a hybrid CPU")
    ap.add_argument("--repeat", type=int, default=3, help="process runs per point; the best is kept")
    ap.add_argument("--out", help="the results' file name (default: the host, plus the "
                    "programs' names when a selection is run)")
    a = ap.parse_args()
    names = a.names or list(PROGRAMS)
    if not a.out:
        a.out = platform.node().split(".")[0] + ("-" + "+".join(a.names) if a.names else "")
    threads_list = [int(t) for t in a.threads.split(",")]
    if not a.no_build:
        build(sorted({program_of(n) for n in names}))
    rows = []
    for name in names:
        args, _ = PROGRAMS[name]
        for t in threads_list:
            best = None
            for _ in range(a.repeat):
                r = run(name, args, t, a.blocktime)
                if best is None or r["ns_min"] < best["ns_min"]:
                    best = r
            rows.append(best)
            print(f"{name:20s} {t:2d} thr  ns_min={best['ns_min']:>12d}  wall={best['wall_s']:.3f}s  "
                  f"cpu={best['cpu_s']:.3f}s  check={best['check']}", file=sys.stderr)
    out_dir = BENCH / "results"
    out_dir.mkdir(exist_ok=True)
    suffix = f"-blocktime{a.blocktime}" if a.blocktime else ""
    (out_dir / f"{a.out}{suffix}.json").write_text(json.dumps({"machine": machine(), "rows": rows}, indent=1))
    md = [f"# {a.out}{suffix}", "", "```", json.dumps(machine(), indent=1), "```", "",
          "Each cell: the loop's best time, its speedup over one thread, the",
          "efficiency (speedup / threads), and how busy the threads were over the",
          "timed loops (CPU time / (wall time * threads); a thread spinning after",
          "a loop, KMP_BLOCKTIME, counts as busy).", "", table(rows, threads_list), ""]
    (out_dir / f"{a.out}{suffix}.md").write_text("\n".join(md))
    print("\n".join(md[-2:]))


if __name__ == "__main__":
    main()
