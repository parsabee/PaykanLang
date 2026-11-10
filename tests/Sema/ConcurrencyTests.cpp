// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the concurrency prototype's checks (docs/design/concurrency.md;
// src/Sema/SemaConcurrency.cpp and the visitors in Sema.cpp that call it):
// `async fn` and `await`, the send check, `Copyable`, `parallel for`
// bodies, `atomic` places and `locked mut` cells.  The programs of
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
class Weak { w: weak Cache?; }
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
  let c: shared = Cache(2);
  let w = Weak(c);
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
       ":35:15: error: cannot send 'w' (a 'Weak'): field 'w' is 'weak': a "
       "weak load is a plain retain",
       ":37:17: error: cannot copy 'o' (a unique 'Node') into a task: write "
       "'move o' to transfer it",
       ":38:36: error: a task cannot be started in a 'parallel for' body"});
}

// What crosses: a moved unique reference, a temporary (`move` before one
// does nothing), a class whose reference field is a handle, a cell's
// handle and the handles of an `atomic shared` object and a `locked mut`
// cell, each copied into any number of tasks, and a `Str` temporary.
TEST(Concurrency, HandlesAndTemporariesCrossToATask) {
  auto r = semaCheck(R"(class Cache { hits: atomic int; }
class Job { name: Str; cache: atomic shared Cache; }
class Node { id: int; }
class Counts { total: int; }
async fn consume(n: Node) -> int { return n.id; }
async fn run(j: Job, x: int) -> int { j.cache.hits += 1; return j.name.len() + x; }
async fn cell(c: atomic int) -> int { return c; }
async fn frozen(n: atomic shared Node) -> int { return n.id; }
async fn guarded(c: locked mut Counts) -> int { lock c as v: inout { v.total += 1; } return 1; }
async fn opt(n: Node?) -> int { return 1; }
async fn name(s: Str) -> int { return s.len(); }
fn main() -> int {
  let b = Node(2);
  let t1 = consume(move b);
  let t2 = consume(Node(3));
  let t3 = consume(move Node(4));
  let shared_cache: atomic shared Cache = Cache(0);
  let t4 = run(Job("job", shared_cache), 3);
  let k: atomic int = 0;
  let t5 = cell(k);
  let t6 = cell(k);
  let fr: atomic shared Node = Node(5);
  let t7 = frozen(fr);
  let t8 = frozen(fr);
  let lk: locked mut Counts = Counts(0);
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

// At a conversion to `atomic shared` or `locked mut`, the object is frozen
// or made a cell with everything it holds, so the send check runs on its
// fields, named by path.  A unique value converts, moved or a temporary,
// `mut` or not.
TEST(Concurrency, SendCheckAtAConversionToAtomicSharedOrLocked) {
  auto r = semaCheck(R"(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
class Holder { job: Job; }
class Fun { f: () -> int; }
class Any { o: Obj; }
class Node { id: int; }
fn main() -> int {
  let k: atomic shared Holder = Holder(Job("k", Cache(2)));
  let l: locked mut Holder = Holder(Job("l", Cache(3)));
  let m: locked mut Fun = Fun(fn() => 1);
  let a: atomic shared Any = Any(Node(0));
  let n = Node(1);
  let f: atomic shared Node = move n;
  let g: locked mut Node = Node(2);
  let h: atomic shared Node = Node(3);
  let c: mut Node = Node(4);
  let v: atomic shared Node = move c;
  return f.id + h.id + v.id;
})");
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  const std::string shared =
      "field 'job.cache' is 'shared Cache': a 'shared' reference is aliased "
      "on its own thread";
  expectErrors(
      r,
      {(":8:3: error: cannot make 'Holder(...)' (a 'Holder') 'atomic "
        "shared': " +
        shared)
           .c_str(),
       (":9:3: error: cannot make 'Holder(...)' (a 'Holder') 'locked mut': " +
        shared)
           .c_str(),
       ":10:3: error: cannot make 'Fun(...)' (a 'Fun') 'locked mut': field "
       "'f' is a function value, whose captures are not known",
       ":11:3: error: cannot make 'Any(...)' (a 'Any') 'atomic shared': "
       "field 'o' is an 'Obj', which may be any class"});
  expectCleanLines(r, {":12:", ":13:", ":14:", ":15:", ":16:", ":17:"});
}

// -- `Copyable`

// `Copyable` is structural: numbers, `bool`, `char`, an `atomic` handle's
// value, a function value (its copies share its closure), and an enum,
// tuple or struct of Copyable parts (a struct's `shared` field counts) meet
// a `T: Copyable` bound, and so does a `shared` reference, which binds `T`
// to its form (#368, tests/Sema/QualifiedPartTests.cpp).  A unique class, a
// `Str`, and a struct holding a unique reference do not, with the reason;
// and `impl Copyable for X` is an error.
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
  let k = twice(fn() => 1);
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
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  const std::string needs =
      "' does not implement trait 'Copyable', which 'twice' requires of 'T': ";
  const std::string moveOnly = "a reference, a 'Str' or a task is move-only";
  // NOLINTBEGIN(bugprone-suspicious-missing-comma)
  expectErrors(r, {":5:1: error: 'Copyable' is built in: a type is Copyable by "
                   "what it "
                   "holds, and cannot be implemented",
                   (":7:11: error: 'Named" + needs + moveOnly).c_str(),
                   (":8:11: error: 'Pair" + needs +
                    "it holds move-only field 'left' ('Node')")
                       .c_str(),
                   (":9:11: error: 'Str" + needs + moveOnly).c_str()});
  // NOLINTEND(bugprone-suspicious-missing-comma)
}

// -- `parallel for`

// A body reads the enclosing function's values, arrays of values, `atomic`
// variables and handles (an `atomic shared` object's fields, a `locked mut`
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
  hits: atomic int = 0;
  let cfg: atomic shared Config = Config(0, 50);
  let counts: locked mut Counts = Counts(0);
  let st: atomic shared Stats = Stats(0);
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

// An `atomic` variable of the enclosing function (an `atomic shared`
// object's field too) changed by a statement of the body itself changes on
// every iteration, which bounces its cache line between cores: a warning,
// not an error, and none for a change under an `if`, a `match` or an inner
// loop, or of an atomic the body declares.
TEST(Concurrency, AnAtomicChangedEveryIterationWarns) {
  auto r = semaCheck(R"(class Stats { served: atomic int; }
fn main() -> int {
  hits: atomic int = 0;
  let st: atomic shared Stats = Stats(0);
  parallel for i in 0..100 {
    hits += 1;
    st.served += 1;
    hits = 3;
  }
  parallel for i in 0..100 {
    if i % 10 == 0 { hits += 1; st.served += 1; }
    mine: atomic int = 0;
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
// any other value makes a new cell.  The `atomic` fields of an
// `atomic shared` object still change, in a task or a parallel body.
TEST(Concurrency, AtomicPlaces) {
  auto r = semaCheck(
      R"(class Stats { name: Str; served: atomic int; bytes: atomic float; done: atomic bool; }
impl Stats {
  fn __init__(name: Str, served: atomic int) {
    self.name = move name; self.served = served; self.bytes = 0.0; self.done = False;
  }
}
class Box { n: atomic int; }
async fn record(st: atomic shared Stats, id: int, n: int) -> int {
  for i in 0..n { st.served += 1; st.bytes += 0.5; }
  return id;
}
fn bump(c: atomic int, by: int) { c += by; }
fn fresh(v: int) -> atomic int { return v; }
fn same(c: atomic int) -> atomic int { return c; }
fn main() -> int {
  a: atomic i8 = 120;
  a += 10;
  u: atomic u8 = 250;
  u -= 10;
  f: atomic f32 = 1.5;
  f += 0.25;
  b: atomic bool = False;
  let was: bool = b.exchange(True);
  c: atomic char = 'a';
  let old: char = c.exchange('b');
  n: atomic int = 5;
  e0 = 5;
  let swapped: bool = n.compareExchange(e0, 9);
  m = n;
  m += 1;
  bump(n, 10);
  k = fresh(3);
  k += 1;
  k2 = same(k);
  k2 -= 1;
  let shared_n: atomic int = 1;
  let st: atomic shared Stats = Stats("s", shared_n);
  let t1 = record(st, 1, 10);
  let ids = await t1;
  st.done = True;
  h = st.served;
  h += 1;
  let v: int = st.served;
  let box = Box(0);
  box.n += 1;
  hn = box.n;
  hn = 4;
  let plain: int = n;
  let sum = n + m + plain + v + k + hn;
  e1 = 9;
  e2 = 2;
  let r = n.compareExchange(e1, 1) && st.served.compareExchange(e2, 0);
  return sum + ids + (if was || old == 'a' || swapped || r then 1 else 0);
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// A `let` cell is read through its name and never changed by it: `=`,
// `+=` and `-=` are assignments to it, and `exchange` and
// `compareExchange` change it.  A cell that changes is declared without
// `let`; a cell field of a `let` object is the object's own, and changes.
// An atomic matters only when threads change it, so a `let` one warns,
// naming its value as the alternative where one can be taken (not from a
// call's new cell); `let k = m` is a read-only copy of the value, no cell.
TEST(Concurrency, ALetCellIsNotChangedThroughItsName) {
  auto r = semaCheck(R"(class Stats { served: atomic int; }
fn main() -> int {
  let n: atomic int = 1;
  n = 2;
  n += 1;
  n -= 1;
  let was = n.exchange(3);
  e = 3; let ok = n.compareExchange(e, 4);
  let st = Stats(0);
  st.served += 1;
  let old = st.served.exchange(5);
  m: atomic int = 1;
  m += 1;
  let k = m;
  let c = fresh(2);
  return n + m + k + c + st.served + old;
}
fn fresh(v: int) -> atomic int { return v; })");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  const std::string letAtomic =
      "' is a 'let' atomic: nothing changes it through its name, and an "
      "atomic matters only when threads change it; declare it without 'let'";
  for (const std::string &diag :
       {":3:3: warning: 'n" + letAtomic + ", or as its value ('let n: int')",
        ":15:3: warning: 'c" + letAtomic + "\n"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(countWarnings(r), 2u) << r.Diagnostics;
  const std::string reassigned =
      ": error: 'n' is declared with 'let' and cannot be reassigned";
  expectErrors(
      r, {(":4:3" + reassigned).c_str(), (":5:3" + reassigned).c_str(),
          (":6:3" + reassigned).c_str(),
          ":7:13: error: 'n' is declared with 'let' and cannot be changed by "
          "'exchange'",
          ":8:19: error: 'n' is declared with 'let' and cannot be changed by "
          "'compareExchange'"});
  expectCleanLines(r, {":9:", ":10:", ":11:", ":12:", ":13:", ":14:", ":16:"});
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
  hits: atomic int = 0;
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
                ":9:3: error: 'atomic' holds a number, a 'bool', a 'char' or a "
                "fixed array of them, "
                "not 'Str': an object crosses threads as 'atomic shared "
                "Str' (frozen) or 'locked mut Str'",
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
  hits: atomic int = 0;
  let b = Box(0);
  ei = 1; ef = 1.0;
  let x = hits.load();
  b.n.compareExchange(1);
  b.n.exchange(1, 2);
  let r: int = hits.compareExchange(ei, 2);
  let e: bool = hits.exchange(5);
  let t = hits.exchange("s");
  let u = hits.compareExchange(ei, True);
  f: atomic float = 1.0;
  let w: bool = f.compareExchange(ef, 2.0);
  let lit = hits.compareExchange(1, 2);
  return 0;
})");
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  expectErrors(
      r, {":6:11: error: 'atomic int' has no method 'load': an atomic place "
          "has 'exchange(v)' and 'compareExchange(expected, desired)'",
          ":7:3: error: method 'compareExchange' expects 2 argument(s), got 1",
          ":8:3: error: method 'exchange' expects 1 argument(s), got 2",
          ":9:3: error: initializer of type 'bool' does not match declared "
          "type 'int' for variable 'r'",
          ":10:3: error: initializer of type 'int' does not match declared "
          "type 'bool' for variable 'e'",
          ":11:25: error: argument 1 of 'exchange' has type 'Str', expected "
          "'int'",
          ":12:36: error: argument 2 of 'compareExchange' has type 'bool', "
          "expected 'int'",
          ":15:34: error: 'compareExchange' writes the value it found into "
          "its first argument"});
  expectCleanLines(r, {":13:", ":14:"});
}

// -- `locked`

// A `locked mut` cell is made where a unique object flows into a `locked
// mut` slot (a variable, a parameter, a field, a result); its handle copies
// into tasks, variables and parallel bodies, and its contents are reached
// in a `lock` block: `inout` to change them, `view` to read.
TEST(Concurrency, LockedCells) {
  auto r = semaCheck(R"(class Counts { hits: int[16]; total: int; }
class Holder { cell: locked mut Counts; }
async fn record(counts: locked mut Counts, id: int, n: int) -> int {
  for i in 0..n {
    lock counts as c: inout { c.hits[(id + i) % 16] += 1; c.total += 1; }
  }
  return id;
}
fn total(counts: locked mut Counts) -> int {
  sum = 0;
  lock counts as v: view { for i in 0..16 { sum += v.hits[i]; } sum += v.total; }
  return sum;
}
fn make() -> locked mut Counts { return Counts([0; 16], 0); }
fn main() -> int {
  let counts: locked mut Counts = Counts([0; 16], 0);
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
// `continue`; a `lock` of what is not `locked`; and a `locked mut` value
// (the prototype locks objects).
TEST(Concurrency, LockedRules) {
  auto r = semaCheck(R"(class Counts { hits: int[16]; total: int; }
async fn ping(n: int) -> int { return n; }
fn look(c: view Counts) -> int { return c.total; }
fn change(c: inout Counts) { c.total = 1; }
fn take(c: Counts) -> int { return c.total; }
fn main() -> int {
  let counts: locked mut Counts = Counts([0; 16], 0);
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
  let k: locked mut int = 1;
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
          ":14:8: error: cannot store a 'locked mut Counts' in parameter 1 of "
          "'take', a 'Counts': a 'shared' reference, a frozen object and a "
          "cell convert only as they are",
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
          ":28:3: error: 'locked mut' holds an object in the prototype, not a "
          "'int' (wrap the value in a class)",
          (":30:8: error: 'o" + notLocked).c_str()});
}

// The lock buys the writes: `locked T` without `mut` locks an object
// nothing writes, which `atomic shared T` gives with no lock, so each
// declaration of one (a variable, a field, a parameter, a result) is a
// warning, not an error.  Its contents are still read in a `lock ... as v:
// view` block; a `lock ... as v: inout` block, or a write outside any
// block, is refused, naming the `locked mut T` to declare; and the two
// forms of cell convert into neither.
TEST(Concurrency, ALockedCellWithoutMutWarns) {
  auto r = semaCheck(R"(class Counts { hits: int[16]; total: int; }
class Holder { cell: locked Counts; }
fn read(c: locked Counts) -> int { n = 0; lock c as v: view { n = v.total; } return n; }
fn give() -> locked Counts { return Counts([0; 16], 0); }
fn main() -> int {
  let counts: locked Counts = Counts([0; 16], 0);
  lock counts as c: inout { c.total += 1; }
  counts.total += 1;
  let h = Holder(give());
  lock h.cell as c: inout { c.total = 1; }
  let other = counts;
  lock other as c: inout { }
  let m: locked mut Counts = Counts([0; 16], 0);
  let k: locked Counts = m;
  let j: locked mut Counts = counts;
  return read(counts) + read(h.cell);
})");
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  const std::string warns =
      ": warning: 'locked Counts' locks an object nothing writes; 'atomic "
      "shared Counts' is read from every thread with no lock";
  for (const std::string &diag :
       {":2:16" + warns, ":3:9" + warns, ":4:1" + warns, ":6:3" + warns,
        ":14:3" + warns})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(countWarnings(r), 5u) << r.Diagnostics;
  const std::string never =
      "' is 'locked Counts', whose contents are never written; declare it "
      "'locked mut Counts' ";
  const std::string asTheyAre =
      "': a 'shared' reference, a frozen object and a cell convert only as "
      "they are";
  expectErrors(
      r, {(":7:18: error: 'counts" + never +
           "for a 'lock ... as c: inout' "
           "block")
              .c_str(),
          (std::string(":8:3: error: 'counts' is 'locked': reach its contents "
                       "in 'lock counts as c: view { }' (or 'inout')"))
              .c_str(),
          (":10:18: error: 'h.cell" + never +
           "for a 'lock ... as c: inout' "
           "block")
              .c_str(),
          (":12:17: error: 'other" + never +
           "for a 'lock ... as c: inout' "
           "block")
              .c_str(),
          (":14:3: error: cannot store a 'locked mut Counts' in variable 'k', "
           "a 'locked Counts" +
           asTheyAre)
              .c_str(),
          (":15:3: error: cannot store a 'locked Counts' in variable 'j', a "
           "'locked mut Counts" +
           asTheyAre)
              .c_str()});
  expectCleanLines(r, {":16:"});
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

// -- The copy rule on borrowed results and conditionals (#368)

TEST(Concurrency, APartOfAnInoutResultIsAPlace) {
  auto r = semaCheck(R"(
class N { v: int; }
class W { n: N; }
fn f(w: inout W) -> inout W { return w; }
fn g(w: inout W) -> int {
  let x = f(w).n;
  return x.v;
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":6:3: error: cannot copy 'f(...).n' (a unique 'N')"});
}

TEST(Concurrency, ASharedMethodsReceiverIsNoTemporaryWhenBorrowed) {
  auto r = semaCheck(R"(
import ::collections as cl;
class C { v: int; }
impl C { shared view fn share() -> shared C { return self; } }
class W { c: C; }
fn f(w: inout W) -> inout W { return w; }
fn fromView(l: view cl::List<C>) -> int {
  let s = l.at(0).share();
  return s.v;
}
fn fromInout(w: inout W) -> int {
  let s = f(w).c.share();
  return s.v;
}
fn fromConditional(a: C, b: C, p: bool) -> int {
  let s = (if p then a else b).share();
  return s.v;
}
fn main() -> int { return 0; }
)");
  expectErrors(r,
               {":8:11: error: 'share' is a 'shared view fn', which takes its "
                "receiver as a 'shared C'; 'l.at(...)' is a 'C'",
                ":12:11: error: 'share' is a 'shared view fn', which takes its "
                "receiver as a 'shared C'; 'f(...).c' is a 'C'",
                "'a' is a 'C'", "'b' is a 'C'"});
}

TEST(Concurrency, ASharedMethodOnATemporaryOrASharedBranchIsAccepted) {
  auto r = semaCheck(R"(
class C { v: int; }
impl C { shared view fn share() -> shared C { return self; } }
fn g(a: shared C, b: shared C, p: bool) -> int {
  let s = (if p then a else b).share();
  let t = C(1).share();
  return s.v + t.v;
}
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AFieldIsNotMovedOutThroughAHandle) {
  auto r = semaCheck(R"(
class N { v: int; }
class H { n: N; }
fn f(h: shared mut H) -> N { return move h.n; }
fn main() -> int { return 0; }
)");
  expectErrors(r, {":4:37: error: cannot move 'h.n': 'h' is a 'shared mut H', "
                   "which other references reach; write 'mem::replace(h.n, "
                   "v)'"});
}

TEST(Concurrency, APartOfAConditionalIsAPartOfItsBranches) {
  auto r = semaCheck(R"(
class N { v: int; }
class W { c: mut N; }
impl N { shared view fn share() -> shared N { return self; } }
fn f(w: inout W) -> inout W { return w; }
fn copied(a: mut W, b: mut W, p: bool) -> int {
  s = (if p then a else b).c;
  return s.v;
}
fn fromInout(a: inout W, b: inout W, p: bool) -> int {
  let s = (if p then f(a) else f(b)).c;
  return s.v;
}
fn shared1(a: mut W, b: mut W, p: bool) -> int {
  let s = (if p then a else b).c.share();
  return s.v;
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":7:3: error: cannot copy", ":11:3: error: cannot copy",
                   ":15:11: error: 'share' is a 'shared view fn'"});
}

TEST(Concurrency, APartOfAConditionalOfTemporariesIsNoPlace) {
  auto r = semaCheck(R"(
class N { v: int; }
class W { c: N; }
fn mk() -> W { return W(N(1)); }
fn g(p: bool) -> int {
  s = (if p then mk() else mk()).c;
  return s.v;
}
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AViewResultLendsTheFormOfWhatItWasGiven) {
  auto r = semaCheck(R"(
class C { v: int; }
impl C { shared view fn share() -> shared C { return self; } }
fn pick<T>(x: view T) -> view T { return x; }
fn main() -> int {
  a: shared C = C(1);
  let s = pick(a).share();
  return s.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AViewResultOfAUniqueArgumentLendsNoShare) {
  auto r = semaCheck(R"(
class C { v: int; }
impl C { shared view fn share() -> shared C { return self; } }
fn pick<T>(x: view T) -> view T { return x; }
fn main() -> int {
  a: C = C(1);
  let s = pick(a).share();
  return s.v;
}
)");
  expectErrors(r, {":7:11: error: 'share' is a 'shared view fn'"});
}

TEST(Concurrency, AnArityErrorInACallOfAMoveOutMethodIsAnErrorAlone) {
  auto r = semaCheck(R"(
class N { v: int; }
class K { v: int; }
impl K { fn t(a: inout N) -> N { return N(1); } }
fn main() -> int {
  k: mut K = K(1);
  a: N[1] = [N(1)];
  let x = k.t(a[0], 1);
  return 0;
}
)");
  expectErrors(r, {"method 't' expects 1 argument(s), got 2"});
}

TEST(Concurrency, APartPastATemporaryHandleIsAPlace) {
  auto r = semaCheck(R"(
class N { v: int; }
impl N { shared view fn me() -> shared N { return self; } }
class H { n: mut N; }
struct P { n: N; }
class B { p: P; }
fn id(h: shared mut H) -> shared mut H { return h; }
fn same(b: shared B) -> shared B { return b; }
fn f(x: P) -> int { return x.n.v; }
fn copied(h: shared mut H) -> int {
  x: mut N = id(h).n;
  return x.v;
}
fn shared1(h: shared mut H) -> int {
  let s = id(h).n.me();
  return s.v;
}
fn byValue(b: shared B) -> int { return f(same(b).p); }
fn main() -> int { return 0; }
)");
  expectErrors(r, {":11:3: error: cannot copy", ":15:11: error: 'me' is a",
                   "error: cannot copy 'same(...).p'"});
}

TEST(Concurrency, AUniqueTemporarysPartIsNoPlace) {
  auto r = semaCheck(R"(
class N { v: int; }
class H { n: mut N; }
fn mk() -> H { return H(N(1)); }
fn g() -> int {
  x: mut N = mk().n;
  return x.v;
}
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AViewResultThatMayBeInsideAnArgumentLendsNothing) {
  auto r = semaCheck(R"(
class C { v: int; }
impl C { shared view fn me() -> shared C { return self; } }
class Holder { c: C; }
fn first(l: view Holder, x: view C) -> view C { return l.c; }
fn main() -> int {
  hh = Holder(C(1));
  s: shared C = C(2);
  let k = first(hh, s).me();
  return k.v;
}
)");
  expectErrors(r, {":9:11: error: 'me' is a 'shared view fn'"});
}

// -- `atomic` arrays (#368)

TEST(Concurrency, AnAtomicArrayIsUsedByElement) {
  auto r = semaCheck(R"(class C { hits: atomic int[2]; }
impl C { fn __init__() { self.hits = [0, 0]; } }
fn total(a: atomic int[2]) -> int { return a[0] + a[1]; }
fn main() -> int {
  a: atomic int[2] = [1, 2];
  a[0] = 5;
  a[1] += 1;
  a[1] -= 1;
  let old = a[0].exchange(9);
  e = 9;
  let ok = a[0].compareExchange(e, 1);
  x: int = a[1];
  m = a;
  m[0] += 1;
  let c = C();
  c.hits[1] += 1;
  return old + x + total(a) + total(c.hits);
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AnAtomicArrayIsNotReadOrLentWhole) {
  auto r = semaCheck(R"(fn bump(b: inout bool) { b = True; }
fn cell(b: atomic bool) { }
class C { a: atomic bool[4]; }
fn main() -> int {
  seen: atomic bool[4] = [False; 4];
  let all: bool[4] = seen;
  seen = [True; 4];
  bump(seen[3]);
  cell(seen[3]);
  h = seen[3];
  let n = seen.exchange(True);
  return 0;
}
fn f(c: mut C) { c.a = [True; 4]; }
)");
  expectErrors(
      r,
      {":6:22: error: 'seen' is an 'atomic bool[4]': it is read by element",
       ":7:3: error: 'seen' is an 'atomic bool[4]': it is written by element",
       "'seen[3]' is an element of an 'atomic bool[4]': it has no handle "
       "and cannot be",
       ":9:8: error: 'seen[3]' is an element of an 'atomic bool[4]': it has "
       "no handle to share",
       ":10:7: error: 'seen[3]' is an element of an 'atomic bool[4]': it has "
       "no handle to share",
       ":11:11: error: 'atomic bool[4]' has no method 'exchange': an array "
       "is used by element",
       ":14:18: error: 'c.a' is an 'atomic bool[4]': it is written by "
       "element"});
}

TEST(Concurrency, ALetAtomicArrayIsNotChangedThroughItsName) {
  auto r = semaCheck(R"(fn main() -> int {
  let a: atomic int[2] = [1, 2];
  a[0] = 5;
  let old = a[0].exchange(9);
  return old;
}
)");
  expectErrors(
      r, {":3:3: error: 'a' is declared with 'let' and cannot be reassigned",
          ":4:13: error: 'a' is declared with 'let' and cannot be changed by "
          "'exchange'"});
}

TEST(Concurrency, CompareExchangeTakesAVariableOfTheCellsType) {
  auto r = semaCheck(R"(class B { e: int; }
fn main() -> int {
  n: atomic int = 1;
  let a = n.compareExchange(1, 2);
  let b = n.compareExchange(1 + 1, 2);
  let k = 1;
  let c = n.compareExchange(k, 2);
  m: mut B = B(1);
  let d = n.compareExchange(m.e, 2);
  e = 1;
  let f = n.compareExchange(e, 2);
  return 0;
}
)");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(r, {":4:29: error: 'compareExchange' writes the value it found",
                   ":5:29: error: 'compareExchange' writes the value it found",
                   ":7:29: error:"});
  expectCleanLines(r, {":9:", ":11:"});
}

// -- A cell's contents are reached in a `lock` block only (#368)

TEST(Concurrency, ACellsContentsAreNotBorrowedOrReadPastIt) {
  auto r = semaCheck(R"(class N { v: int; }
class A { inner: locked mut N; }
class W { c: locked mut N; }
fn nested(a: locked mut A) -> int {
  x = 0;
  lock a.inner as v: view { x = v.v; }
  return x;
}
fn viewOf(l: locked mut N) -> int { v: view N = l; return v.v; }
fn inoutOf(l: locked mut N) -> int { v: inout N = l; v.v = 1; return 0; }
fn result(w: view W) -> view N { return w.c; }
fn eq(a: locked Str, b: locked Str) -> bool { return a == b; }
fn add(a: locked Str) -> Str { return a + "x"; }
fn text(a: locked Str) -> Str { return Str(a); }
fn none() -> int {
  o: locked mut N? = None;
  x = 0;
  lock o as v: view { x = 1; }
  return x;
}
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":6:8: error: 'a' is 'locked'", ":9:37: error: 'l' is 'locked'",
          ":10:38: error: 'l' is 'locked'", ":11:34: error: 'w.c' is 'locked'",
          ":12:54: error: 'a' is 'locked'", ":13:39: error: 'a' is 'locked'",
          ":14:40: error: 'a' is 'locked'",
          ":18:8: error: 'o' may be None: 'match' it"});
}

TEST(Concurrency, TwoHandlesOfCellsCompareByIdentity) {
  auto r = semaCheck(R"(class N { v: int; }
fn eq(a: locked mut N, b: locked mut N) -> bool { return a == b; }
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, ALockOperandIsItselfAndNothingElse) {
  auto r = semaCheck(R"(class N { v: int; }
fn pick(x: int, a: locked mut N) -> locked mut N { return a; }
fn main() -> int {
  c: locked mut N = N(1);
  let n = N(2);
  lock pick(c.v, c) as v: view { }
  lock (if False then c else n) as w: view { }
  return 0;
}
)");
  expectErrors(r,
               {":6:13: error: 'c' is 'locked'", "is not 'locked': a 'lock'"});
}

TEST(Concurrency, ACellKeepsTheClassItWasMadeWith) {
  auto r = semaCheck(R"(class B { v: int; }
class D : B { y: int; }
fn main() -> int {
  d: locked mut D = D(1, 2);
  o: locked mut Obj = d;
  b: locked mut B = d;
  return 0;
}
)");
  expectErrors(r, {":5:23: error: a 'locked' cell keeps the class it was made "
                   "with: cannot store 'D' in 'Obj'",
                   ":6:21: error: a 'locked' cell keeps the class"});
}

TEST(Concurrency, AnAtomicPlaceIsNoExpectedOfCompareExchange) {
  auto r = semaCheck(R"(class C { m: atomic int; }
fn f(c: C) {
  n: atomic int = 5;
  m: atomic int = 5;
  a: atomic int[1] = [5];
  n.compareExchange(m, 7);
  n.compareExchange(c.m, 7);
  n.compareExchange(a[0], 7);
}
fn main() -> int { return 0; }
)");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

TEST(Concurrency, AnAtomicCellIsNotLentByAddress) {
  auto r = semaCheck(R"(fn w(a: inout atomic int[2]) -> int { return a[0]; }
fn main() -> int { return 0; }
)");
  expectErrors(r, {"parameter 'a' is 'inout': an 'atomic' cell is never lent"});
}

TEST(Concurrency, AwaitKeepsTheFormOfTheResult) {
  auto r = semaCheck(R"(class R { v: int; }
async fn cell(x: locked mut R) -> locked mut R { return x; }
async fn frozen(x: atomic shared R) -> atomic shared R { return x; }
fn main() -> int {
  let c: locked mut R = R(1);
  let t = cell(c);
  let r = await t;
  let bad = r.v;
  let f: atomic shared R = R(2);
  let u = frozen(f);
  w: mut R = await u;
  lock r as v: view { }
  return 0;
}
)");
  expectErrors(r, {":8:13: error: 'r' is 'locked'",
                   "a 'shared' reference, a frozen object and a cell convert "
                   "only as they are"});
  expectCleanLines(r, {":12:"});
}

TEST(Concurrency, ADerivedClassThatCannotCrossKeepsItsBaseFromCrossing) {
  auto r = semaCheck(R"(class K { v: int; }
class Base { v: int; }
class D : Base { w: weak K?; }
fn f(b: Base) { let r: atomic shared Base = move b; }
fn main() -> int { return 0; }
)");
  expectErrors(r, {"(of class 'D', derived from 'Base')"});
}

TEST(Concurrency, AnAtomicPlaceThroughSeveralFieldsIsItsHandle) {
  auto r = semaCheck(R"(class P { a: atomic int[2]; n: atomic int; }
class Q { p: P; }
fn bump(c: atomic int) { c += 1; }
fn f(q: Q) {
  q.p.a[1] += 4;
  bump(q.p.n);
  h = q.p.a;
  h[0] = 1;
}
fn main() -> int { return 0; }
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, AnAtomicPlaceHasNoAddress) {
  auto r = semaCheck(R"(class C { n: atomic int; }
fn bump(x: inout int) { x += 1; }
fn f(c: mut C) {
  a: atomic int = 1;
  bump(a);
  bump(c.n);
  x: inout = a;
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {"'a' is 'atomic': it has no address and cannot be passed",
                   "'c.n' is 'atomic': it has no address and cannot be passed",
                   "'a' is 'atomic': it has no address and cannot be named"});
}

TEST(Concurrency, AnAtomicVariableIsNotWrittenAfterAMoveOrInALambda) {
  auto r = semaCheck(R"(fn i(x: atomic int) -> int { return x; }
fn moved() -> int {
  a: atomic int = 1;
  b: atomic int = move a;
  a = 3;
  return b;
}
fn self() {
  a: atomic int = 1;
  a += i(move a);
}
fn captured() {
  a: atomic int = 1;
  f = fn() { a += 1; };
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":5:3: error: 'a' was moved", ":10:3: error: 'a' was moved",
                   ":14:14: error: cannot capture 'a'"});
}

TEST(Concurrency, ExpectedIsNotWrittenFromAParallelBody) {
  auto r = semaCheck(R"(fn main() -> int {
  n: atomic int = 5;
  e = 0;
  parallel for i in 0..100 { n.compareExchange(e, i); }
  return e;
}
)");
  expectErrors(r, {"'e' is written by 'compareExchange' in a 'parallel for'"});
}

TEST(Concurrency, AVariableAPlainBindingReadsIsNotMovedInTheArm) {
  auto r = semaCheck(R"(class In { v: int; }
class O { o: In?; }
async fn run(i: O) -> int { return 1; }
fn f(n: O) -> int {
  match n.o {
    x: In { let t = run(move n); return x.v + await t; }
    None { return 0; }
  }
}
fn g(e: In?) -> In? {
  match move e {
    x: In { return move x; }
    None { return None; }
  }
}
fn main() -> int { return 0; }
)");
  expectErrors(r, {":6:25: error: 'n' is read by a 'match' binding"});
  expectCleanLines(r, {":12:"});
}

TEST(Concurrency, AConditionalOfCellsKeepsEachBranchsClass) {
  auto r = semaCheck(R"(class N { v: int; }
class D : N { s: int; }
fn f(p: bool, a: locked mut N, d: locked mut D) -> int {
  let c: locked mut N = if p then a else d;
  lock (if p then a else d) as y: inout { }
  return 0;
}
fn main() -> int { return 0; }
)");
  expectErrors(
      r, {":4:42: error: a 'locked' cell keeps the class", "is not 'locked'"});
}

TEST(Concurrency, ATaskHasNoFormUntilItIsAwaited) {
  auto r = semaCheck(R"(class N { v: int; }
async fn lk() -> locked mut N { return N(2); }
fn main() -> int {
  lock lk() as y: view { }
  return 0;
}
)");
  expectErrors(r, {":4:8: error: 'lk()' is not 'locked'"});
}

TEST(Concurrency, ADerivedClassThatCannotCrossIsFoundOnceAllInstancesExist) {
  auto r = semaCheck(R"(class In { v: int; }
class Base { n: int; }
class D<T> : Base { c: T; }
fn share(b: Base) -> atomic shared Base { return move b; }
fn main() -> int {
  s: shared In = In(1);
  let f = share(D<shared In>(1, s));
  return 0;
}
)");
  expectErrors(r, {"extends 'Base', which is sent to another thread"});
}

TEST(Concurrency, ALoopOrALambdaInsideALockBlockIsTheBlocksOwn) {
  auto r = semaCheck(R"(class N { v: int; }
fn main() -> int {
  let c: locked mut N = N(1);
  lock c as v: inout {
    for i in 0..3 { if i == 1 { break; } else { continue; } }
    let f = fn() -> int { return 2; };
    v.v = f();
  }
  parallel for i in 0..4 { let g = fn() -> int { return 1; }; }
  return 0;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Concurrency, ATaskIsHeldByAVariable) {
  auto r = semaCheck(R"(async fn ping(n: int) -> int { return n; }
fn pass<T>(t: T) -> T { return move t; }
fn main() -> int {
  let p = (ping(1), ping(2));
  let q = pass(ping(1));
  return 0;
}
)");
  expectErrors(r, {"a tuple cannot hold a task yet",
                   "cannot be a task yet: a task is held by a variable"});
}

TEST(Concurrency, AnOptionalHandleIsNotMatchedOrClonedYet) {
  auto r = semaCheck(R"(fn none<T: Copyable>() -> T? { return None; }
fn main() -> int {
  v = none<atomic int>();
  x = v.clone();
  match none<atomic int>() { y: atomic int { } None { } }
  return 0;
}
)");
  expectErrors(r, {"'clone' of an optional 'atomic' handle",
                   "a 'match' on an optional 'atomic' handle"});
}
