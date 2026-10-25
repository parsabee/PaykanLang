// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Borrowed box parameters (#98, docs/pir.md §3, §12): the programs that
// pin down when an object dies, run through the passes the driver runs (the
// moves at last use, the lending, the reference-count elimination).  A copy
// the caller makes of a reference it keeps for the call is lent instead;
// each program prints what it printed before borrowed parameters, so every
// object still dies where it did, and frees everything it allocates.  Each
// also says how many arguments the lending pass lent, and how many copies it
// kept because something between the read and the call may let the object
// go (the window).

#include "CodeGenTestUtils.h"

#include <gtest/gtest.h>

using namespace paykan;
using namespace paykan::test;

// A forwarding chain (probes/t1_forward): every argument handed over is
// handed over as before, so each node dies inside the callee that lets it
// go; the copies main keeps for itself are lent.
TEST(BorrowedParams, AParameterHandedOnDiesWhereItDid) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watcher {
  weak target: Node?;
}

fn report(w: view Watcher, tag: view Str) {
  match w.target {
    t: Node { io::println(tag + ": alive"); }
    None { io::println(tag + ": gone"); }
  }
}

// Reads its parameter only.
fn inner(n: Node) {
  io::println("inner reads " + n.name);
}

// Hands its parameter on at its last use, then does more work.
fn outer(n: Node, w: view Watcher) {
  inner(n);
  report(w, "in outer, after inner");
}

// Reassigns its parameter.
fn rebind(n: Node, w: view Watcher) {
  n = Node("other");
  report(w, "in rebind, after the assignment");
}

class Holder {
  n: Node?;
}

// Keeps its parameter in a field, then lets the field go.
fn stash(h: Holder, n: Node, w: view Watcher) {
  h.n = n;
  h.n = None;
  report(w, "in stash, after the field let go");
}

fn main() -> int {
  a = Node("a");
  wa = Watcher(a);
  outer(a, wa);              // a's last use: a transfer
  report(wa, "in main, after outer");

  b = Node("b");
  wb = Watcher(b);
  rebind(b, wb);
  report(wb, "in main, after rebind");

  c = Node("c");
  wc = Watcher(c);
  h = Holder(None);
  stash(h, c, wc);
  report(wc, "in main, after stash");

  // A temporary handed to outer: outer hands it on to inner.
  wt = Watcher(None);
  outer(Node("t"), wt);
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "inner reads a\n"
                      "in outer, after inner: gone\n"
                      "in main, after outer: gone\n"
                      "in rebind, after the assignment: gone\n"
                      "in main, after rebind: gone\n"
                      "in stash, after the field let go: gone\n"
                      "in main, after stash: gone\n"
                      "inner reads t\n"
                      "in outer, after inner: gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 9u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("t1_forward");
}

// `nameAndNode(x.name, x, wx)` at x's last use (probes/t2_hazards): the
// callee may let x go, so x.name keeps its copy (the window); h.s, which
// clearThenRead replaces, is held as before.
TEST(BorrowedParams, AFieldReadBesideItsRootsLastUseKeepsItsCopy) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Holder {
  n: Node?;
  s: Str;
}

class Watcher {
  weak target: Node?;
}

fn report(w: view Watcher, tag: view Str) {
  match w.target {
    t: Node { io::println(tag + ": alive"); }
    None { io::println(tag + ": gone"); }
  }
}

// The callee replaces the field its argument was read from, then reads
// the argument: the caller must keep the argument alive (a hold).
fn clearThenRead(s: Str, h: Holder) {
  h.s = "replaced";
  io::println("still have " + s);
}

// Given a node's name and the node at its last use: the node may die
// inside, the name must not.
fn nameAndNode(s: Str, n: Node, w: view Watcher) {
  n = Node("other");
  report(w, "in nameAndNode, after the assignment");
  io::println("name " + s);
}

