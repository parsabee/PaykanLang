// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen / E2E tests for tuples.  Every runtime test runs under
// the tracking allocator and asserts zero live heap blocks afterwards, so each
// case checks both the observable output and that the tuple, its box and its
// reference elements are released exactly once.

#include "CodeGenTestUtils.h"
#include "Names.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// The slot-kind codes CodeGen emits must be the ones the runtime interprets.
static_assert(int(paykan::names::kTupleSlotInt) == int(PAYKAN_TUPLE_INT),
              "tuple slot-kind ABI drift");
static_assert(int(paykan::names::kTupleSlotFloat) == int(PAYKAN_TUPLE_FLOAT),
              "tuple slot-kind ABI drift");
static_assert(int(paykan::names::kTupleSlotBool) == int(PAYKAN_TUPLE_BOOL),
              "tuple slot-kind ABI drift");
static_assert(int(paykan::names::kTupleSlotChar) == int(PAYKAN_TUPLE_CHAR),
              "tuple slot-kind ABI drift");
static_assert(int(paykan::names::kTupleSlotRef) == int(PAYKAN_TUPLE_REF),
              "tuple slot-kind ABI drift");

// -- Literals, element reads, printing

TEST(Tuple, PrimitiveElementsRoundTrip) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      t = (1, 2.5, True, 'c');
      println(Str<int>(t.0));
      println(Str<float>(t.1));
      println(Str<bool>(t.2));
      println(Str<char>(t.3));
      println(t);
      return t.0 + 41;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n2.5\nTrue\nc\n(1, 2.5, True, c)\n");
  EXPECT_EQ(r.ExitCode, 42);
  g.expectNoLeaks("PrimitiveElementsRoundTrip");
}

TEST(Tuple, StrElements) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      t = (1, "a");
      s: Str = t.1;        // new owner retains the slot's box
      x = t.1;             // implicit declaration, same rule
      println(s + x + t.1);
      println(t);
      u: (Str, Str) = ("p" + "q", s);   // concat temp and owned var as elements
      println(u.0 + u.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "aaa\n(1, a)\npqa\n");
  g.expectNoLeaks("StrElements");
}

TEST(Tuple, NestedTuples) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      t = ((1, "x"), (2.5, ("deep", True)));
      println(t);
      inner = t.1;
      println(inner.1.0);
      println(t.0.1);
      println(Str<int>(t.0.0 + 1));
      println(Str<bool>(t.1.1.1));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "((1, x), (2.5, (deep, True)))\ndeep\nx\n2\nTrue\n");
  g.expectNoLeaks("NestedTuples");
}

TEST(Tuple, TupleInArrayAndArrayInTuple) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      pairs: (int, Str)[] = [(1, "one"), (2, "two")];
      pairs.push((3, "three"));
      println(pairs[2].1);
      println(pairs[0]);
      println(Str<int>(pairs.len()));
      p = pairs[1];            // element acquired by a new owner
      println(p.1);
      pairs[0] = (10, "ten");  // set_obj releases the old tuple
      println(pairs[0]);

      arrs: (int[], Str) = ([1, 2, 3], "nums");
      arrs.0.push(4);
      println(Str<int>(arrs.0.len()));
      println(Str<int>(arrs.0[3]));
      println(arrs.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "three\n(1, one)\n3\ntwo\n(10, ten)\n4\n4\nnums\n");
  g.expectNoLeaks("TupleInArrayAndArrayInTuple");
}

// -- Multiple return and destructuring

TEST(Tuple, MultipleReturnAndDestructuring) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn divmod(a: int, b: int) -> (int, int) {
      return (a / b, a % b);
    }
    fn named() -> (Str, int) {
      return ("seven", 7);
    }
    fn main() -> int {
      q, r = divmod(7, 2);
      println(Str<int>(q) + " " + Str<int>(r));
      name, n = named();
      println(name);
      _, only = named();
      println(Str<int>(only));
      first: Str, _ = named();
      println(first);
      q, r = divmod(9, 4);         // re-assign existing q, r
      println(Str<int>(q) + " " + Str<int>(r));
      f: float, _ = (1, 2);        // int -> float into an annotated target
      println(Str<float>(f));
      return q + r + n;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 1\nseven\n7\nseven\n2 1\n1\n");
  EXPECT_EQ(r.ExitCode, 10);
  g.expectNoLeaks("MultipleReturnAndDestructuring");
}

TEST(Tuple, DestructureReassignsRefVariables) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "old";
      arr: int[] = [1];
      s, arr = ("new", [4, 5, 6]);   // both old boxes released
      println(s);
      println(Str<int>(arr.len()));
      o: Obj = None;
      o, s = ("boxed", "again");      // None-initialised var promoted to owned
      println(o);
      println(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "new\n3\nboxed\nagain\n");
  g.expectNoLeaks("DestructureReassignsRefVariables");
}

