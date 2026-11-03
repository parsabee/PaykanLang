# `parallel for` benchmarks

Programs that measure the concurrency prototype's `parallel for`
(`docs/design/concurrency.md`, `samples/concurrency/`): how it scales with
threads, how busy the threads are, what a loop costs to start, and what
sharing a cache line between cores costs.  Everything runs on the LLVM
path (`paykan build`): the loop's chunks become an `omp.wsloop` over at
most 256 chunks, scheduled dynamically one chunk at a time, each chunk
accumulating its reductions in slots of its own and writing them once,
and LLVM's OpenMP runtime (`libomp`) runs the chunks on a thread pool.

## The programs

Each program times its own loop with a monotonic clock
(`bench.pkn`: `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` through a
`native fn`), repeats it, and prints one line, `name k=v ... ns_min=N
ns_mean=M check=C`.  A parallel body reads fixed-size arrays (at most
16384 elements), so the array kernels go round a 128 KB array that lives
in L2; and it reads a frozen heap buffer in a box (`atomic const
mem::Box<mem::Buffer<T>>`: a buffer is a struct, a value, and takes no
qualifier of its own), which the `_buf` kernels use for data larger than
any cache.

| Program | Loop | What it shows |
|---|---|---|
| `sum.pkn` | `s += xs[i % 16384]` over n iterations | a reduction streaming a cache-resident array: one load per iteration |
| `dot.pkn` | `s += xs[i % 16384] * ys[i % 16384]` | two loads and a multiply-add per iteration |
| `sum_buf.pkn` | `s += xs.value().at(i)` over a frozen `atomic const mem::Box<mem::Buffer<float>>` of n elements | a reduction over a boxed heap buffer, 512 MB by default: data larger than any array, read with no counts; each element read is three calls |
| `dot_buf.pkn` | the same over two boxed buffers | six calls and a multiply-add per iteration |
| `compute.pkn` | `work` dependent multiply-adds per element, then `s += x` | compute bound: ideal scaling, and the performance/efficiency core mix |
| `saxpy.pkn` | `ys[i] = a * xs[i] + ys[i]`, n ≤ 16384 | a store at the loop's index; whether so short a loop pays for its fork and join |
| `overhead.pkn` | `reps` loops of n trivial iterations (n ≥ 4096, or the loop runs inline) | the cost of one `parallel for`: fork, chunking, join |
| `reduce_counter.pkn` | `if i % stride == 0 { c += 1; }` as a reduction | counting with no shared line |
| `atomic_counter.pkn` | the same on one `atomic int` | true sharing: one cache line bouncing between cores, at a rate set by `stride` |

`run.py` builds them, runs each under `OMP_NUM_THREADS` = 1, 2, 4, 8, 12,
16 (three process runs per point, the best kept), and records the loop's
time, the speedup over one thread, the efficiency (speedup / threads) and
how busy the threads were over the timed loops (the process's CPU time,
`clock()`, over wall time × threads).  After a loop a worker thread spins
for `KMP_BLOCKTIME` milliseconds before it sleeps, which counts as busy;
`libomp`'s default is 200 ms on most machines but 0 on a hybrid CPU (one
with performance and efficiency cores), where every loop then wakes the
workers from a kernel sleep.  `--blocktime 200` sets it.

```
python3 benchmarks/run.py                       # everything; build/benchmarks/results/<host>.md and .json
python3 benchmarks/run.py sum compute --threads 1,8,16
python3 benchmarks/run.py --blocktime 200       # workers spin 200 ms after a loop
OMP_NUM_THREADS=8 build/benchmarks/bin/atomic_counter 4194304 5 16
```

## What to look for

- **Scaling and utilization**: `compute` should approach the core count;
  the streaming kernels are bound by L2 bandwidth per core and by the
  reduction's per-chunk work.  With a static schedule (what
  `convert-scf-to-openmp` leaves, and what the prototype had before these
  benchmarks) every thread takes the same number of chunks and a thread on
  an efficiency core sets the loop's time; the dynamic schedule hands the
  256 chunks out as threads come free.
