// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the concurrency prototype's checks (docs/design/concurrency.md;
// src/Sema/SemaConcurrency.cpp and the visitors in Sema.cpp that call it):
// `async fn` and `await`, the send check, `Copyable`, `parallel for`
// bodies, `atomic` places and `locked` cells.  The programs of
// samples/concurrency are the map; each diagnostic's text is the compiler's.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

/// Expect @p r to have failed with each of @p diags (`line:col: error: ...`).
void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

/// Expect no diagnostic at any of @p lines (`:N:`): the accepted statements
/// beside the rejected ones.
void expectCleanLines(const SemaResult &r,
                      std::initializer_list<const char *> lines) {
  for (const char *line : lines)
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

/// The warnings in @p r.
size_t countWarnings(const SemaResult &r) {
  size_t n = 0;
  for (size_t at = r.Diagnostics.find("warning: "); at != std::string::npos;
       at = r.Diagnostics.find("warning: ", at + 1))
    ++n;
  return n;
}

} // namespace

// -- `async fn` and `await`

// A call of an `async fn` is a `Task<T>` of its result, and `await` of the
// task is the `T`: a number, bool, char, a reference or nothing.  The task
// may be awaited where it is called or later, in an `async fn` or not: the
// design keeps `await` to async code, which the prototype does not check
// (the samples' `fn main` awaits), so `sync` is clean here.
TEST(Concurrency, AsyncFnCallsAreTasks) {
  auto r = semaCheck(R"(class Job { name: Str; }
async fn fib(n: int) -> int {
  if n < 2 { return n; }
  let a = fib(n - 1);
  let b = fib(n - 2);
  return (await a) + (await b);
}
async fn nothing() { }
async fn give() -> Job { return Job("j"); }
async fn flag() -> bool { return True; }
async fn letter() -> char { return 'c'; }
async fn name(s: Str) -> int { return s.len(); }
fn sync() -> int {
  let t = fib(1);
  let k: int = await t;
  let v = nothing();
  await v;
  let j = await give();
  let b: bool = await flag();
  let c: char = await letter();
  let n = await name("lit");
  return k + n + j.name.len() + (if b && c == 'c' then 1 else 0);
}
async fn main() -> int { return sync(); })");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// What an `async fn` cannot be: its result crosses back to the awaiter (the
// send check, so no `shared` one) and is a number, bool, char, a reference
// or nothing (no tuple, no optional in the prototype); its parameters are
// by value (the task outlives the call) and not a value held in memory (a
// struct, a function value: the task would read the caller's frame).  The
// task is a `Task<T>`, and `await` takes nothing else; a task of nothing
// yields no value.
TEST(Concurrency, AsyncFnRules) {
  auto r = semaCheck(R"(class Job { name: Str; }
struct Point { x: int; y: int; }
async fn fib(n: int) -> int { return n; }
async fn nothing() { }
async fn giveShared() -> shared Job { return Job("x"); }
async fn plot(p: Point) -> int { return p.x; }
async fn call(f: () -> int) -> int { return f(); }
async fn byInout(n: inout int) -> int { return n; }
async fn byView(n: view int) -> int { return n; }
async fn pair() -> (int, int) { return (1, 2); }
async fn maybe() -> int? { return None; }
fn main() -> int {
  let s: Str = fib(2);
  let w = await 3;
  let v = await nothing();
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
  const std::string inMemory =
      "', a value held in memory: the prototype passes numbers, references "
      "and handles to a task (hold the value in a class)";
  const std::string outlives = "': a task outlives the call that starts it";
  const std::string returns =
      ": error: an 'async fn' returns a number, bool, char, a reference or "
      "nothing in the prototype, not '";
  expectErrors(
      r, {":5:1: error: the result of 'async fn' 'giveShared' (a 'shared "
          "Job') cannot cross to the task's awaiter: is 'shared Job': a "
          "'shared' reference is aliased on its own thread",
          (":6:15: error: parameter 'p' of 'async fn' 'plot' has type 'Point" +
           inMemory)
              .c_str(),
          (":7:15: error: parameter 'f' of 'async fn' 'call' has type '() -> "
           "int" +
           inMemory)
              .c_str(),
          (":8:18: error: parameter 'n' of 'async fn' 'byInout' cannot be "
           "'inout" +
           outlives)
              .c_str(),
          (":9:17: error: parameter 'n' of 'async fn' 'byView' cannot be "
           "'view" +
           outlives)
              .c_str(),
          (":10:1" + returns + "(int, int)'").c_str(),
          (":11:1" + returns + "int?'").c_str(),
          ":13:3: error: initializer of type 'Task<int>' does not match "
          "declared type 'Str' for variable 's'",
          ":14:11: error: 'await' takes a task (the result of calling an "
          "'async fn'), not a 'int'",
          ":15:3: error: variable 'v' cannot have type 'void' (the expression "
          "produces no value)"});
}

// -- The send check

// At an async call's arguments: a unique reference (a `Str` too) read from
// a place crosses only by `move`; a `shared` one never crosses, moved or
// not; what a class holds must cross as well, and the error names the
// field's path: a `shared` or `weak` field, a function value, an `Obj`.
// An optional of a reference is one.  No task starts in a `parallel for`.
TEST(Concurrency, SendCheckAtAnAsyncCall) {
  auto r = semaCheck(R"(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
class Holder { job: Job; }
class Fun { f: () -> int; }
class Any { o: Obj; }
class Weak { weak w: Cache?; }
class Node { id: int; }
async fn consume(n: Node) -> int { return n.id; }
async fn name(s: Str) -> int { return s.len(); }
async fn run(j: Job) -> int { return 1; }
async fn hold(h: Holder) -> int { return 1; }
async fn fun(f: Fun) -> int { return 1; }
async fn any(a: Any) -> int { return 1; }
async fn wk(w: Weak) -> int { return 1; }
async fn opt(n: Node?) -> int { return 1; }
fn main() -> int {
  let a = Node(1);
  let t1 = consume(a);
  let str = "s";
  let t2 = name(str);
  let s: shared Node = Node(2);
  let t3 = consume(s);
  let s2: shared Node = Node(3);
  let t4 = consume(move s2);
  let j = Job("j", Cache(0));
  let t5 = run(move j);
  let h = Holder(Job("h", Cache(1)));
  let t6 = hold(move h);
  let f = Fun(fn() => 3);
  let t7 = fun(move f);
  let an = Any(Node(4));
  let t8 = any(move an);
  let w = Weak(Cache(2));
  let t9 = wk(move w);
  let o = Node(5);
  let t10 = opt(o);
  parallel for i in 0..4 { let t = consume(Node(i)); }
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 11u) << r.Diagnostics;
  const std::string shared =
      "': a 'shared' reference is aliased on its own thread";
  expectErrors(
      r,
      {":18:20: error: cannot copy 'a' (a unique 'Node') into a task: write "
       "'move a' to transfer it",
       ":20:17: error: cannot copy 'str' (a unique 'Str') into a task: write "
       "'move str' to transfer it",
       (":22:20: error: 's' is 'shared Node': it never crosses to a task (a "
        "'shared' reference is aliased on its own thread)"),
       (":24:20: error: 's2' is 'shared Node': it never crosses to a task (a "
        "'shared' reference is aliased on its own thread)"),
       (":26:16: error: cannot send 'j' (a 'Job'): field 'cache' is 'shared "
        "Cache" +
        shared)
           .c_str(),
       (":28:17: error: cannot send 'h' (a 'Holder'): field 'job.cache' is "
        "'shared Cache" +
        shared)
           .c_str(),
       ":30:16: error: cannot send 'f' (a 'Fun'): field 'f' is a function "
       "value, whose captures are not known",
       ":32:16: error: cannot send 'an' (a 'Any'): field 'o' is an 'Obj', "
       "which may be any class",
       ":34:15: error: cannot send 'w' (a 'Weak'): field 'w' is 'weak': a "
       "weak load is a plain retain",
       ":36:17: error: cannot copy 'o' (a unique 'Node') into a task: write "
       "'move o' to transfer it",
       ":37:36: error: a task cannot be started in a 'parallel for' body"});
}

// What crosses: a moved unique reference, a temporary (`move` before one
// does nothing), a class whose reference field is a handle, a cell's
// handle and the handles of an `atomic const` object and a `locked` cell,
// each copied into any number of tasks, and a `Str` temporary.
TEST(Concurrency, HandlesAndTemporariesCrossToATask) {
  auto r = semaCheck(R"(class Cache { hits: atomic int; }
class Job { name: Str; cache: atomic const Cache; }
class Node { id: int; }
class Counts { total: int; }
async fn consume(n: Node) -> int { return n.id; }
async fn run(j: Job, x: int) -> int { j.cache.hits += 1; return j.name.len() + x; }
async fn cell(c: atomic int) -> int { return c; }
async fn frozen(n: atomic const Node) -> int { return n.id; }
async fn guarded(c: locked Counts) -> int { lock c as v: inout { v.total += 1; } return 1; }
async fn opt(n: Node?) -> int { return 1; }
async fn name(s: Str) -> int { return s.len(); }
fn main() -> int {
  let b = Node(2);
  let t1 = consume(move b);
  let t2 = consume(Node(3));
  let t3 = consume(move Node(4));
  let shared_cache: atomic const Cache = Cache(0);
  let t4 = run(Job("job", shared_cache), 3);
  let k: atomic int = 0;
  let t5 = cell(k);
  let t6 = cell(k);
  let fr: atomic const Node = Node(5);
  let t7 = frozen(fr);
  let t8 = frozen(fr);
  let lk: locked Counts = Counts(0);
  let t9 = guarded(lk);
  let t10 = guarded(lk);
  let o = Node(6);
  let t11 = opt(move o);
  let t12 = name("lit");
  let t13 = name(Str(7));
  return await t1 + await t2 + await t3 + await t4 + await t5 + await t6
      + await t7 + await t8 + await t9 + await t10 + await t11 + await t12
      + await t13 + shared_cache.hits;
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// At a conversion to `atomic const` or `locked`, the object is frozen or
// made a cell with everything it holds, so the send check runs on its
// fields, named by path.  A unique value converts, moved or a temporary,
// and a `const` one into `atomic const`.
TEST(Concurrency, SendCheckAtAConversionToAtomicConstOrLocked) {
  auto r = semaCheck(R"(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
class Holder { job: Job; }
class Fun { f: () -> int; }
class Any { o: Obj; }
class Node { id: int; }
fn main() -> int {
  let k: atomic const Holder = Holder(Job("k", Cache(2)));
  let l: locked Holder = Holder(Job("l", Cache(3)));
  let m: locked Fun = Fun(fn() => 1);
  let a: atomic const Any = Any(Node(0));
  let n = Node(1);
  let f: atomic const Node = move n;
  let g: locked Node = Node(2);
  let h: atomic const Node = Node(3);
  let c: const Node = Node(4);
  let v: atomic const Node = c;
  return f.id + h.id + v.id;
})");
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  const std::string shared =
      "field 'job.cache' is 'shared Cache': a 'shared' reference is aliased "
      "on its own thread";
  expectErrors(
      r, {(":8:3: error: cannot make 'Holder(...)' (a 'Holder') 'atomic "
           "const': " +
           shared)
              .c_str(),
          (":9:3: error: cannot make 'Holder(...)' (a 'Holder') 'locked': " +
           shared)
              .c_str(),
          ":10:3: error: cannot make 'Fun(...)' (a 'Fun') 'locked': field 'f' "
          "is a function value, whose captures are not known",
          ":11:3: error: cannot make 'Any(...)' (a 'Any') 'atomic const': "
          "field 'o' is an 'Obj', which may be any class"});
  expectCleanLines(r, {":12:", ":13:", ":14:", ":15:", ":16:", ":17:"});
}

// -- `Copyable`

// `Copyable` is structural: numbers, `bool`, `char`, an `atomic` handle's
// value, and an enum, tuple or struct of Copyable parts (a struct's
// `shared` field counts) meet a `T: Copyable` bound.  A unique class, a
// `Str`, a function value, and a struct holding a unique reference do not,
// with the reason; and `impl Copyable for X` is an error.
TEST(Concurrency, CopyableIsAStructuralBound) {
  auto ok = semaCheck(R"(class Node { id: int; }
struct Point { x: int; y: int; }
struct Alias { s: shared Node; n: int; }
enum Color { Red, Green }
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn main() -> int {
  let a = twice(1);
  let b = twice(Point(1, 2));
  let c = twice(Alias(Node(3), 1));
  let d = twice((1, 2.5));
  let e = twice(Color::Red);
  let f = twice(1.5);
  let m: atomic int = 1;
  let g = twice(m);
  let h = twice(True);
  let i = twice('c');
  return a.0 + b.1.x + c.0.n + c.1.s.id;
})");
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  EXPECT_EQ(ok.ErrorCount, 0u) << ok.Diagnostics;

  auto r = semaCheck(R"(class Node { id: int; }
class Named { tag: Str; n: int; }
struct Pair { left: Node; n: int; }
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
impl Copyable for Named {}
fn main() -> int {
  let d = twice(Named("d", 2));
  let e = twice(Pair(Node(2), 1));
  let g = twice("s");
  let k = twice(fn() => 1);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  const std::string needs =
      "' does not implement trait 'Copyable', which 'twice' requires of 'T': ";
  const std::string moveOnly =
      "a reference, a 'Str', a function value or a task is move-only";
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  expectErrors(r, {":5:1: error: 'Copyable' is built in: a type is Copyable by "
                   "what it "
                   "holds, and cannot be implemented",
                   (":7:11: error: 'Named" + needs + moveOnly).c_str(),
                   (":8:11: error: 'Pair" + needs +
                    "it holds move-only field 'left' ('Node')")
                       .c_str(),
                   (":9:11: error: 'Str" + needs + moveOnly).c_str(),
                   (":10:11: error: '() -> int" + needs + moveOnly).c_str()});
  // NOLINTEND(bugprone-suspicious-missing-comma)
}

// -- `parallel for`

// A body reads the enclosing function's values, arrays of values, `atomic`
// variables and handles (an `atomic const` object's fields, a `locked`
// cell under a `lock`), declares what it likes, changes an outer number by
// a reduction (`+=`, `-=`, `*=`, under an `if`, a `match` or an inner loop
// too), bumps an atomic, and writes an `inout` or outer array at the loop's
// variable.
TEST(Concurrency, ParallelForBodies) {
  auto r = semaCheck(R"(class Config { lo: int; hi: int; }
class Counts { total: int; }
class Stats { served: atomic int; }
enum Color { Red, Green }
struct Point { x: int; y: int; }
fn dot(xs: view float[], ys: view float[]) -> float {
  s = 0.0;
  parallel for i in 0..xs.len() { s += xs[i] * ys[i]; }
  return s;
}
fn saxpy(a: float, xs: view float[], ys: inout float[]) {
  parallel for i in 0..xs.len() { ys[i] = a * xs[i] + ys[i]; }
}
fn main() -> int {
  let n = 100;
  let k = 2;
  let flag = True;
  let c = Color::Red;
  xs = [0.0; 100];
  ys = [0.0; 100];
  let hits: atomic int = 0;
  let cfg: atomic const Config = Config(0, 50);
  let counts: locked Counts = Counts(0);
  let st: atomic const Stats = Stats(0);
  total = 0;
  prod = 1;
  parallel for i in 0..n {
    let local = Point(i, i);
    let node = Config(i, i);
    total += i * k + local.x + node.lo;
    prod *= 1;
    if i % 10 == 0 { hits += 1; }
    if flag && i > cfg.lo && i < cfg.hi { st.served += 1; }
    if i == n - 1 { lock counts as v: inout { v.total += 1; } }
    ys[i] = xs[i] + 1.0;
    match c { Red { total -= 1; } _ { } }
    for j in 0..3 { total += j; }
  }
  saxpy(2.0, xs, ys);
  return int(dot(xs, ys)) + total + prod;
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// What a body may not do: read a reference of the enclosing function (an
// object, an array of objects, a `Str`); assign an outer variable other
// than by a reduction (`=` of a value, `/=`; `x = x + e` and `x = x * e`
// are the reductions the parser spells `+=` and `*=`); write an outer array
// at any index but the loop's variable; `break`, `continue`, `return`
// (from an inner loop too), `await`, or start a task.
TEST(Concurrency, ParallelForBodyRejects) {
  auto r = semaCheck(R"(class Node { id: int; }
async fn consume(n: int) -> int { return n; }
fn main() -> int {
  let nodes = [Node(4)];
  let one = Node(1);
  let s = "str";
  total = 0;
  parallel for i in 0..1 {
    total = nodes[i].id;
    total = one.id;
    total = s.len();
  }
  parallel for i in 0..4 {
    total = 5;
    total += 1;
    total /= 2;
    total = total + i;
    total = total * 2;
  }
  let xs = [0.0; 4];
  ys = [0.0; 4];
  parallel for i in 0..4 {
    ys[3 - i] = xs[i];
    ys[0] = xs[i];
    ys[i] = xs[i];
  }
  parallel for i in 0..4 { break; }
  parallel for i in 0..4 { continue; }
  parallel for i in 0..4 { return 1; }
  parallel for i in 0..4 { let t = consume(i); }
  let t = consume(1);
  parallel for i in 0..4 { let r = await t; }
  parallel for i in 0..4 { for j in 0..2 { break; } }
  return total;
})");
  // The thirteen rejections, and the task `t` of line 31 that the body may
  // not await (line 32) and the function never does: the handle check.
  EXPECT_EQ(r.ErrorCount, 14u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":34:3: error: task 't' is not awaited before "
                               "this 'return'"),
            std::string::npos)
      << r.Diagnostics;
  const std::string read =
      "cannot be used in a 'parallel for' body: an iteration uses only "
      "values, arrays of values and 'atomic' variables of the enclosing "
      "function";
  const std::string assign =
      ": error: cannot assign to 'total' in a 'parallel for' body: a variable "
      "of the enclosing function changes only by a reduction ('total += e', "
      "'total -= e', 'total *= e') or, if 'atomic', by its operations";
  const std::string atIndex =
      ": error: 'ys' is written in a 'parallel for' body at an index other "
      "than the loop's variable 'i': two iterations could write one element";
  expectErrors(
      r, {(":9:13: error: 'nodes' (a 'Node[1]') " + read).c_str(),
          (":10:13: error: 'one' (a 'Node') " + read).c_str(),
          (":11:13: error: 's' (a 'Str') " + read).c_str(),
          (":14:5" + assign).c_str(), (":16:5" + assign).c_str(),
          (":23:8" + atIndex).c_str(), (":24:8" + atIndex).c_str(),
          ":27:28: error: 'break' cannot be used in a 'parallel for' body "
          "(the prototype)",
          ":28:28: error: 'continue' cannot be used in a 'parallel for' body "
          "(the prototype)",
          ":29:28: error: 'return' cannot leave a 'parallel for' body",
          ":30:36: error: a task cannot be started in a 'parallel for' body",
          ":32:36: error: 'await' cannot be used in a 'parallel for' body",
          ":33:44: error: 'break' cannot be used in a 'parallel for' body "
          "(the prototype)"});
  expectCleanLines(r, {":15:", ":17:", ":18:", ":25:"});
}

// An `atomic` variable of the enclosing function (an `atomic const`
// object's field too) changed by a statement of the body itself changes on
// every iteration, which bounces its cache line between cores: a warning,
// not an error, and none for a change under an `if`, a `match` or an inner
// loop, or of an atomic the body declares.
TEST(Concurrency, AnAtomicChangedEveryIterationWarns) {
  auto r = semaCheck(R"(class Stats { served: atomic int; }
fn main() -> int {
  let hits: atomic int = 0;
  let st: atomic const Stats = Stats(0);
  parallel for i in 0..100 {
    hits += 1;
    st.served += 1;
    hits = 3;
  }
  parallel for i in 0..100 {
    if i % 10 == 0 { hits += 1; st.served += 1; }
    let mine: atomic int = 0;
    mine += 1;
    match i { 3 { hits += 1; } _ { } }
    for j in 0..2 { hits += 1; }
  }
  hits += 1;
  return hits;
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
  const std::string warns =
      "' changes on every iteration of the 'parallel for': each atomic "
      "operation moves its cache line between cores, a thousand times a "
      "reduction's cost on 16 threads (benchmarks/README.md); count in a "
      "reduction (a plain variable of the enclosing function, '+='), or "
      "change it less often";
  for (const std::string &diag :
       {":6:5: warning: 'hits" + warns, ":7:5: warning: 'st.served" + warns,
        ":8:5: warning: 'hits" + warns})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(countWarnings(r), 3u) << r.Diagnostics;
}

// -- `atomic`

// An `atomic` place holds any number type, `bool` or `char`, as a variable
// or a class field: a read is its value, `=` a store, `+=` and `-=` one
// operation, `exchange(v)` gives the old value and `compareExchange(e, d)`
// a `bool`.  A copy of the place (`let m = n`, an argument, a result,
// `self.f = p` in `__init__`) is a handle to the same cell, so a handle
// passes to an `atomic` parameter and comes back from an `atomic` result;
// any other value makes a new cell.  The `atomic` fields of an `atomic
// const` object still change, in a task or a parallel body.
TEST(Concurrency, AtomicPlaces) {
  auto r = semaCheck(
      R"(class Stats { name: Str; served: atomic int; bytes: atomic float; done: atomic bool; }
impl Stats {
  fn __init__(name: Str, served: atomic int) {
    self.name = name; self.served = served; self.bytes = 0.0; self.done = False;
  }
}
class Box { n: atomic int; }
async fn record(st: atomic const Stats, id: int, n: int) -> int {
  for i in 0..n { st.served += 1; st.bytes += 0.5; }
  return id;
}
fn bump(c: atomic int, by: int) { c += by; }
fn fresh(v: int) -> atomic int { return v; }
fn same(c: atomic int) -> atomic int { return c; }
fn main() -> int {
  let a: atomic i8 = 120;
  a += 10;
  let u: atomic u8 = 250;
  u -= 10;
  let f: atomic f32 = 1.5;
  f += 0.25;
  let b: atomic bool = False;
  let was: bool = b.exchange(True);
  let c: atomic char = 'a';
  let old: char = c.exchange('b');
  let n: atomic int = 5;
  let swapped: bool = n.compareExchange(5, 9);
  let m = n;
  m += 1;
  bump(n, 10);
  let k = fresh(3);
  k += 1;
  let k2 = same(k);
  k2 -= 1;
  let shared_n: atomic int = 1;
  let st: atomic const Stats = Stats("s", shared_n);
  let t1 = record(st, 1, 10);
  let ids = await t1;
  st.done = True;
  let h = st.served;
  h += 1;
  let v: int = st.served;
  let box = Box(0);
  box.n += 1;
  let hn = box.n;
  hn = 4;
  let plain: int = n;
  let sum = n + m + plain + v + k + hn;
  let r = n.compareExchange(9, 1) && st.served.compareExchange(2, 0);
  return sum + ids + (if was || old == 'a' || swapped || r then 1 else 0);
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// What an `atomic` place refuses: a `Str` (or any reference) inside, a
// struct field (a struct is copied whole), an assignment whose right side
// reads the place (`x = x * 2`, through a call, a conditional, the place
// itself: it would read and write the cell separately), a compound operator
// other than `+=` and `-=`, and a value of another type stored or added.
// `x = x + 1` is `x += 1`, a store of another cell's value is a store.
TEST(Concurrency, AtomicRules) {
  auto r = semaCheck(R"(class Box { n: atomic int; }
struct P { c: atomic int; }
fn double(n: int) -> int { return n * 2; }
fn main() -> int {
  let hits: atomic int = 0;
  hits = hits * 2 + 1;
  hits *= 2;
  hits /= 2;
  let s: atomic Str = "a";
  let b = Box(0);
  b.n = 2 * b.n;
  b.n = double(b.n);
  b.n = b.n;
  hits = double(hits);
  hits = if hits > 0 then 1 else 2;
  hits = hits;
  hits += 1.5;
  hits = 1.5;
  hits -= 1.5;
  b.n = b.n + 1;
  hits = hits + 1;
  hits = b.n;
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 14u) << r.Diagnostics;
  const std::string separately =
      "' is 'atomic': this operator reads and writes it separately; its "
      "atomic operations are '+=', '-=', 'exchange' and 'compareExchange'";
  const std::string hitsReads =
      ": error: 'hits' is 'atomic': 'hits = ...' reads and writes it "
      "separately; write 'hits += e', or use 'exchange' or 'compareExchange'";
  const std::string bnReads =
      ": error: 'b.n' is 'atomic': 'b.n = ...' reads and writes it "
      "separately; write 'b.n += e', or use 'exchange' or 'compareExchange'";
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  expectErrors(r,
               {":2:12: error: field 'c' in struct 'P' has type 'atomic int': "
                "a "
                "struct cannot hold an 'atomic' field in the prototype (hold "
                "the "
                "cell in a class)",
                (":6:3" + hitsReads).c_str(),
                (":7:3: error: 'hits" + separately).c_str(),
                (":8:3: error: 'hits" + separately).c_str(),
                ":9:3: error: 'atomic' holds a number, a 'bool' or a 'char', "
                "not "
                "'Str' (the prototype has no atomic arrays)",
                (":11:3" + bnReads).c_str(), (":12:3" + bnReads).c_str(),
                (":13:3" + bnReads).c_str(), (":14:3" + hitsReads).c_str(),
                (":15:3" + hitsReads).c_str(), (":16:3" + hitsReads).c_str(),
                ":17:3: error: cannot add a 'float' to 'hits', an 'atomic int'",
                ":18:3: error: cannot store a 'float' in 'hits', an 'atomic "
                "int'",
                ":19:3: error: cannot add a 'float' to 'hits', an 'atomic "
                "int'"});
  // NOLINTEND(bugprone-suspicious-missing-comma)
  expectCleanLines(r, {":20:", ":21:", ":22:"});
}

// `exchange` and `compareExchange` are the only methods of an atomic place:
// `exchange` takes one argument and gives the old value, `compareExchange`
// takes two and gives a `bool`, each argument of the cell's type.
TEST(Concurrency, ExchangeAndCompareExchange) {
  auto r = semaCheck(R"(class Box { n: atomic int; }
fn main() -> int {
  let hits: atomic int = 0;
  let b = Box(0);
  let x = hits.load();
  b.n.compareExchange(1);
  b.n.exchange(1, 2);
  let r: int = hits.compareExchange(1, 2);
  let e: bool = hits.exchange(5);
  let t = hits.exchange("s");
  let u = hits.compareExchange(1, True);
  let f: atomic float = 1.0;
  let w: bool = f.compareExchange(1.0, 2.0);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  expectErrors(
      r, {":5:11: error: 'atomic int' has no method 'load': an atomic place "
          "has 'exchange(v)' and 'compareExchange(expected, desired)'",
          ":6:3: error: method 'compareExchange' expects 2 argument(s), got 1",
          ":7:3: error: method 'exchange' expects 1 argument(s), got 2",
          ":8:3: error: initializer of type 'bool' does not match declared "
          "type 'int' for variable 'r'",
          ":9:3: error: initializer of type 'int' does not match declared "
          "type 'bool' for variable 'e'",
          ":10:25: error: argument 1 of 'exchange' has type 'Str', expected "
          "'int'",
          ":11:35: error: argument 2 of 'compareExchange' has type 'bool', "
          "expected 'int'"});
  expectCleanLines(r, {":12:", ":13:"});
}

// -- `locked`

// A `locked` cell is made where a unique object flows into a `locked` slot
// (a variable, a parameter, a field, a result); its handle copies into
// tasks, variables and parallel bodies, and its contents are reached in a
// `lock` block: `inout` to change them, `view` to read.
TEST(Concurrency, LockedCells) {
  auto r = semaCheck(R"(class Counts { hits: int[16]; total: int; }
class Holder { cell: locked Counts; }
async fn record(counts: locked Counts, id: int, n: int) -> int {
  for i in 0..n {
    lock counts as c: inout { c.hits[(id + i) % 16] += 1; c.total += 1; }
  }
  return id;
}
fn total(counts: locked Counts) -> int {
  sum = 0;
  lock counts as v: view { for i in 0..16 { sum += v.hits[i]; } sum += v.total; }
  return sum;
}
fn make() -> locked Counts { return Counts([0; 16], 0); }
fn main() -> int {
  let counts: locked Counts = Counts([0; 16], 0);
  let a = record(counts, 1, 10);
  let b = record(counts, 2, 10);
  let ids = await a + await b;
  let h = Holder(make());
  lock h.cell as c: inout { c.total = 1; }
  let other = counts;
  parallel for i in 0..4 { lock other as c: inout { c.total += 1; } }
  return total(counts) + total(h.cell) + ids;
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// What `locked` refuses: reaching the contents outside a `lock` block (a
// field read, an element, a compound assignment, an element write), a
// borrow of the cell by a `view` or `inout` parameter, the handle in a
// unique slot; inside a `lock` block, a write through a `view` binding,
// `await`, another `lock`, a `parallel for`, `return`, `break` and
// `continue`; a `lock` of what is not `locked`; and a `locked` value (the
// prototype locks objects).
TEST(Concurrency, LockedRules) {
  auto r = semaCheck(R"(class Counts { hits: int[16]; total: int; }
async fn ping(n: int) -> int { return n; }
fn look(c: view Counts) -> int { return c.total; }
fn change(c: inout Counts) { c.total = 1; }
fn take(c: Counts) -> int { return c.total; }
fn main() -> int {
  let counts: locked Counts = Counts([0; 16], 0);
  let t = counts.total;
  let h = counts.hits[0];
  counts.total += 1;
  counts.hits[0] = 1;
  look(counts);
  change(counts);
  take(counts);
  lock counts as c: view {
    c.total = 5;
    c.hits[0] = 1;
    let p = ping(1);
    let r = await p;
    lock counts as d: inout { }
    parallel for i in 0..4 { }
    return 1;
  }
  for i in 0..2 { lock counts as c: inout { break; } }
  for i in 0..2 { lock counts as c: inout { continue; } }
  let n = 3;
  lock n as m: inout { }
  let k: locked int = 1;
  let o = Counts([0; 16], 0);
  lock o as m: inout { }
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 18u) << r.Diagnostics;
  const std::string reach =
      ": error: 'counts' is 'locked': reach its contents in 'lock counts as "
      "c: view { }' (or 'inout')";
  const std::string borrowed =
      ": error: 'counts' is 'locked': a cell's contents are reached in a "
      "'lock' block, not borrowed by '";
  const std::string notLocked =
      "' is not 'locked': a 'lock' block takes a 'locked' variable, field or "
      "parameter";
  expectErrors(
      r, {(":8:11" + reach).c_str(), (":9:11" + reach).c_str(),
          (":10:3" + reach).c_str(), (":11:3" + reach).c_str(),
          (":12:8" + borrowed + "view' parameter 'c' of 'look'").c_str(),
          (":13:10" + borrowed + "inout' parameter 'c' of 'change'").c_str(),
          ":14:8: error: cannot store a 'locked Counts' in parameter 1 of "
          "'take', a 'Counts': a reference converts only from unique, or from "
          "'const' into the other 'const' forms",
          ":16:5: error: 'c' is a 'view' local; cannot assign to its field "
          "'total'",
          ":17:5: error: 'c' is a 'view' local; cannot assign to its elements",
          ":19:13: error: 'await' cannot be used in a 'lock' block: a wait "
          "holding a lock",
          ":20:5: error: a 'lock' block cannot hold another 'lock'",
          ":21:14: error: a 'parallel for' cannot run in a 'lock' block",
          ":22:5: error: 'return' cannot leave a 'lock' block",
          ":24:45: error: 'break' cannot leave a 'lock' block",
          ":25:45: error: 'continue' cannot leave a 'lock' block",
          (":27:8: error: 'n" + notLocked).c_str(),
          ":28:3: error: 'locked' holds an object in the prototype, not a "
          "'int' (wrap the value in a class)",
          (":30:8: error: 'o" + notLocked).c_str()});
}

// -- Heap buffers of the enclosing function

// A `mem::Buffer<T>` of numbers that the enclosing function owns is used
// in a body as an `inout` array is: read at any index (`at`, `get`,
// `len`), written at the loop's variable only (`set`, the index the
// variable or a conversion of it), and never reshaped, since `grow`,
// `reserve` and the rest could move its storage under the other
// iterations.
TEST(Concurrency, ParallelForReadsAndWritesABufferOfTheEnclosingFunction) {
  auto r = semaCheck(R"(import ::memory as mem;
fn main() -> int {
  xs = mem::Buffer<float>();
  ys = mem::Buffer<int>();
  let g = xs.grow(16);
  let h = ys.grow(16);
  s = 0.0;
  parallel for i in 0..16 {
    let u = unsigned<int>(i);
    s += xs.at(u) + xs.at(unsigned<int>(i + 1) % 16) + float<unsigned>(xs.len());
    let a = xs.set(unsigned<int>(i), xs.at(u) + 1.0);
    let b = ys.set(unsigned<int>(i), ys.get(unsigned<int>(15 - i)));
  }
  return 0;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, ParallelForWritesABufferAtTheLoopsVariableOnly) {
  auto r = semaCheck(R"(import ::memory as mem;
fn main() -> int {
  ys = mem::Buffer<float>();
  let g = ys.grow(16);
  parallel for i in 0..16 {
    let a = ys.set(unsigned<int>(i + 1), 1.0);
    let b = ys.set(unsigned<int>(15), 1.0);
    let c = ys.reserve(32);
    let d = ys.grow(1);
    let e = ys.set(unsigned<int>(i), 2.0);
  }
  return 0;
}
)");
  expectErrors(r, {"6:20: error: 'ys' is written in a 'parallel for' body at "
                   "an index other than the loop's variable 'i': two "
                   "iterations could write one element",
                   "7:20: error: 'ys' is written in a 'parallel for' body at "
                   "an index other than the loop's variable 'i'",
                   "8:13: error: 'ys' (a 'Buffer<float>' of the enclosing "
                   "function) is read ('at', 'get', 'len') and written at the "
                   "loop's variable ('set') in a 'parallel for' body; "
                   "'reserve' could move its storage under the other "
                   "iterations",
                   "9:13: error: 'ys' (a 'Buffer<float>' of the enclosing "
                   "function) is read ('at', 'get', 'len') and written at the "
                   "loop's variable ('set') in a 'parallel for' body; 'grow' "
                   "could move its storage under the other iterations"});
  expectCleanLines(r, {":10:"});
}

// A buffer of references stays out of a body, as the references do: its
// elements would be counted from several threads.
TEST(Concurrency, ParallelForRejectsABufferOfReferences) {
  auto r = semaCheck(R"(import ::memory as mem;
class Node { w: int; }
fn main() -> int {
  ns = mem::Buffer<Node>();
  let g = ns.grow(4);
  total = 0;
  parallel for i in 0..4 {
    total += ns.at(unsigned<int>(i)).w;
  }
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(r.Diagnostics.find("'ns' (a 'Buffer<Node>') cannot be used in a "
                               "'parallel for' body"),
            std::string::npos)
      << r.Diagnostics;
}
