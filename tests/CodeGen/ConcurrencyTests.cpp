// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen / E2E tests for the concurrency prototype
// (docs/design/concurrency.md, samples/concurrency/README.md): tasks
// (`async fn`, `await`), `parallel for`, `atomic` and `locked` cells,
// `atomic shared` objects, `move` and the qualifiers at run time.  Each
// test compiles and runs a small program through the mlir backend's JIT, which
// loads MLIR's async runtime and LLVM's OpenMP runtime when a program uses
// them, and checks its exact output.
//
// Two things decide which tests check the heap:
//
// * An `atomic` cell, a `locked` cell and a frozen (`atomic shared`)
//   object are immortal in the prototype: their counts are PAYKAN_REF_MAX
//   and they are never freed (the samples README, "What is not").  A
//   program that makes one leaks it by design, so those tests run without
//   a LeakGuard and say so.
// * The tracking allocator's counters are plain integers shared by every
//   thread (src/Runtime/Heap.c).  A test that does check the heap keeps
//   the threads from allocating at once: a task that runs beside another
//   touches no object or Str, and a task that does is awaited before the
//   next such task starts.  A parallel body touches values only.
//
// The programs are deterministic: tasks run on a pool, so a program prints
// only after awaiting, and reductions fold in chunk order whatever the
// thread count.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// -- Tasks

// A call of an `async fn` starts a task and returns at once; `await` gives
// its result.  Three tasks run before any is awaited, a task awaits a task
// of its own, twenty tasks are started and awaited in a loop, an awaited
// call is an operand, and `async fn main` returns the exit code.  No task
// body allocates, so only main's thread touches the heap.
TEST(Concurrency, TasksAreStartedThenAwaited) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    async fn add(a: int, b: int) -> int { return a + b; }
    async fn square(n: int) -> int {
      let t = add(n, n);
      let d = await t;
      return d * n / 2;
    }
    async fn count(upto: int) -> int {
      s = 0;
      for i in 0..upto { s += i; }
      return s;
    }
    async fn main() -> int {
      let t = add(2, 3);
      io::println("add " + Str(await t));
      let a = square(3);
      let b = square(4);
      let c = square(5);
      io::println(Str(await a) + " " + Str(await b) + " " + Str(await c));
      total = 0;
      for i in 0..20 {
        let k = count(i);
        total += await k;
      }
      io::println("total " + Str(total));
      io::println("inline " + Str(await add(10, 20)));
      return await add(1, 2);
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "add 5\n9 16 25\ntotal 1140\ninline 30\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("TasksAreStartedThenAwaited");
}

// A task returns a reference it made, takes one by `move` and hands it
// back, and releases one it was given at its end.  Each task is awaited
// before the next starts, so one thread at a time touches the heap, and the
// object made on a worker is freed by main with nothing left over.
TEST(Concurrency, ATaskReturnsAReference) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Job { name: Str; n: int; }
    async fn mk(n: int) -> Job { return Job("job" + Str(n), n); }
    async fn pass(j: Job) -> Job { return j; }
    async fn weigh(j: Job) -> int { return j.n * 2; }
    fn main() -> int {
      let t = mk(7);
      let job = await t;
      io::println(job.name + " " + Str(job.n));
      let p = pass(move job);
      let back = await p;
      io::println(back.name);
      let w = weigh(move back);
      io::println(Str(await w));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "job7 7\njob7\n14\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ATaskReturnsAReference");
}

// -- parallel for