- **Fork and join**: `overhead` gives the fixed cost of a loop, which
  depends on the block time above far more than on anything the compiler
  emits; `saxpy` at 16384 elements shows the break-even.  A light body
  (no call, no loop of its own) under 4096 iterations runs inline, on the
  calling thread, since a fork cannot pay there; a body with a call or a
  loop may be heavy however short the loop, and always forks.
- **Cache coherence**: the prototype's reductions never share a line
  between threads (each chunk's partial is a slot on its own thread's
  stack, written to the shared partials array once, at the chunk's end),
  so `reduce_counter` scales; `atomic_counter` is what sharing one line
  costs, and `stride` 16 and 256 show the cost falling with the rate of
  sharing.  Writes through an `inout` array are at the loop's index only
  (Sema rejects any other index), so two threads write adjacent elements
  only at a chunk boundary, 255 lines at most.

## Results

Apple M3 Max (12 performance + 4 efficiency cores, 128-byte lines, 64 KB
L1d, 16 MB L2 per performance cluster), macOS 26, 2026-11-02; `run.py`
with the thread counts below, three process runs a point, the best kept
(the `static schedule (before)` rows from the build before the dynamic
schedule, the `KMP_BLOCKTIME=200` rows with `--blocktime 200`).  Each
cell is the loop's best time and its speedup over one thread; the
runner's own output, with efficiency and the busy fraction, is written
under `build/` and not kept here.

| program | schedule | 1 thr | 4 thr | 8 thr | 12 thr | 16 thr |
|---|---|---|---|---|---|---|
| sum | static schedule (before) | 10.12 ms (1.0x) | 2.82 ms (3.6x) | 1.44 ms (7.0x) | 1.01 ms (10.0x) | 1.11 ms (9.1x) |
| sum | dynamic schedule | 10.19 ms (1.0x) | 2.82 ms (3.6x) | 1.44 ms (7.1x) | 1.01 ms (10.1x) | 0.87 ms (11.7x) |
| sum | dynamic, KMP_BLOCKTIME=200 | 10.15 ms (1.0x) | 2.72 ms (3.7x) | 1.42 ms (7.1x) | 0.98 ms (10.3x) | 0.82 ms (12.3x) |
| dot | static schedule (before) | 10.90 ms (1.0x) | 3.02 ms (3.6x) | 1.55 ms (7.1x) | 1.09 ms (10.0x) | 1.15 ms (9.5x) |
| dot | dynamic schedule | 11.13 ms (1.0x) | 3.02 ms (3.7x) | 1.55 ms (7.2x) | 1.08 ms (10.3x) | 0.95 ms (11.7x) |
| dot | dynamic, KMP_BLOCKTIME=200 | 11.75 ms (1.0x) | 2.93 ms (4.0x) | 1.53 ms (7.7x) | 1.05 ms (11.2x) | 0.88 ms (13.3x) |
| compute | static schedule (before) | 88.08 ms (1.0x) | 23.70 ms (3.7x) | 12.28 ms (7.2x) | 8.52 ms (10.3x) | 8.24 ms (10.7x) |
| compute | dynamic schedule | 87.79 ms (1.0x) | 23.73 ms (3.7x) | 12.28 ms (7.1x) | 8.48 ms (10.4x) | 6.98 ms (12.6x) |
| compute | dynamic, KMP_BLOCKTIME=200 | 87.15 ms (1.0x) | 24.44 ms (3.6x) | 12.27 ms (7.1x) | 8.44 ms (10.3x) | 7.08 ms (12.3x) |
| reduce_counter | static schedule (before) | 2.07 ms (1.0x) | 0.59 ms (3.5x) | 0.33 ms (6.4x) | 0.25 ms (8.4x) | 0.34 ms (6.0x) |
| reduce_counter | dynamic schedule | 2.49 ms (1.0x) | 0.60 ms (4.2x) | 0.33 ms (7.5x) | 0.24 ms (10.3x) | 0.27 ms (9.2x) |
| reduce_counter | dynamic, KMP_BLOCKTIME=200 | 2.22 ms (1.0x) | 0.58 ms (3.8x) | 0.30 ms (7.5x) | 0.21 ms (10.7x) | 0.19 ms (11.4x) |
| atomic_counter | static schedule (before) | 7.81 ms (1.0x) | 48.09 ms (0.2x) | 139.51 ms (0.1x) | 123.59 ms (0.1x) | 250.98 ms (0.0x) |
| atomic_counter | dynamic schedule | 7.86 ms (1.0x) | 46.55 ms (0.2x) | 147.42 ms (0.1x) | 121.41 ms (0.1x) | 267.03 ms (0.0x) |
| atomic_counter | dynamic, KMP_BLOCKTIME=200 | 7.80 ms (1.0x) | 45.85 ms (0.2x) | 145.63 ms (0.1x) | 125.51 ms (0.1x) | 268.19 ms (0.0x) |
| atomic_counter_16 | static schedule (before) | 2.07 ms (1.0x) | 6.17 ms (0.3x) | 9.68 ms (0.2x) | 10.11 ms (0.2x) | 16.43 ms (0.1x) |
| atomic_counter_16 | dynamic schedule | 2.23 ms (1.0x) | 6.11 ms (0.4x) | 10.79 ms (0.2x) | 10.31 ms (0.2x) | 17.29 ms (0.1x) |
| atomic_counter_16 | dynamic, KMP_BLOCKTIME=200 | 2.22 ms (1.0x) | 6.03 ms (0.4x) | 10.58 ms (0.2x) | 10.01 ms (0.2x) | 17.30 ms (0.1x) |
| atomic_counter_256 | static schedule (before) | 2.23 ms (1.0x) | 0.72 ms (3.1x) | 0.81 ms (2.8x) | 1.02 ms (2.2x) | 1.22 ms (1.8x) |
| atomic_counter_256 | dynamic schedule | 2.35 ms (1.0x) | 0.75 ms (3.1x) | 0.85 ms (2.8x) | 1.01 ms (2.3x) | 1.23 ms (1.9x) |
| atomic_counter_256 | dynamic, KMP_BLOCKTIME=200 | 2.36 ms (1.0x) | 0.73 ms (3.2x) | 0.83 ms (2.8x) | 0.97 ms (2.4x) | 1.25 ms (1.9x) |
| saxpy | static schedule (before) | 5.4 µs (1.0x) | 14.9 µs (0.4x) | 28.2 µs (0.2x) | 46.3 µs (0.1x) | 77.1 µs (0.1x) |
| saxpy | dynamic schedule | 7.4 µs (1.0x) | 13.9 µs (0.5x) | 30.9 µs (0.2x) | 44.0 µs (0.2x) | 83.2 µs (0.1x) |
| saxpy | dynamic, KMP_BLOCKTIME=200 | 5.8 µs (1.0x) | 3.0 µs (1.9x) | 4.7 µs (1.2x) | 6.8 µs (0.9x) | 10.9 µs (0.5x) |
| overhead | static schedule (before) | 1.0 µs (1.0x) | 18.7 µs (0.1x) | 40.9 µs (0.0x) | 66.7 µs (0.0x) | 98.6 µs (0.0x) |
| overhead | dynamic schedule | 1.6 µs (1.0x) | 19.9 µs (0.1x) | 44.5 µs (0.0x) | 69.6 µs (0.0x) | 0.10 ms (0.0x) |
| overhead | dynamic, KMP_BLOCKTIME=200 | 1.1 µs (1.0x) | 3.0 µs (0.4x) | 8.0 µs (0.1x) | 10.6 µs (0.1x) | 15.1 µs (0.1x) |

What the numbers say:

- **Scaling.** `compute` reaches 10.4x on 12 threads (efficiency 0.86) and
  12.6x on 16; `sum` and `dot`, streaming a 128 KB array from L2 at one or
  two loads per iteration, 10x and 11.7x.  The threads are busy 0.84 to
  0.96 of the time over the loops at 16 threads (`clock()` over wall time
  x threads).  The rest is the fork, the join, and the efficiency cores:
  a chunk on one takes about twice as long.
- **The static schedule lost on the efficiency cores.**  With every thread
  taking 16 of the 256 chunks, 16 threads were *slower* than 12 (`sum`
  1.11 ms against 1.01 ms): the loop waited for the four threads on
  efficiency cores.  The dynamic schedule, one chunk at a time, is what
  the prototype emits now: 16 threads beat 12 on every scaling kernel
  (`sum` 0.87 ms, `compute` 6.98 ms against 8.24 ms), and nothing changes
  below 12 threads.  Results are identical whichever thread runs a chunk:
  the partials fold in chunk order.
- **Fork and join cost 6 µs per thread, or 1 µs, by the wait policy.**
  `libomp` lets a worker spin for `KMP_BLOCKTIME` ms after a loop before it
  sleeps; its default is 200 ms, but 0 on a hybrid CPU, so here every loop
  wakes 15 sleeping threads: 100 µs per `parallel for` at 16 threads, 20 µs
  at 4.  With `KMP_BLOCKTIME=200` a loop costs 15 µs at 16 threads and 3 µs
  at 4, and a 16384-element `saxpy` (5.8 µs serial) finally pays for itself
  at 4 threads (3.0 µs); with the default it never does.  Break-even for a
  trivial body with the default policy is roughly 100,000 iterations at 16
  threads.  The prototype leaves the policy to the environment; the design's
  own runtime (pinned loops) decides it for itself.
- **Cache coherence.**  The prototype's reductions share no line between
  threads: each chunk's partial is a slot on its own thread's stack, written
  to the shared array once (the generated loop keeps the accumulator in a
  register, one load and one add per iteration, `--emit-llvm`).  So
  `reduce_counter` scales like `sum`.  One `atomic int` touched every
  iteration is the opposite: 7.8 ms on one thread, 25 ms on two, 267 ms on
  16, 1,000x the reduction, each add costing about 60 ns while the line
  bounces between cores (true sharing).  One atomic add per 16 iterations
  still loses to one thread at every count; per 256 iterations it scales to
  3.2x on 4 threads and falls off after.  An `atomic` in a parallel body
  is for a result that cannot be a reduction, touched rarely.
- **False sharing** has nowhere to come from: an `inout` array is written
  at the loop's index only (Sema now rejects any other index, found while
  writing these), so two threads write neighbouring elements only at a
  chunk boundary, 255 lines in a loop at most.