TEST(Tuple, DestructureFromVariableAndLiteralKeepsSource) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      t = ("a", "b");
      x, y = t;                // t stays alive and usable
      println(x + y + t.0);
      a, b = ("lit", "eral");  // temporary tuple released after extraction
      println(a + b);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "aba\nliteral\n");
  g.expectNoLeaks("DestructureFromVariableAndLiteralKeepsSource");
}

// -- Ownership: temporaries, copies, parameters

TEST(Tuple, CallRootedIndexTearsDownTemporary) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn mk() -> (Str, int) { return ("tmp", 3); }
    fn main() -> int {
      println(mk().0);              // ref element retained, tuple released
      println(Str<int>(mk().1));      // primitive element copied out
      n: int = mk().0.len();        // element used as a receiver
      println(Str<int>(n));
      s = mk().0;                   // element bound to a new owner
      println(s);
      println(("x", "y").1);        // literal receiver
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "tmp\n3\n3\ntmp\ny\n");
  g.expectNoLeaks("CallRootedIndexTearsDownTemporary");
}

TEST(Tuple, TupleCopiedAndPassed) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn take(p: (Str, int)) -> Str { return p.0; }
    fn main() -> int {
      t = ("moved", 1);
      u = t;
      println(u.0);
      println(take(u));       // moved into a parameter, released there
      v = ("again", 2);
      w: (Str, int) = v;
      v = ("revived", 3);
      println(w.0 + v.0);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "moved\nmoved\nagainrevived\n");
  g.expectNoLeaks("TupleCopiedAndPassed");
}

TEST(Tuple, PassedToFunctionsAndReturnedThrough) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn swap(p: (int, Str)) -> (Str, int) { return (p.1, p.0); }
    fn same(p: (int, Str)) -> (int, Str) { return p; }
    fn main() -> int {
      t = (1, "one");
      s, n = swap(t);
      println(s + Str<int>(n));
      u = same(t);
      println(u);
      println(same((2, "two")));
      x, y = swap(same(t));
      println(x);
      return n;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "one1\n(1, one)\n(2, two)\none\n");
  EXPECT_EQ(r.ExitCode, 1);
  g.expectNoLeaks("PassedToFunctionsAndReturnedThrough");
}

// -- Tuples in classes

TEST(Tuple, TupleFieldInClass) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Holder {
      pair: (int, Str);
      fn __init__(p: (int, Str)) { self.pair = p; }
      fn get() -> (int, Str) { return self.pair; }
      fn first() -> int { return self.pair.0; }
      fn second() -> Str { return self.pair.1; }
      fn reset() { self.pair = (0, "zero"); }
    }
    fn main() -> int {
      h: Holder = Holder((9, "nine"));
      a, b = h.get();
      println(Str<int>(a) + b);
      println(h.pair);
      println(h.pair.1 + Str<int>(h.first()));
      println(h.second());
      h.reset();              // old tuple released by the field store
      println(h.pair);
      h.pair = (5, "five");   // field assignment from a literal
      println(h.get().1);     // call-rooted element read
      return h.first();
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9nine\n(9, nine)\nnine9\nnine\n(0, zero)\nfive\n");
  EXPECT_EQ(r.ExitCode, 5);
  g.expectNoLeaks("TupleFieldInClass");
}

