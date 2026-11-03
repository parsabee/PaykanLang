// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: `move` (docs/design/concurrency.md, "Copies and `move`" and
// "Use after move"; the prototype's move analysis in
// src/Sema/SemaConcurrency.cpp, checkMove / checkPartialMove /
// checkNotMoved / joinMoves / checkLoopIteration).  A forward "maybe
// moved" set runs over each body: `move x` ends x, a branch that moves on
// one path leaves x "may have been moved", a loop checks every iteration's
// end, an assignment revives, and `move x.f` moves a unique field out of an
// owned variable and ends it.  The programs are samples/concurrency/
// 08_moves.pkn and errors/err_moves.pkn taken apart.

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

} // namespace

// `move x` ends x: every later use is an error naming the move, at the use
// (a read, a field write, an `inout` argument, a `view` of it, a second
// move, a read after `y = move x`).  A `let` local and a by-value parameter
// move like a plain variable; `return move j` and a move into a call's
// argument are fine.
TEST(Move, UseAfterMove) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn grab(j: inout Job) -> int { j = Job("g"); return 0; }
fn pass(j: Job) -> Job { return move j; }
fn param(j: Job) -> int { n = keep(move j); return n + keep(j); }
fn main() -> int {
  let a = Job("a");
  n = keep(move a);
  n = n + a.name.len();
  b = Job("b");
  keep(move b);
  b.name = "x";
  c = Job("c");
  keep(move c);
  grab(c);
  d = Job("d");
  keep(move d);
  v: view Job = d;
  e = Job("e");
  keep(move e);
  keep(move e);
  f = Job("f");
  g = move f;
  n = n + keep(f) + keep(g);
  h = Job("h");
  n = n + keep(pass(move h)) + param(Job("p"));
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  expectErrors(
      r, {":7:61: error: 'j' was moved at 7:36; it cannot be used after the "
          "move",
          ":11:11: error: 'a' was moved at 10:12; it cannot be used after the "
          "move",
          ":14:3: error: 'b' was moved at 13:8; it cannot be used after the "
          "move",
          ":17:8: error: 'c' was moved at 16:8; it cannot be used after the "
          "move",
          ":20:17: error: 'd' was moved at 19:8; it cannot be used after the "
          "move",
          ":23:13: error: 'e' was moved at 22:8; it cannot be used after the "
          "move",
          ":26:16: error: 'f' was moved at 25:7; it cannot be used after the "
          "move"});
}