**Boxed heap buffers** (the dynamic schedule, the default block time).  `sum_buf` reads 64M floats (512 MB), `sum_buf_1m`
1M (8 MB, in L2), `dot_buf` 32M of each of two buffers:

| program | 1 thr | 4 thr | 8 thr | 12 thr | 16 thr |
|---|---|---|---|---|---|
| sum_buf | 465.2 ms (1.0x) | 120.6 ms (3.9x) | 60.9 ms (7.6x) | 42.0 ms (11.1x) | 36.0 ms (12.9x) |
| sum_buf_1m | 7.2 ms (1.0x) | 1.9 ms (3.8x) | 1.0 ms (7.4x) | 0.7 ms (10.4x) | 0.6 ms (11.7x) |
| dot_buf | 475.9 ms (1.0x) | 122.9 ms (3.9x) | 62.1 ms (7.7x) | 42.8 ms (11.1x) | 36.7 ms (13.0x) |

- They scale like the compute-bound kernel (12.9x and 13.0x on 16
  threads, efficiency 0.81), because they *are* compute bound: an element
  read costs 6.9 ns on one thread, against 0.6 ns for a fixed-array
  element in `sum`, and the 8 MB buffer that fits in L2 reads no faster
  than the 512 MB one.  512 MB in 36 ms is 14 GB/s, a small fraction of
  what the memory system gives sixteen cores.  Where the time goes, in
  the emitted IR: the body calls the box's `value()`, the buffer's `at`
  (inlined) and the runtime's bounds-checked `PaykanBuffer_get`, and
  `at`'s hand-over of the buffer's object to `_get` as a `view` is held
  with a retain and a release: on a frozen object those read the count
  and call the runtime, which leaves it alone, so no count is written and
  no line bounces, but it is two calls and a compare per element.
- So a frozen buffer in a parallel body is correct and scales, and is
  the prototype's way to data larger than an array; measuring memory
  bandwidth through it waits on an element read the lowering emits
  itself (a bounds check and a load, as a fixed array gets) and on holds
  skipped under a frozen root, both of which the design has (the
  Counting section).

The `busy` column is the process's CPU time over wall time x threads,
from `clock()`: it counts the threads' work and their spinning alike, and
it lags a little behind running threads, so it is indicative, not exact.

What the prototype cannot measure yet: memory-bound scaling (a buffer
read is call bound, above), and anything about the design's own runtime.
