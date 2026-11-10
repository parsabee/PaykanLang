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
// `continue` ends one iteration, on every path: in order on the calling
// thread, and in chunks whose threads each have their own exit flag.  What
// the iteration holds is let go of.  A loop in the body has its own `break`
// and `continue`.
TEST(Concurrency, ParallelForContinue) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      few = 0;
      parallel for i in 0..100 {
        if i % 2 == 0 { continue; }
        few += i;
      }
      many = 0;
      parallel for i in 0..100000 {
        if i % 3 == 0 { continue; }
        many += 1;
      }
      owned = 0;
      parallel for i in 0..20000 {
        let s = Str(i);
        if i % 2 == 0 { continue; }
        owned += s.len() - s.len() + 1;
      }
      inner = 0;
      parallel for i in 0..20000 {
        j = 0;
        while True {
          j += 1;
          if j == 2 { continue; }
          if j > 4 { break; }
          inner += 1;
        }
        for k in 0..3 {
          if k == 1 { continue; }
          inner += 1;
        }
        if i % 2 == 0 { continue; }
        inner += 1;
      }
      nested = 0;
      parallel for i in 0..64 {
        if i % 4 != 0 { continue; }
        parallel for j in 0..64 {
          if j % 2 == 0 { continue; }
          nested += 1;
        }
      }
      after = 0;
      for i in 0..4 {
        if i == 2 { continue; }
        after += 1;
      }
      io::println(Str(few) + " " + Str(many) + " " + Str(owned) + " "
                  + Str(inner) + " " + Str(nested) + " " + Str(after));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2500 66666 10000 110000 512 3\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ParallelForContinue");
}

// A reduction written in a nested loop is a reduction of each loop it is
// outside of: the inner loop's fold is a chunk's partial, not the variable.
TEST(Concurrency, ParallelForNestedReductionIsFoldedOnce) {
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s = 0;
      parallel for i in 0..2000 {
        parallel for j in 0..100 {
          s += 1;
        }
      }
      io::println(Str(s));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "200000\n");
}