// The reductions: a sum, a product, two in one loop (an int and a float),
// `-=` over an inclusive range, and a dot product over `view` arrays.  Each
// chunk accumulates its own partial and the partials fold in chunk order,
// so the float sum is the same on every run.
TEST(Concurrency, ParallelForReductions) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn dot(xs: view int[], ys: view int[]) -> int {
      s = 0;
      parallel for i in 0..xs.len() { s += xs[i] * ys[i]; }
      return s;
    }
    fn main() -> int {
      s = 0;
      parallel for i in 0..10000 { s += i; }
      io::println("sum " + Str(s));
      p = 1;
      parallel for i in 1..20 { p *= i; }
      io::println("product " + Str(p));
      a = 0;
      b = 0.0;
      parallel for i in 0..5000 { a += i % 3; b += 0.5; }
      io::println("two " + Str(a) + " " + Str(b));
      d = 100;
      parallel for i in 0..=10 { d -= i; }
      io::println("minus " + Str(d));
      xs = [0; 6000];
      ys = [0; 6000];
      for i in 0..6000 { xs[i] = i; ys[i] = 2; }
      io::println("dot " + Str(dot(xs, ys)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "sum 49995000\n"
                      "product 121645100408832000\n"
                      "two 4999 2500\n"
                      "minus 45\n"
                      "dot 35994000\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ParallelForReductions");
}

// An array is written at the loop's index only: through an `inout`
// parameter, and a local array of the caller's own.  Every element is
// written by exactly one iteration, so the loop needs no reduction.
TEST(Concurrency, ParallelForWritesAnInoutArrayAtTheLoopsIndex) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn saxpy(a: float, xs: view float[], ys: inout float[]) {
      parallel for i in 0..xs.len() { ys[i] = a * xs[i] + ys[i]; }
    }
    fn main() -> int {
      xs = [0.0; 5000];
      ys = [1.0; 5000];
      for i in 0..5000 { xs[i] = float(i); }
      saxpy(2.0, xs, ys);
      io::println(Str(ys[0]) + " " + Str(ys[1]) + " " + Str(ys[4999]));
      zs = [0; 8192];
      parallel for i in 0..8192 { zs[i] = i * 2; }
      io::println(Str(zs[0]) + " " + Str(zs[4095]) + " " + Str(zs[8191]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1 3 9999\n0 8190 16382\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ParallelForWritesAnInoutArrayAtTheLoopsIndex");
}

// The trip counts the lowering treats differently: zero iterations run no
// chunk; a light body (no call, no loop) under 4096 iterations runs inline
// on the calling thread, where a fork cannot pay (benchmarks/README.md);
// over 4096 it forks; a body with a loop of its own is heavy and forks at
// any count.  The result is the same on every path.
TEST(Concurrency, ParallelForTripCounts) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      z = 0;
      parallel for i in 0..0 { z += 1; }
      io::println("none " + Str(z));
      n = 0;
      parallel for i in 0..100 { n += 1; }
      io::println("inline " + Str(n));
      m = 0;
      parallel for i in 0..5000 { m += 1; }
      io::println("forked " + Str(m));
      h = 0;
      parallel for i in 0..64 {
        k = 0;
        for j in 0..i { k += j; }
        h += k;
      }
      io::println("heavy " + Str(h));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "none 0\ninline 100\nforked 5000\nheavy 41664\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ParallelForTripCounts");
}

// -- atomic

// A cell of every kind: `int`, `i8` (wrapping on `+=` and `-=` like i8
// arithmetic), `u8`, `float`, `f32`, `bool` and `char`; a read is a load,
// `=` a store, `+=` and `-=` one atomic add.  The cells are immortal, so
// the program leaks them by design and the heap is not checked.
TEST(Concurrency, AtomicCellsOfEveryType) {
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      n: atomic int = 5;
      n += 10;
      n -= 3;
      i: atomic i8 = 120;
      i += 10;
      u: atomic u8 = 250;
      u += 10;
      f: atomic float = 1.5;
      f += 0.25;
      f -= 1.0;
      g: atomic f32 = 0.5;
      g += 0.25;
      b: atomic bool = False;
      b = True;
      c: atomic char = 'a';
      c = 'z';
      io::println(Str(n) + " " + Str(i) + " " + Str(u) + " " + Str(f) + " "
          + Str(g) + " " + Str(b) + " " + Str(c));
      i -= 5;
      u -= 10;
      io::println(Str(i) + " " + Str(u));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "12 -126 4 0.75 0.75 True z\n125 250\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// `exchange` returns the value found; `compareExchange(expected, desired)`
// stores and returns True only when the cell holds `expected`.  On an int,
// a float, a bool and a char.  Cells are immortal: no heap check.
TEST(Concurrency, AtomicExchangeAndCompareExchange) {
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      n: atomic int = 5;
      let old = n.exchange(100);
      let ok = n.compareExchange(100, 7);
      let no = n.compareExchange(100, 8);
      io::println(Str(old) + " " + Str(ok) + " " + Str(no) + " " + Str(n));
      f: atomic float = 2.0;
      let fo = f.exchange(0.5);
      let fok = f.compareExchange(0.5, 4.0);
      io::println(Str(fo) + " " + Str(f) + " " + Str(fok));
      b: atomic bool = False;
      let was = b.exchange(True);
      io::println(Str(was) + " " + Str(b));
      c: atomic char = 'a';
      let cok = c.compareExchange('a', 'b');
      let cno = c.compareExchange('a', 'c');
      let wc = c.exchange('q');
      io::println(Str(c) + " " + Str(cok) + " " + Str(cno) + " " + Str(wc));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 True False 7\n2 4 True\nFalse True\nq True False b\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// A first assignment from an atomic place takes its handle: `m = n`
// shares n's cell, `h = st.served` the field's, `k = fresh(3)` the new cell
// the call returns; a declaration typed with the number (`x: int = n`) and
// a `let` one (`let v = n`, a read-only copy) take its value.  Cells are
// immortal: no heap check.
TEST(Concurrency, AFirstAssignmentOfAnAtomicPlaceTakesItsHandle) {
  auto r = compileAndRun(R"(import ::io;
    class Stats { served: atomic int; }
    fn fresh(v: int) -> atomic int { return v; }
    fn main() -> int {
      n: atomic int = 5;
      m = n;
      m += 1;
      st = Stats(0);
      h = st.served;
      h += 2;
      k = fresh(3);
      k += 1;
      x: int = n;
      x += 10;
      let v = n;
      n += 1;
      io::println(Str(n) + " " + Str(m) + " " + Str(st.served) + " "
          + Str(k) + " " + Str(x) + " " + Str(v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7 7 2 4 16 6\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// A copy of an atomic place is another handle to the same cell: a second
// variable, an `atomic int` parameter, a handle taken from a field; a value
// returned into an `atomic` result is a new cell.  An `atomic` field of a
// frozen object still changes, from two tasks at once and from a parallel
// body.  Cells and the frozen object are immortal: no heap check.
TEST(Concurrency, AtomicHandlesShareACell) {
  auto r = compileAndRun(R"(import ::io;
    class Stats { name: Str; served: atomic int; }
    fn bump(c: atomic int, by: int) { c += by; }
    fn read(c: atomic int) -> int { return c; }
    fn fresh(v: int) -> atomic int { return v; }
    async fn hit(st: atomic shared Stats, n: int) -> int {
      for i in 0..n { st.served += 1; }
      return n;
    }
    fn main() -> int {
      a: atomic int = 1;
      b = a;
      b += 10;
      a += 100;
      io::println(Str(a) + " " + Str(b));
      bump(a, 5);
      io::println(Str(a) + " " + Str(b) + " " + Str(read(b)));
      k = fresh(3);
      k += 1;
      io::println(Str(k) + " " + Str(a));
      let st: atomic shared Stats = Stats("s", 0);
      let t1 = hit(st, 500);
      let t2 = hit(st, 500);
      let n = await t1 + await t2;
      st.served += 1;
      h = st.served;
      h -= 2;
      io::println(st.name + " " + Str(st.served) + " " + Str(n));
      parallel for i in 0..1000 { if i % 10 == 0 { st.served += 1; } }
      io::println(Str(st.served));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "111 111\n116 116 116\n4 116\ns 999 1000\n1099\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// -- locked

// A unique value flowing into a `locked` slot becomes a cell; its contents
// are reached only in a `lock` block, `inout` to change them and `view` to
// read them.  The handle copies into two tasks, which bump the counts under
// the lock, so every increment lands.  The cell is immortal: no heap check.
TEST(Concurrency, LockedCellIsReachedInLockBlocks) {
  auto r = compileAndRun(R"(import ::io;
    class Counts { hits: int[4]; total: int; }
    async fn record(counts: locked mut Counts, id: int, n: int) -> int {
      for i in 0..n {
        lock counts as c: inout {
          c.hits[(id + i) % 4] += 1;
          c.total += 1;
        }
      }
      return id;
    }
    fn main() -> int {
      let counts: locked mut Counts = Counts([0; 4], 0);
      lock counts as c: inout { c.total = 10; c.hits[1] = 2; }
      lock counts as v: view {
        io::println(Str(v.total) + " " + Str(v.hits[1]));
      }
      let a = record(counts, 1, 300);
      let b = record(counts, 2, 300);
      let ids = await a + await b;
      sum = 0;
      lock counts as v: view {
        for i in 0..4 { sum += v.hits[i]; }
        io::println(Str(v.total) + " " + Str(sum) + " " + Str(ids));
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10 2\n610 602 3\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// -- atomic shared

// A unique object flowing into an `atomic shared` slot is frozen with
// everything it holds (here a Str, an object and an array); the handle
// copies into three tasks, which read the graph with no count, and main
// reads it after them.  The frozen object is immortal: no heap check.
TEST(Concurrency, AtomicSharedObjectIsReadByTasks) {
  auto r = compileAndRun(R"(import ::io;
    class Limits { lo: int; hi: int; }
    class Config { name: Str; limits: Limits; weights: float[4]; }
    async fn score(cfg: atomic shared Config, x: int) -> int {
      if x < cfg.limits.lo || x > cfg.limits.hi { return 0; }
      return x * cfg.name.len() + int(cfg.weights[x % 4]);
    }
    fn main() -> int {
      let cfg: atomic shared Config =
          Config("demo", Limits(0, 100), [1.0, 2.0, 3.0, 4.0]);
      let a = score(cfg, 7);
      let b = score(cfg, 8);
      let c = score(cfg, 500);
      io::println(Str(await a) + " " + Str(await b) + " " + Str(await c));
      io::println(cfg.name + " " + Str(cfg.limits.lo) + ".."
          + Str(cfg.limits.hi));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "32 33 0\ndemo 0..100\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// A buffer is a struct, a value, and takes no qualifier; boxed, the box is
// frozen with the buffer and its elements, and a parallel body reads it
// through the box's `value()`: the prototype's way to data larger than a
// fixed-size array.  The body calls, so it is heavy and forks at 1000
// iterations.  The frozen box is immortal: no heap check.
TEST(Concurrency, AtomicSharedBoxedBufferIsReadInAParallelFor) {
  auto r = compileAndRun(R"(import ::io;
    import ::memory as mem;
    fn main() -> int {
      buf = mem::Buffer<float>();
      let grown = buf.grow(unsigned(1000));
      for i in 0..1000 { let e = buf.set(unsigned(i), float(i)); }
      let xs: atomic shared mem::Box<mem::Buffer<float>> = mem::Box(move buf);
      s = 0.0;
      parallel for i in 0..1000 { s += xs.value().at(unsigned(i)); }
      io::println(Str(s) + " " + Str(xs.value().len()) + " "
          + Str(xs.value().at(unsigned(999))));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "499500 1000 999\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// -- move

// A moved object is released exactly once, where it went: a `weak` local
// reads None once the callee has released the argument, and the caller's
// ended variable releases nothing.  A partial move `move h.job` takes the
// field out and ends the holder, whose teardown leaves the field alone; the
// object goes when its new owner does.  A struct value moves into a
// parameter and a unique field moves out of one.  Zero live blocks says
// nothing was released twice or never.
TEST(Concurrency, MovesReleaseTheObjectOnce) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Job { name: Str; }
    class Holder { job: Job; tag: Str; }
    struct Pair { left: Job; n: int; }
    fn show(w: Job?) -> Str {
      match w { j: Job { return j.name; } None { return "gone"; } }
    }
    fn keep(j: Job) -> int { return j.name.len(); }
    fn take(p: Pair) -> int { return p.left.name.len() + p.n; }
    fn main() -> int {
      let a = Job("abc");
      weak wa = a;
      let n = keep(move a);
      io::println(Str(n) + " " + show(wa));
      let h = Holder(Job("inner"), "t");
      weak wj = h.job;
      if True {
        let inner = move h.job;
        io::println(inner.name + " " + show(wj));
      }
      io::println(show(wj));
      let p = Pair(Job("left"), 1);
      io::println(Str(take(move p)));
      let r = Pair(Job("xy"), 2);
      weak wr = r.left;
      let l = move r.left;
      io::println(l.name + " " + show(wr));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 gone\ninner inner\ngone\n5\nxy xy\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("MovesReleaseTheObjectOnce");
}

// -- Qualifiers

// A `shared` reference aliases on its thread: a change through one name
// shows through the other, and the two compare equal.  A `const` object
// is read through a `view` local, a `view` parameter and a `view fn`, and
// converts into `shared`.
TEST(Concurrency, QualifiersAtRunTime) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node {
      id: int;
      name: Str;
      view fn label() -> Str { return self.name + Str(self.id); }
    }
    fn describe(n: view Node) -> Str { return n.name + " #" + Str(n.id); }
    fn main() -> int {
      let s: shared mut Node = Node(2, "s");
      let t = s;
      t.id = 22;
      s.name = "ss";
      io::println(describe(s) + " " + describe(t) + " " + Str(s == t));
      let a = Node(1, "a");
      let c: Node = move a;
      v: view Node = c;
      io::println(describe(c) + " " + v.label() + " " + c.label());
      let d: shared Node = c;
      io::println(describe(d));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ss #22 ss #22 True\na #1 a1 a1\na #1\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("QualifiersAtRunTime");
}

// A buffer of the enclosing function is written at the loop's variable and
// read beside the write in one statement, on every thread: the reads are
// loads and the write a call, and nothing counts the buffer's object, whose
// count is plain (a snapshot of the struct would race on it).
TEST(Concurrency, ParallelForWritesABufferOfTheEnclosingFunction) {
  auto r = compileAndRun(R"(import ::io;
    import ::memory as mem;
    fn main() -> int {
      let n = 200000;
      xs = mem::Buffer<float>();
      ys = mem::Buffer<float>();
      let gx = xs.grow(unsigned(n));
      let gy = ys.grow(unsigned(n));
      for i in 0..n { let e = xs.set(unsigned(i), float(i % 10)); }
      for r in 0..4 {
        parallel for i in 0..n {
          let u = unsigned(i);
          let e = ys.set(unsigned(i), 2.0 * xs.at(u) + ys.at(u) + float(ys.len() - unsigned(n)));
        }
      }
      s = 0.0;
      parallel for i in 0..n { s += ys.at(unsigned(i)); }
      io::println(Str(int(s)) + " " + Str(ys.at(unsigned(n - 1))));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  // Each pass adds 2 * xs[i]: 8 * sum(i % 10) = 8 * 4.5 * n.
  EXPECT_EQ(r.StdOut, "7200000 72\n");
}