// An `if` joins the branches that reach what follows it: a move on one
// path (the then branch, the else branch, an `elif` chain's first branch, a
// nested `if`, which is the one named) leaves the variable "may have been
// moved", with the `if` and the move's location; a move on every path
// leaves it moved, at the first path's move.  A branch that assigns the
// variable again after its move, or ends with a `return`, moves nothing
// past the `if`.
TEST(Move, AMoveOnOnePathOfAnIf) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int {
  n = 0;
  a = Job("a");
  if n > 3 { keep(move a); }
  n = n + keep(a);
  b = Job("b");
  if n > 3 { n = 1; } else { keep(move b); }
  n = n + keep(b);
  c = Job("c");
  if n > 3 { keep(move c); } else { keep(move c); }
  n = n + keep(c);
  d = Job("d");
  if n > 3 { if n > 4 { keep(move d); } }
  n = n + keep(d);
  e = Job("e");
  if n > 3 { keep(move e); } elif n > 1 { n = 2; }
  n = n + keep(e);
  f = Job("f");
  if n > 3 { keep(move f); f = Job("ff"); } else { keep(move f); return n; }
  n = n + keep(f);
  g = Job("g");
  if n > 3 { keep(move g); return n; }
  n = n + keep(g);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  expectErrors(
      r, {":9:16: error: 'a' may have been moved (by the 'if' at 8:3, at "
          "8:19); it cannot be used after a move",
          ":12:16: error: 'b' may have been moved (by the 'if' at 11:3, at "
          "11:35); it cannot be used after a move",
          ":15:16: error: 'c' was moved at 14:19; it cannot be used after the "
          "move",
          ":18:16: error: 'd' may have been moved (by the 'if' at 17:14, at "
          "17:30); it cannot be used after a move",
          ":21:16: error: 'e' may have been moved (by the 'if' at 20:3, at "
          "20:19); it cannot be used after a move"});
  for (const char *line : {":24:", ":27:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A `match` joins its arms the same way: a move in one arm leaves the
// variable "may have been moved (by the 'match' ...)", a move in every arm
// leaves it moved.  A match no arm of which takes the subject (an optional
// matched without `None`) falls through as it came, one more path.  An arm
// that returns, or assigns the variable again, moves nothing past the
// match.
TEST(Move, AMoveOnOnePathOfAMatch) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int {
  n = 0;
  o: Job? = Job("o");
  a = Job("a");
  match n { 3 { keep(move a); } _ { } }
  n = n + keep(a);
  b = Job("b");
  match n { 3 { keep(move b); } _ { keep(move b); } }
  n = n + keep(b);
  c = Job("c");
  match o { x: Job { keep(move c); } }
  n = n + keep(c);
  d = Job("d");
  match o { x: Job { keep(move d); return n; } None { } }
  n = n + keep(d);
  e = Job("e");
  match n { 3 { keep(move e); e = Job("ee"); } _ { } }
  n = n + keep(e);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(
      r, {":10:16: error: 'a' may have been moved (by the 'match' at 9:3, at "
          "9:22); it cannot be used after a move",
          ":13:16: error: 'b' was moved at 12:22; it cannot be used after the "
          "move",
          ":16:16: error: 'c' may have been moved (by the 'match' at 15:3, "
          "at 15:27); it cannot be used after a move"});
  for (const char *line : {":19:", ":22:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A loop checks every iteration's end, the body's end and each `continue`:
// a variable of the enclosing function that the body moves is assigned
// again before it, or the move is an error naming the loop (and the
// `continue`).  A `while` condition reads the variable before the body
// moves it.  Assigning again on every path is fine, a `continue` before
// the move too.  Nested loops each check their own iteration, so a move in
// the inner one is reported for both.  The body's own variables are
// declared anew each iteration: a move of one is fine, and a later use in
// the same iteration is a plain use after move.
TEST(Move, ALoopChecksEveryIterationsEnd) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int {
  n = 0;
  let a = Job("a");
  for i in 0..2 { n = n + keep(move a); }
  b = Job("b");
  for i in 0..3 { n = n + keep(move b); b = Job("bb"); }
  n = n + keep(b);
  c = Job("c");
  while c.name.len() > 0 { keep(move c); }
  d = Job("d");
  while n < 100 {
    if n > 0 { keep(move d); continue; }
    d = Job("dd");
    break;
  }
  e = Job("e");
  for i in 0..3 {
    if i == 0 { continue; }
    keep(move e);
    e = Job("ee");
  }
  n = n + keep(e);
  f = Job("f");
  for i in 0..3 {
    keep(move f);
    if i == 1 { f = Job("f1"); continue; }
    f = Job("f2");
  }
  n = n + keep(f);
  g = Job("g");
  for i in 0..2 { for k in 0..2 { keep(move g); } }
  for i in 0..2 { h = Job("h"); n = n + keep(move h); }
  for i in 0..2 { let k = Job("k"); n = n + keep(move k); n = n + keep(k); }
  m = Job("m");
  while n < 100 { n = n + 1; while n < 50 { keep(move m); m = Job("mm"); } }
  n = n + keep(m);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 6u) << r.Diagnostics;
  expectErrors(
      r, {":8:32: error: 'a' is moved in the loop at 8:3 and not assigned "
          "again before the next iteration",
          ":13:33: error: 'c' is moved in the loop at 13:3 and not assigned "
          "again before the next iteration",
          ":16:21: error: 'd' is moved in the loop at 15:3 and not assigned "
          "again before the next iteration (the 'continue' at 16:30)",
          ":35:40: error: 'g' is moved in the loop at 35:19 and not assigned "
          "again before the next iteration",
          ":35:40: error: 'g' is moved in the loop at 35:3 and not assigned "
          "again before the next iteration",
          ":37:72: error: 'k' was moved at 37:50; it cannot be used after the "
          "move"});
  for (const char *line :
       {":10:", ":11:", ":23:", ":26:", ":29:", ":33:", ":36:", ":39:", ":40:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// What follows a loop joins no iteration at all, the last one's end and
// each `break`: a `break` after a move leaves the variable "may have been
// moved (by the 'loop' ...)", until an assignment revives it.  A `return`
// after a move leaves the loop with nothing to join, and a `break` ends its
// iteration without the iteration's check (a `continue` beside it still
// gets one).
TEST(Move, AfterALoop) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int {
  n = 0;
  a = Job("a");
  while True {
    keep(move a);
    break;
  }
  n = n + keep(a);
  a = Job("aa");
  n = n + keep(a);
  b = Job("b");
  while n < 100 {
    if n > 50 { keep(move b); break; }
    n = n + 1;
  }
  n = n + keep(b);
  c = Job("c");
  for i in 0..3 {
    if i == 1 { keep(move c); return n; }
  }
  n = n + keep(c);
  d = Job("d");
  for i in 0..3 {
    if i == 1 { keep(move d); break; }
    if i == 2 { keep(move d); continue; }
  }
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
  expectErrors(
      r, {":12:16: error: 'a' may have been moved (by the 'loop' at 8:3, at "
          "9:10); it cannot be used after a move",
          ":20:16: error: 'b' may have been moved (by the 'loop' at 16:3, at "
          "17:22); it cannot be used after a move",
          ":29:22: error: 'd' is moved in the loop at 27:3 and not assigned "
          "again before the next iteration (the 'continue' at 29:31)"});
  for (const char *line : {":14:", ":25:", ":28:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// An assignment revives a moved variable, after the move or inside the
// branch that moved it.  A `let` local moves but cannot be assigned again,
// so it stays moved.
TEST(Move, AnAssignmentRevives) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int {
  a = Job("a");
  n = keep(move a);
  a = Job("aa");
  n = n + keep(a);
  b = Job("b");
  if n > 3 { keep(move b); b = Job("bb"); }
  n = n + keep(b);
  let c = Job("c");
  n = n + keep(move c);
  c = Job("cc");
  n = n + keep(c);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  expectErrors(
      r, {":15:3: error: 'c' is declared with 'let' and cannot be reassigned",
          ":16:16: error: 'c' was moved at 14:16; it cannot be used after the "
          "move"});
  for (const char *line : {":9:", ":12:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// `move` takes an owned variable: not `self`, a `view` or `inout`
// parameter, a `weak` variable, a variable a function captures by copy, a
// variable a `view` local reads (a move is a write), an array element or a
// tuple's member.  A `for` variable (an integer) is read as it is, a
// temporary (a constructor, a call) is owned already and `move` before it
// does nothing, and a lambda's own local moves like any other.
TEST(Move, WhatMoveRefuses) {
  auto r = semaCheck(R"(class Job {
  name: Str;
  fn me() -> int { return keep(move self); }
}
fn keep(j: Job) -> int { return j.name.len(); }
fn keepN(n: int) -> int { return n; }
fn viewed(j: view Job) -> int { return keep(move j); }
fn lent(j: inout Job) -> int { return keep(move j); }
fn main() -> int {
  n = 0;
  weak w: Job? = Job("w");
  n = n + keep(move w);
  a = Job("a");
  f = fn() -> int { n2 = keep(a); return n2 + keep(move a); };
  n = n + f();
  b = Job("b");
  v: view Job = b;
  n = n + keep(move b);
  n = n + v.name.len();
  xs = [Job("x")];
  n = n + keep(move xs[0]);
  t = (Job("t"), 1);
  n = n + keep(move t.0);
  for i in 0..2 { n = n + keepN(move i); }
  n = n + keep(move Job("t")) + keep(move keep2());
  g = fn() -> int { let own = Job("own"); return keep(move own); };
  return n + g();
}
fn keep2() -> Job { return Job("k"); })");
  EXPECT_EQ(r.ErrorCount, 8u) << r.Diagnostics;
  expectErrors(
      r, {":3:32: error: cannot move 'self': a method borrows its receiver",
          ":7:45: error: cannot move 'j': it is a 'view', a borrow",
          ":8:44: error: cannot move 'j': it is an 'inout' parameter, the "
          "caller's storage",
          ":12:16: error: cannot move 'w': a 'weak' variable holds no strong "
          "reference",
          ":14:52: error: cannot move 'a': the function captures it by copy",
          ":18:16: error: 'b' is viewed by 'view' local 'v' until 'v' is last "
          "used; it cannot be moved",
          ":21:16: error: cannot move 'xs[0]': an element leaves by "
          "'mem::replace' or 'mem::take'",
          ":23:16: error: cannot move 't.0': an element leaves by "
          "'mem::replace' or 'mem::take'"});
  for (const char *line : {":24:", ":25:", ":26:"})
    EXPECT_EQ(r.Diagnostics.find(std::string(".pkn") + line), std::string::npos)
        << line << "\n"
        << r.Diagnostics;
}

// A move is a write for exclusivity: in one call it cannot share storage
// with what the call borrows, a `view` argument before it (the variable, or
// a field of it), the receiver of a method (a `view fn`'s too), or an
// `inout` argument, which the prototype's message counts the move as.  A
// move before a `view` of the same variable is a use after the move.
// Another variable beside it is fine.
TEST(Move, AMoveBesideABorrowInOneCall) {
  auto r = semaCheck(R"(class Job {
  name: Str;
  fn take(j: Job) -> int { return j.name.len(); }
  view fn peek(j: Job) -> int { return j.name.len(); }
}
class Holder {
  job: Job;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn look(j: view Job, k: Job) -> int { return j.name.len() + k.name.len(); }
fn look2(k: Job, j: view Job) -> int { return j.name.len() + k.name.len(); }
fn grab(a: inout Job, b: Job) -> int { a = Job("g"); return b.name.len(); }
fn lookH(h: view Holder, k: Holder) -> int { return 0; }
fn lookJ(j: view Job, k: Holder) -> int { return 0; }
fn main() -> int {
  n = 0;
  let a = Job("a");
  n = n + look(a, move a);
  let b = Holder(Job("b"));
  n = n + lookH(b, move b);
  let c = Holder(Job("c"));
  n = n + lookJ(c.job, move c);
  let d = Job("d");
  n = n + d.take(move d);
  let e = Job("e");
  n = n + e.peek(move e);
  f = Job("f");
  n = n + grab(f, move f);
  let g = Job("g");
  n = n + look2(move g, g);
  h = Job("h");
  i = Job("i");
  n = n + look(h, move i);
  return n;
})");
  EXPECT_EQ(r.ErrorCount, 7u) << r.Diagnostics;
  // The receiver conflicts name the parameter moved into; the prototype
  // leaves a method's parameter name out of them, so only their start is
  // checked.
  expectErrors(
      r, {":18:19: error: 'a' is passed to 'view' parameter 'j'; it cannot "
          "also be moved into parameter 'k'",
          ":20:20: error: 'b' is passed to 'view' parameter 'h'; it cannot "
          "also be moved into parameter 'k'",
          ":22:24: error: 'c' contains 'c.job', passed to 'view' parameter "
          "'j'; it cannot also be moved into parameter 'k'",
          ":24:18: error: 'd' is the receiver of 'take' (not a 'view fn'); it "
          "cannot also be moved into parameter",
          ":26:18: error: 'e' is the receiver of 'view fn peek'; it cannot "
          "also be moved into parameter",
          ":28:19: error: 'f' is passed to two 'inout' parameters ('a' and "
          "'b')",
          ":30:25: error: 'g' was moved at 30:17; it cannot be used after the "
          "move"});
  EXPECT_EQ(r.Diagnostics.find(".pkn:33:"), std::string::npos) << r.Diagnostics;
}

// `move x.f` moves a unique reference field out of an owned variable (a
// plain or `let` local, a by-value parameter) and ends the variable: a
// class's or a struct's field, through unique links (`e.inner.job`), a
// `Str`, an optional or an object of its own.  The variable revives by an
// assignment, after the move or in the branch that moved.
TEST(Move, PartialMoves) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
class Inner {
  job: Job;
}
class Holder {
  job: Job;
  tag: Str;
  maybe: Job?;
  inner: Inner;
  n: int;
}
struct Pair {
  left: Job;
  inner: Inner;
  n: int;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn keepS(s: Str) -> int { return s.len(); }
fn keepO(j: Job?) -> int { return 0; }
fn keepI(i: Inner) -> int { return i.job.name.len(); }
fn make() -> Holder { return Holder(Job("j"), "t", Job("m"), Inner(Job("i")), 1); }
fn fromParam(h: Holder) -> int { return keep(move h.job); }
fn main() -> int {
  n = 0;
  let a = make();
  n = n + keep(move a.job);
  b = make();
  n = n + keepS(move b.tag);
  c = make();
  n = n + keepO(move c.maybe);
  d = make();
  n = n + keepI(move d.inner);
  e = make();
  n = n + keep(move e.inner.job);
  let p = Pair(Job("l"), Inner(Job("i")), 1);
  n = n + keep(move p.left);
  q = Pair(Job("l"), Inner(Job("i")), 1);
  n = n + keep(move q.inner.job);
  f = make();
  n = n + keep(move f.job);
  f = make();
  n = n + keep(move f.job);
  g = make();
  if n > 3 { keep(move g.job); g = make(); }
  n = n + g.n;
  return n + fromParam(make());
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// What `move x.f` refuses: the variable is used after it (the field left,
// the holder ended); a `shared`, `const` or `weak` field is not a unique
// part of the variable (anywhere on the chain), nor is a value field (an
// `int` of a class or a struct), and a field of `self`, of a `view` or
// `inout` parameter, of a captured or `weak` variable, or of a temporary is
// not a field of an owned variable, with the `mem::replace` hint where a
// variable roots it.  A `view` local reading the variable blocks the move
// as it blocks any write.
TEST(Move, PartialMovesRefused) {
  auto r = semaCheck(R"(class Job {
  name: Str;
}
class Inner {
  job: Job;
  other: shared Job;
}
class Holder {
  job: Job;
  other: shared Job;
  fixed: const Job;
  weak back: Job?;
  n: int;
  inner: Inner;
  fn take() -> int { return keep(move self.job); }
}
struct Pair {
  left: Job;
  n: int;
}
fn keep(j: Job) -> int { return j.name.len(); }
fn keepO(j: Job?) -> int { return 0; }
fn make() -> Holder { return Holder(Job("j"), Job("o"), Job("f"), None, 1, Inner(Job("i"), Job("io"))); }
fn fromView(h: view Holder) -> int { return keep(move h.job); }
fn fromInout(h: inout Holder) -> int { return keep(move h.job); }
fn main() -> int {
  n = 0;
  let a = make();
  n = n + keep(move a.job);
  n = n + a.n;
  b = make();
  n = n + keep(move b.other);
  c = make();
  n = n + keep(move c.fixed);
  d = make();
  n = n + keepO(move d.back);
  e = make();
  n = n + move e.n;
  f = make();
  n = n + keep(move f.inner.other);
  p = Pair(Job("l"), 1);
  n = n + move p.n;
  g = make();
  v: view Holder = g;
  n = n + keep(move g.job);
  n = n + v.n;
  h = make();
  k = fn() -> int { n2 = h.n; return n2 + keep(move h.job); };
  n = n + k();
  weak w: Holder? = h;
  n = n + keep(move w.job);
  return n + keep(move make().job);
})");
  EXPECT_EQ(r.ErrorCount, 14u) << r.Diagnostics;
  const std::string kNotOwned =
      "' is not an owned variable (a borrow, a capture or a 'weak' "
      "reference); write 'mem::replace(";
  const std::string kValue =
      "': it is a 'int', a value (a value is copied; a field of a reference "
      "type moves out)";
  for (const std::string &diag :
       {std::string(":15:34: error: cannot move 'self.job': 'move' takes a "
                    "variable, or a field of one it owns; write "
                    "'mem::replace(self.job, v)'"),
        ":24:50: error: cannot move 'h.job': 'h" + kNotOwned + "h.job, v)'",
        ":25:52: error: cannot move 'h.job': 'h" + kNotOwned + "h.job, v)'",
        std::string(":30:11: error: 'a' was moved at 29:16; it cannot be used "
                    "after the move"),
        std::string(":32:16: error: cannot move 'b.other': it is 'shared "
                    "Job', not a unique part of 'b'"),
        std::string(":34:16: error: cannot move 'c.fixed': it is 'const Job', "
                    "not a unique part of 'c'"),
        std::string(":36:17: error: cannot move 'd.back': it is a 'weak' "
                    "field, which owns nothing"),
        ":38:11: error: cannot move 'e.n" + kValue,
        std::string(":40:16: error: cannot move 'f.inner.other': it is "
                    "'shared Job', not a unique part of 'f'"),
        ":42:11: error: cannot move 'p.n" + kValue,
        std::string(":45:16: error: 'g' is viewed by 'view' local 'v' until "
                    "'v' is last used; it cannot be moved out of"),
        ":48:48: error: cannot move 'h.job': 'h" + kNotOwned + "h.job, v)'",
        ":51:16: error: cannot move 'w.job': 'w" + kNotOwned + "w.job, v)'",
        std::string(":52:19: error: cannot move 'make().job': 'move' takes a "
                    "variable, or a field of one it owns")})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  // The hint names a variable to replace in; a temporary gets none.
  EXPECT_EQ(r.Diagnostics.find("mem::replace(make().job"), std::string::npos)
      << r.Diagnostics;
}