fn main() -> int {
  h = Holder(None, "original " + "string");
  clearThenRead(h.s, h);

  x = Node("x" + "name");
  wx = Watcher(x);
  nameAndNode(x.name, x, wx);   // x's last use
  report(wx, "in main, after nameAndNode");
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "still have original string\n"
                      "in nameAndNode, after the assignment: gone\n"
                      "name xname\n"
                      "in main, after nameAndNode: gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 3u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 1u);
  g.expectNoLeaks("t2_hazards");
}

// `f(x.name, c && g(x))` (crit/c1_andand_window): x moves into g inside the
// `if` the `&&` lowers to, while x.name is in use; the window takes in the
// nested region, and the copy stays.
TEST(BorrowedParams, AMoveInsideAnAndKeepsTheCopyOfAFieldRead) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn g(n: Node) -> bool { return True; }

fn f(s: Str, b: bool, w: view Watch) {
  io::println("in f, x is " + state(w) + "; s = " + s);
}

fn main() -> int {
  x = Node("x" + "name");
  wx = Watch(x);
  c = wx.target != None;
  f(x.name, c && g(x), wx);
  io::println("after f: x is " + state(wx));
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "in f, x is gone; s = xname\n"
                      "after f: x is gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 3u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 1u);
  g.expectNoLeaks("c1_andand_window");
}

// `f(x, c && g(x))` (crit/c1b_andand_var): the first argument is a copy of x
// itself, and g drops x inside the `&&`: f must still have a reference of
// its own.
TEST(BorrowedParams, AMoveInsideAnAndKeepsTheCopyOfTheVariable) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn g(n: Node) -> bool { return True; }

fn f(n: Node, b: bool, w: view Watch) {
  io::println("in f, x is " + state(w) + "; n.name = " + n.name);
}

fn main() -> int {
  x = Node("x" + "name");
  wx = Watch(x);
  c = wx.target != None;
  f(x, c && g(x), wx);
  io::println("after f: " + state(wx));
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "in f, x is alive; n.name = xname\n"
                      "after f: gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 3u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 1u);
  g.expectNoLeaks("c1b_andand_var");
}

// `f(x.name, g(x))` (crit/c3_alias_sibling): x is moved into g, which drops it
// before f is called; f gets a copy of x.name, and x dies in g as before.
TEST(BorrowedParams, AMoveIntoASiblingArgumentKeepsTheCopy) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn g(n: Node) -> int { return 1; }

fn f(s: Str, k: int) -> int { return s.len() + k; }

fn main() -> int {
  x = Node("x" + "name");
  wx = Watch(x);
  r = f(x.name, g(x));
  io::println("after f: x is " + state(wx) + " (" + Str(r) + ")");
  io::println("end of main");
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "after f: x is gone (6)\n"
                      "end of main\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 1u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 1u);
  g.expectNoLeaks("c3_alias_sibling");
}

// `f(x.name, x, wx)` with x an `inout` argument (plan/q1_inout_root): f
// replaces x, then reads s; x.name is no read to lend.
TEST(BorrowedParams, AReadOfAnInoutArgumentsVariableIsOwned) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn f(s: Str, n: inout Node, w: view Watch) {
  n = Node("other");
  io::println("in f, old x is " + state(w) + "; s = " + s);
}

fn main() -> int {
  x = Node("x" + "name");
  wx = Watch(x);
  f(x.name, x, wx);
  io::println("after f: " + x.name);
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "in f, old x is gone; s = xname\n"
                      "after f: other\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 2u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("q1_inout_root");
}

// `st.set(st.n.name, st.n)` on a struct method that changes self
// (plan/q2_struct_mut): the method replaces the node, then reads its
// arguments.
TEST(BorrowedParams, AReadOfAMutatedStructIsOwned) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

struct S {
  n: Node;
  w: Watch;
}