TEST(Tuple, ClassInstanceElements) {
  LeakGuard g;
  // Delimited raw string: the Paykan source contains `")"`.
  auto r = compileAndRun(R"pkn(
    class Point {
      x: int;
      y: int;
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
      fn toString() -> Str {
        return "P(" + Str<int>(self.x) + "," + Str<int>(self.y) + ")";
      }
    }
    fn main() -> int {
      t = ("origin", Point(0, 0));
      println(t);
      println(t.1.toString());
      println(Str<int>(t.1.x));
      p: Point = t.1;
      p.x = 7;                  // shared object: visible through the tuple
      println(Str<int>(t.1.x));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(origin, P(0,0))\nP(0,0)\n0\n7\n");
  g.expectNoLeaks("ClassInstanceElements");
}

// -- Equality, toString, Obj-typed use

TEST(Tuple, EqualityIsElementWise) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Thing { fn __init__() {} }
    fn main() -> int {
      a = (1, "s");
      b = (1, "s");
      c = (2, "s");
      println(Str<bool>(a == b));         // content-equal Str elements
      println(Str<bool>(a != b));
      println(Str<bool>(a == c));
      println(Str<bool>((1, 2) == (1, 2)));   // two temporaries
      println(Str<bool>(a.equals(b)));
      n = ((1, "x"), 2.5);
      m = ((1, "x"), 2.5);
      println(Str<bool>(n == m));          // nested tuples compare deeply
      t = Thing();
      p = (t, 1);
      q = (t, 1);
      w = (Thing(), 1);
      println(Str<bool>(p == q));          // same object: identity equals
      println(Str<bool>(p == w));          // distinct objects
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nFalse\nTrue\nTrue\nTrue\nTrue\nFalse\n");
  g.expectNoLeaks("EqualityIsElementWise");
}

TEST(Tuple, ToStringAndObjTyped) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show(o: Obj) { println(o); }
    fn main() -> int {
      t = (1, "a");
      s: Str = t.toString();
      println(s + "!");
      o: Obj = t;
      show(o);
      show((2, None));
      println(("q", 'z').toString());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(1, a)!\n(1, a)\n(2, None)\n(q, z)\n");
  g.expectNoLeaks("ToStringAndObjTyped");
}

TEST(Tuple, EnumElementsAndScopes) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    enum Color { Red, Green }
    fn main() -> int {
      i: int = 0;
      while (i < 3) {
        t = (Color::Green, "g" + Str<int>(i));   // fresh tuple each iteration
        if (t.0 == Color::Green) { println(t.1); }
        i = i + 1;
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "g0\ng1\ng2\n");
  g.expectNoLeaks("EnumElementsAndScopes");
}

// #71: literals take their destination's element types.  A `None` tuple
// element is the null box of its `T?` slot (so `match` sees None, not a boxed
// `None` object), and array literals of tuples mixing `int` and `int?` take
// the declared element type, in every typed sink.
TEST(Tuple, LiteralsTakeTheDestinationsOptionalElementTypes) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { v: int; fn __init__(v: int) { self.v = v; } }
    class Holder {
      t: (Str, int?)[];
      fn __init__() { self.t = []; }
      fn put(xs: (Str, int?)[]) { self.t = xs; }
    }
    fn which(n: Node?) -> Str { match n { x: Node { return "node"; } None { return "none"; } } }
    fn val(n: int?) -> Str { match n { x: int { return Str<int>(x); } None { return "none"; } } }
    fn count(xs: (Str, int?)[]) -> int { return xs.len(); }
    fn mk() -> (Node?, int)[] { return [(None, 1), (Node(2), 2)]; }
    fn main() -> int {
      b: (Node?, int) = (None, 1);
      a: (int?, Str) = (None, "a");
      println(which(b.0) + " " + val(a.0));
      ps = mk();
      println(which(ps[0].0) + " " + which(ps[1].0));
      nb: int? = None;
      tbl: (Str, int?)[] = [("a", 1), ("b", nb), ("c", None)];
      println(val(tbl[0].1) + " " + val(tbl[1].1) + " " + val(tbl[2].1));
      h = Holder();
      h.put([("m", 4), ("n", nb)]);
      h.t[1] = ("o", 5);
      h.t.push(("p", None));
      println(val(h.t[0].1) + " " + val(h.t[1].1) + " " + val(h.t[2].1));
      nodes: Node?[] = [None, Node(1)];
      println(which(nodes[0]) + " " + which(nodes[1]));
      return count([("x", 1), ("y", nb), ("z", None)]);
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 3);
  EXPECT_EQ(r.StdOut, "none none\nnone node\n1 none none\n4 5 none\n"
                      "none node\n");
  g.expectNoLeaks("LiteralsTakeTheDestinationsOptionalElementTypes");
}

// -- NaN and self-comparison (#111)

// Tuple equality is element-wise even when both sides are the same object, so
// a tuple holding a NaN (directly, nested, or in a `float?` element) is
// unequal to itself, like the NaN.  `float?` behaves the same; `float[]` is
// reference identity by definition, so it equals itself.  Without a NaN a
// tuple still equals itself.
TEST(Tuple, NaNElementMakesATupleUnequalToItself) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      nan: float = 0.0 / 0.0;
      t = (1, nan);
      u = (1, nan);
      alias = t;
      println(Str(t == t) + " " + Str(t != t) + " " + Str(t.equals(t)));
      println(Str(t == u) + " " + Str(alias == t));
      n = ((1, nan), "x");
      o: (int, float?) = (1, nan);
      println(Str(n == n) + " " + Str(o == o));
      x: float? = nan;
      println(Str(x == x) + " " + Str(x != x));
      a: float[] = [nan];
      println(Str(a == a));
      ok = (1, 2.5, "s", (2, 'c'));
      y: float? = 2.5;
      println(Str(ok == ok) + " " + Str(ok != ok) + " " + Str(y == y));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False True False\nFalse False\nFalse False\n"
                      "False True\nTrue\nTrue False True\n");
  g.expectNoLeaks("NaNElementMakesATupleUnequalToItself");
}