// The partials of a loop with a reduction are stack space of the function,
// not of each time round the loop around it.
TEST(Concurrency, ParallelForInALoopKeepsItsPartialsOnce) {
  auto r = compileAndRun(R"(import ::io;
    fn f(i: int) -> int { return i; }
    fn main() -> int {
      r = 0;
      for j in 0..100000 {
        s = 0;
        parallel for i in 0..3 { s += f(i); }
        r += s;
      }
      io::println(Str(r));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "300000\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// Code after a `continue` is dead, and may open statements of its own.
TEST(Concurrency, DeadCodeAfterAContinueIsDiscarded) {
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      parallel for i in 0..1 {
        continue;
        if i == 1 {
          continue;
          if i == 2 { }
          while i == 3 { }
          parallel for j in 0..2 { }
        } else { }
      }
      io::println("done");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
}

TEST(Concurrency, AGenericAsyncFunctionIsAwaited) {
  auto r = compileAndRun(R"(import ::io;
    async fn t<T: Copyable>(x: T) -> T { return x; }
    async fn m() -> int {
      let h = t(7);
      return await h;
    }
    fn main() -> int {
      let h = m();
      io::println(Str(await h));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n");
}

// The folds wrap as the type's own operations do, whatever the chunks'
// partials added up to; a reduction on an `inout` writes through it.
TEST(Concurrency, ParallelForReductionsKeepTheirType) {
  auto r = compileAndRun(R"(import ::io;
    fn f(s: inout int) { parallel for i in 0..4 { s += i; } }
    fn g(s: inout float, t: inout u8) {
      parallel for i in 0..4 { s += 0.5; t += 100; }
    }
    fn main() -> int {
      s: u8 = 200;
      parallel for i in 0..100 { s += 1; }
      a: u8 = 0;
      parallel for i in 0..10000 { if i % 2 == 0 { continue; } a += 1; }
      b: i32 = 2147483647;
      parallel for i in 0..2 { b += 1; }
      c: i8 = -100;
      parallel for i in 0..100 { c -= 1; }
      d: u8 = 3;
      parallel for i in 0..3 { d *= 7; }
      n: u8 = 0;
      parallel for i in 0..3000 { parallel for j in 0..3 { n += 1; } }
      io::println(Str(s) + " " + Str(a) + " " + Str(b) + " " + Str(c) + " "
                  + Str(d) + " " + Str(n));
      t = 10;
      f(t);
      k = 1;
      r: inout = k;
      parallel for i in 0..10 { r += 1; }
      x = 0.0;
      y: u8 = 0;
      g(x, y);
      io::println(Str(t) + " " + Str(k) + " " + Str(x) + " " + Str(y));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "44 136 -2147483647 56 5 40\n16 11 2 144\n");
  EXPECT_EQ(r.ExitCode, 0);
}

// A lambda in dead code builds a function of its own while the dead
// statement around it is still open.
TEST(Concurrency, ALambdaInDeadCodeLeavesItsStatementOpen) {
  auto r = compileAndRun(R"(fn f() -> int {
      return 1;
      if True {
        let g = fn(x: int) -> int { return x; };
      }
      return 0;
    }
    fn main() -> int {
      parallel for i in 0..3 {
        continue;
        if i == 1 {
          let g = fn() -> int { return 1; };
        }
      }
      return f();
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 1);
}

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
      e = 100;
      let ok = n.compareExchange(e, 7);
      let no = n.compareExchange(e, 8);
      io::println(Str(old) + " " + Str(ok) + " " + Str(no) + " " + Str(n));
      f: atomic float = 2.0;
      let fo = f.exchange(0.5);
      fe = 0.5;
      let fok = f.compareExchange(fe, 4.0);
      io::println(Str(fo) + " " + Str(f) + " " + Str(fok));
      b: atomic bool = False;
      let was = b.exchange(True);
      io::println(Str(was) + " " + Str(b));
      c: atomic char = 'a';
      ce = 'a';
      let cok = c.compareExchange(ce, 'b');
      let cno = c.compareExchange(ce, 'c');
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

// A unique object stored in an `atomic shared` field is frozen at the
// store, with what it holds, and one stored in a `locked mut` field becomes
// a cell there: in `__init__` and in a later assignment through a `mut`
// reference, as in a variable or a parameter.  Tasks read the frozen field
// and lock the cell; a `lock` block takes the field itself (`lock
// self.account`, `lock b.account`).  The field stores once kept the object
// as it was: a frozen field's object was counted from every thread and
// freed with its holder, and a locked field held no cell.  The heap is
// tracked (no task allocates): the three frozen items and the cell (its
// mutex block and the account) are what stays live.
TEST(Concurrency, AFieldStoreFreezesOrMakesACell) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Item { v: int; child: Item?; }
    class Account { balance: int; }
    class Bank {
      catalog: atomic shared Item;
      account: locked mut Account;
      view fn deposit(n: int) -> int {
        total = 0;
        lock self.account as a: inout { a.balance += n; total = a.balance; }
        return total;
      }
    }
    impl Bank {
      fn __init__() {
        self.catalog = Item(1, Item(2, None));
        self.account = Account(10);
      }
    }
    async fn read(c: atomic shared Item) -> int {
      match c.child {
        ch: Item { return c.v + ch.v; }
        None { return c.v; }
      }
    }
    async fn work(acct: locked mut Account, n: int) -> int {
      for i in 0..n { lock acct as a: inout { a.balance += 1; } }
      return n;
    }
    fn main() -> int {
      let b: mut = Bank();
      let r0 = read(b.catalog);
      let r1 = read(b.catalog);
      let w0 = work(b.account, 500);
      let w1 = work(b.account, 500);
      let k = await r0 + await r1 + await w0 + await w1;
      b.catalog = Item(4, None);
      total = b.deposit(5);
      lock b.account as v: view { total += v.balance; }
      io::println(Str(k) + " " + Str(total) + " " + Str(b.catalog.v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1006 2030 4\n");
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(liveBlocksAfterRun(), 0);
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

// A moved object is released exactly once, where it went: a `weak` local of
// its name (a `shared` string the Job alone holds: a `weak` reference names
// a `shared` object) reads None once the callee has released the argument,
// and the caller's ended variable releases nothing.  A partial move `move
// h.job` takes the field out and ends the holder, whose teardown leaves the
// field alone; the object goes when its new owner does.  A struct value
// moves into a parameter and a unique field moves out of one.  Zero live
// blocks says nothing was released twice or never.
TEST(Concurrency, MovesReleaseTheObjectOnce) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Job { name: shared Str; }
    class Holder { job: Job; tag: Str; }
    struct Pair { left: Job; n: int; }
    fn show(w: view Str?) -> Str {
      match w { s: Str { return s.clone(); } None { return "gone"; } }
    }
    fn keep(j: Job) -> int { return j.name.len(); }
    fn take(p: Pair) -> int { return p.left.name.len() + p.n; }
    fn main() -> int {
      let a = Job("abc");
      wa: weak = a.name;
      let n = keep(move a);
      io::println(Str(n) + " " + show(wa));
      let h = Holder(Job("inner"), "t");
      wj: weak = h.job.name;
      if True {
        let inner = move h.job;
        io::println(inner.name + " " + show(wj));
      }
      io::println(show(wj));
      let p = Pair(Job("left"), 1);
      io::println(Str(take(move p)));
      let r = Pair(Job("xy"), 2);
      wr: weak = r.left.name;
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
      let d: shared Node = move c;
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

// -- Counted cells, atomic arrays and compareExchange's write-back (#368)

TEST(Concurrency, CellsAndFrozenValuesAreFreedByTheirLastHandle) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Item { v: int; child: Item?; name: Str; }
    class Count { n: atomic int; }
    fn bump(c: atomic int) { c += 1; }
    fn same(c: atomic int) -> atomic int { return c; }
    async fn read(c: atomic shared Item) -> int { return c.v; }
    async fn add(l: locked mut Item, n: int) -> int {
      for i in 0..n { lock l as a: inout { a.v += 1; } }
      return n;
    }
    fn main() -> int {
      a: atomic int = 1;
      bump(a);
      h = same(a);
      h += 1;
      let c = Count(a);
      c.n += 1;
      let f: atomic shared Item = Item(1, Item(2, None, "in"), "out");
      let t = read(f);
      let u = read(f);
      l: locked mut Item = Item(10, None, "cell");
      let w = add(l, 100);
      n = await t + await u + await w;
      lock l as v: view { n += v.v; }
      io::println(Str(a) + " " + Str(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4 212\n");
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, ACellHeldByManyTasksIsCountedExactly) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    async fn work(c: atomic int, n: int) -> int {
      for i in 0..n {
        h: atomic int = c;
        h += 1;
      }
      return n;
    }
    fn main() -> int {
      c: atomic int = 0;
      let a = work(c, 2000);
      let b = work(c, 2000);
      let d = work(c, 2000);
      let t = await a + await b + await d;
      io::println(Str(c) + " " + Str(t));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6000 6000\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, CompareExchangeWritesTheValueFound) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B { e: int; }
    fn main() -> int {
      n: atomic int = 5;
      e = 4;
      let no = n.compareExchange(e, 9);
      let ok = n.compareExchange(e, 9);
      f: atomic f32 = 1.5;
      fe: f32 = 2.5;
      let fno = f.compareExchange(fe, 0.5);
      c: atomic char = 'x';
      ce = 'y';
      let cno = c.compareExchange(ce, 'z');
      t: atomic bool = True;
      be = False;
      let bno = t.compareExchange(be, False);
      m: mut B = B(0);
      let fieldNo = n.compareExchange(m.e, 1);
      io::println(Str(no) + " " + Str(e) + " " + Str(ok) + " " + Str(n) + " "
          + Str(fno) + " " + Str(fe) + " " + Str(cno) + " " + Str(ce) + " "
          + Str(bno) + " " + Str(be) + " " + Str(fieldNo) + " " + Str(m.e));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False 5 True 9 False 1.5 False x False True False 9\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AnAtomicArrayWorksFromEveryThread) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Hits { a: atomic int[3]; }
    impl Hits { fn __init__() { self.a = [0; 3]; } }
    fn total(a: atomic int[3]) -> int { return a[0] + a[1] + a[2]; }
    fn main() -> int {
      a: atomic int[3] = [1, 2, 3];
      parallel for i in 0..3000 { a[i % 3] += 1; }
      a[0] -= 1;
      let was = a[1].exchange(7);
      e = 7;
      let ok = a[1].compareExchange(e, 8);
      let h = Hits();
      h.a[2] += 5;
      f: atomic float[2] = [0.5; 2];
      f[1] += 1.0;
      b: atomic bool[2] = [False; 2];
      b[1] = True;
      x: float = f[1];
      y: bool = b[1];
      io::println(Str(was) + " " + Str(ok) + " " + Str(total(a)) + " "
          + Str(total(h.a)) + " " + Str(x) + " " + Str(y) + " " + Str(a.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1002 True 2011 5 1.5 True 3\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AHoldOfAPartOfAFrozenValueKeepsItAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class K { v: int; }
    class C { name: Str; opt: K?; }
    class H { cfg: atomic shared C; }
    fn mk() -> atomic shared C { return C("a" + Str(1), K(4)); }
    fn look(n: view Str, h: shared mut H) -> int {
      h.cfg = C("r", None);
      return n.len();
    }
    fn main() -> int {
      io::println(mk().name);
      let h: shared mut H = H(C("o" + Str(2), None));
      io::println(Str(look(h.cfg.name, h)));
      r: atomic shared C = C("s", K(1));
      match r.opt {
        k: K { r = C("t", None); io::println(Str(k.v)); }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "a1\n2\n1\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, ALockBlockHoldsItsCell) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class K { v: int; }
    class C { l: locked mut K; }
    fn mk() -> locked mut K { return K(7); }
    fn main() -> int {
      c: locked mut K = K(1);
      lock c as v: inout {
        c = K(2);
        v.v = 5;
        io::println(Str(v.v));
      }
      lock mk() as v: view { io::println(Str(v.v)); }
      lock C(K(1)).l as v: view { io::println(Str(v.v)); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n7\n1\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AnOperationHoldsItsCellWhileLaterOperandsRun) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class C { n: atomic int; a: atomic int[2]; }
    impl C { fn __init__(v: int) { self.n = v; self.a = [v, v]; } }
    fn rep(o: inout C) -> int { o = C(2); return 0; }
    fn mk() -> atomic int[3] { return [7, 8, 9]; }
    fn main() -> int {
      o: mut C = C(1);
      let x = o.n.exchange(rep(o));
      p: mut C = C(3);
      let y = p.a[rep(p)];
      q: mut C = C(5);
      q.a[0] = rep(q);
      io::println(Str(x) + " " + Str(y) + " " + Str(mk()[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1 3 8\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AHandleIsCopyableAndMoves) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn fresh(v: int) -> atomic int { return v; }
    fn take(t: (atomic int, int)) -> int { return t.1; }
    fn main() -> int {
      t = (fresh(1), 2);
      let k = take(t);
      h = t.0;
      h += 1;
      c = t.clone();
      a: atomic int = 1;
      b: atomic int = move a;
      b += 1;
      f = fn(x: atomic int) { x += 1; };
      n: atomic int = 1;
      f(n);
      io::println(Str(k) + " " + Str(h) + " " + Str(b) + " " + Str(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2 2 2 2\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, APlainArrayFlowsIntoAnAtomicSlotAsANewCell) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class C { a: atomic int[2]; }
    class P { a: atomic int[2]; }
    impl P { fn __init__(x: int[2]) { self.a = x; } }
    fn take(a: atomic int[3]) -> int { return a[0]; }
    fn mk() -> atomic int[3] { io::println("made"); return [7, 8, 9]; }
    fn main() -> int {
      c = C([1, 2]);
      o = P([3, 4]);
      io::println(Str(c.a[0] + take([5, 6, 7]) + o.a[1]));
      io::println(Str(o.a.len()) + " " + Str(mk().len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "10\nmade\n2 3\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, ACompoundAssignmentOfAnElementIsOneOperation) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn f() -> int { io::println("f"); return 1; }
    fn main() -> int {
      hits: atomic int[3] = [0; 3];
      parallel for i in 0..300000 { hits[i % 3] += 1; }
      a: atomic int[2] = [1, 2];
      a[f()] += 10;
      u: atomic u8 = 255;
      u += 1;
      e: u8 = u;
      let ok = u.compareExchange(e, 7);
      s: atomic i8[1] = [127];
      s[0] += 1;
      x: i8 = s[0];
      io::println(Str(hits[0] + hits[1] + hits[2]) + " " + Str(a[1]) + " "
          + Str(ok) + " " + Str(x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "f\n300000 12 True -128\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AVariableReturnedIntoAFrozenResultMoves) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class C { id: int; }
    fn f() -> atomic shared C { let c = C(1); return c; }
    fn g(c: C) -> atomic shared C { return c; }
    fn main() -> int {
      io::println(Str(f().id + g(C(2)).id));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, CompareExchangeHoldsWhatExpectedIsIn) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B { e: int; }
    class W { b: B; c: B?; }
    fn rep(w: shared mut W) -> int {
      w.b = B(7);
      w.c = B(9);
      return 1;
    }
    fn main() -> int {
      n: atomic int = 5;
      w: shared mut W = W(B(0), None);
      n.compareExchange(w.b.e, rep(w));
      match w.c {
        c: B { io::println(Str(c.e)); }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, AHandleInAHeldReadIsRetainedOnce) {
  LeakGuard g;
  auto r = compileAndRun(R"(class C { n: atomic int; }
    fn f(c: atomic int, o: inout C) -> int { return 1; }
    fn main() -> int {
      o: mut C = C(1);
      f(o.n, o);
      p = C(1);
      h: atomic int = (move p).n;
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, ACompoundAssignmentWhoseIndexTakesAnArgumentCompiles) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn f(n: int) -> int { io::println("f"); return n; }
    fn main() -> int {
      a: atomic int[2] = [1, 2];
      a[f(1)] += 1;
      io::println(Str(a[1]));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "f\n3\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

TEST(Concurrency, NoneIsNotCountedAcrossThreads) {
  auto r = compileAndRun(R"(import ::io;
    async fn reader() -> int {
      match io::open("/dev/null", "r") {
        o: Ok { for j in 0..200000 { let r = o.value.readln(); } }
        e: Err { }
      }
      return 0;
    }
    async fn run() -> int {
      let a = reader();
      let b = reader();
      let c = reader();
      let d = reader();
      return await a + await b + await c + await d;
    }
    fn main() -> int {
      let t = run();
      await t;
      io::println("done");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "done\n");
  EXPECT_EQ(r.ExitCode, 0);
}