impl S {
  fn set(s: Str, v: Node) {
    self.n = Node("z");
    io::println("in set, old node is " + state(self.w) + "; s = " + s + ", v = " + v.name);
  }
}

fn main() -> int {
  st = S(Node("n" + "ame"), Watch(None));
  st.w.target = st.n;
  st.set(st.n.name, st.n);
  io::println("after: " + st.n.name);
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "in set, old node is alive; s = name, v = name\n"
                      "after: z\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 1u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("q2_struct_mut");
}

// `sink(h.n.name, h, w)` inside cur (plan/q3_lent_param_chain): cur hands h
// on at its last use, and sink replaces h.n; h.n.name keeps its copy.
TEST(BorrowedParams, AParameterHandedOnBesideItsFieldKeepsTheField) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Holder {
  n: Node;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn sink(s: Str, h: Holder, w: view Watch) {
  h.n = Node("replaced");
  io::println("in sink, old node is " + state(w) + "; s = " + s);
}

fn cur(h: Holder, w: view Watch) {
  sink(h.n.name, h, w);
}

fn main() -> int {
  h = Holder(Node("n" + "ame"));
  w = Watch(h.n);
  cur(h, w);
  io::println("after cur: old node is " + state(w));
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "in sink, old node is gone; s = name\n"
                      "after cur: old node is gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 4u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("q3_lent_param_chain");
}

// Each call kind given `h.n` and `h`, where the callee replaces h.n and then
// uses its argument (plan/k1_kinds): a direct call, a vcall, a constructor,
// a struct method, a function value, a generic function and `__super__`.
// The read stays held; the variables are lent.
TEST(BorrowedParams, EveryCallKindKeepsWhatItsCalleeReplaces) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Holder {
  n: Node;
}

fn direct(m: Node, h: Holder) {
  h.n = Node("z");
  io::println("direct: " + m.name);
}

class Base {
  view fn take(m: Node, h: Holder) {
    io::println("base");
  }
}

class Derived: Base {
  view fn take(m: Node, h: Holder) {
    h.n = Node("z");
    io::println("vcall: " + m.name);
  }
}

class Keeper {
  m: Node;
}

impl Keeper {
  fn __init__(m: Node, h: Holder) {
    h.n = Node("z");
    self.m = m;
    io::println("ctor: " + m.name);
  }
}

class Parent {
  m: Node;
}

impl Parent {
  fn __init__(m: Node, h: Holder) {
    h.n = Node("z");
    self.m = m;
    io::println("super: " + m.name);
  }
}

class Child: Parent {
}

impl Child {
  fn __init__(h: Holder) {
    __super__(h.n, h);
  }
}

struct S {
  k: int;
}

impl S {
  view fn meth(m: Node, h: Holder) {
    h.n = Node("z");
    io::println("struct method: " + m.name);
  }
}

fn gen<T>(m: T, h: Holder) -> T {
  h.n = Node("z");
  return m;
}

fn main() -> int {
  h = Holder(Node("a" + "1"));
  direct(h.n, h);
  h.n = Node("a" + "2");
  b: Base = Derived();
  b.take(h.n, h);
  h.n = Node("a" + "3");
  k = Keeper(h.n, h);
  h.n = Node("a" + "4");
  s = S(1);
  s.meth(h.n, h);
  h.n = Node("a" + "5");
  f = fn(m: Node) -> Str { h.n = Node("z"); return m.name; };
  io::println("icall: " + f(h.n));
  h.n = Node("a" + "6");
  io::println("generic: " + gen(h.n, h).name);
  h.n = Node("a" + "7");
  c = Child(h);
  io::println("kept: " + k.m.name + " " + c.m.name);
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "direct: a1\n"
                      "vcall: a2\n"
                      "ctor: a3\n"
                      "struct method: a4\n"
                      "icall: a5\n"
                      "generic: a6\n"
                      "super: a7\n"
                      "kept: a3 a7\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 5u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("k1_kinds");
}

// Every way a callee can write its box parameter (plan/w1_written), each
// called with a copy, which is lent (the caller's node stays alive), and at
// the argument's last use (a move: the old node dies inside, as before).
TEST(BorrowedParams, EveryWayACalleeWritesItsParameter) {
  LeakGuard g;
  pir::Statistics stats;
  auto r = compilePassesAndRun(R"pkn(import ::io;

class Node {
  name: Str;
}

class Watch {
  weak target: Node?;
}

fn state(w: view Watch) -> Str {
  match w.target {
    Node { return "alive"; }
    None { return "gone"; }
  }
}

fn assign(n: Node, w: view Watch) {
  n = Node("b");
  io::println("  assign: old is " + state(w) + ", n = " + n.name);
}

fn compound(s: Str) {
  s += "!";
  io::println("  compound: s = " + s);
}

fn bump(n: inout Node) {
  n = Node("c");
}

fn viaInout(n: Node, w: view Watch) {
  bump(n);
  io::println("  viaInout: old is " + state(w) + ", n = " + n.name);
}

fn pair(a: Node) -> (Node, int) { return (a, 1); }

fn destructure(n: Node, w: view Watch) {
  n, k = pair(Node("m"));
  io::println("  destructure: old is " + state(w) + ", n = " + n.name);
}

class Sub: Node {
  extra: int;
}

fn armInout(n: Node, w: view Watch) {
  match n {
    s: inout Node { s = Node("arm"); }
  }
  io::println("  armInout: old is " + state(w) + ", n = " + n.name);
}

fn inoutLocal(n: Node, w: view Watch) {
  r: inout = n;
  r = Node("loc");
  io::println("  inoutLocal: old is " + state(w) + ", n = " + n.name);
}

fn toNone(n: Node?, w: view Watch) {
  n = None;
  io::println("  toNone: old is " + state(w));
}

fn main() -> int {
  a = Node("a");
  wa = Watch(a);
  io::println("copies:");
  assign(a, wa);
  compound(a.name);
  viaInout(a, wa);
  destructure(a, wa);
  toNone(a, wa);
  armInout(a, wa);
  inoutLocal(a, wa);
  io::println("after copies: a is " + state(wa) + ", a = " + a.name);
  io::println("moves:");
  b = Node("b0");
  wb = Watch(b);
  assign(b, wb);
  c = Node("c0");
  wc = Watch(c);
  viaInout(c, wc);
  d = Node("d0");
  wd = Watch(d);
  toNone(d, wd);
  e = Node("e0");
  we = Watch(e);
  destructure(e, we);
  f = Node("f0");
  wf = Watch(f);
  armInout(f, wf);
  g = Node("g0");
  wg = Watch(g);
  inoutLocal(g, wg);
  io::println("after moves: " + state(wb) + " " + state(wc) + " " + state(wd) + " " + state(we) + " " + state(wf) + " " + state(wg));
  return 0;
}
)pkn",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "copies:\n"
                      "  assign: old is alive, n = b\n"
                      "  compound: s = a!\n"
                      "  viaInout: old is alive, n = c\n"
                      "  destructure: old is alive, n = m\n"
                      "  toNone: old is alive\n"
                      "  armInout: old is alive, n = arm\n"
                      "  inoutLocal: old is alive, n = loc\n"
                      "after copies: a is alive, a = a\n"
                      "moves:\n"
                      "  assign: old is gone, n = b\n"
                      "  viaInout: old is gone, n = c\n"
                      "  toNone: old is gone\n"
                      "  destructure: old is gone, n = m\n"
                      "  armInout: old is gone, n = arm\n"
                      "  inoutLocal: old is gone, n = loc\n"
                      "after moves: gone gone gone gone gone gone\n");
  EXPECT_EQ(stats.get("lending", "arguments lent"), 32u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 0u);
  g.expectNoLeaks("w1_written");
}
