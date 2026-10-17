// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen / E2E tests for tuples, values held in place: copying one retains
// the references it holds, and a variable's, an element's or a field's are
// released when it goes.  Every runtime test runs under the tracking
// allocator and asserts zero live heap blocks afterwards, so each case checks
// both the observable output and that every reference a tuple holds is
// released exactly once.

#include "CodeGenTestUtils.h"
#include "Names.h"
#include "paykan/pir/PIR.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// An inline optional in memory (an `int?[n]` element, a field, a native
// function's PaykanType) is the C backend's `{ bool present; T value; }`: the
// size and alignment the lowering gives the runtime are that struct's.
namespace {
template <typename T> struct OptLayout {
  bool present;
  T value;
};
template <typename T> constexpr bool sameLayout(paykan::pir::Type t) {
  return paykan::pir::storeSize(t) == int64_t(sizeof(OptLayout<T>)) &&
         paykan::pir::storeAlign(t) == int64_t(alignof(OptLayout<T>));
}
} // namespace
static_assert(sameLayout<int64_t>(paykan::pir::Type::OptI64),
              "optional layout ABI drift");
static_assert(sameLayout<double>(paykan::pir::Type::OptF64),
              "optional layout ABI drift");
static_assert(sameLayout<bool>(paykan::pir::Type::OptBool),
              "optional layout ABI drift");
static_assert(sameLayout<int8_t>(paykan::pir::Type::OptChar),
              "optional layout ABI drift");

// Likewise the type kinds a generic native function is passed.
static_assert(int(paykan::names::kNativeKindInt) == int(PAYKAN_KIND_INT),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindFloat) == int(PAYKAN_KIND_FLOAT),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindBool) == int(PAYKAN_KIND_BOOL),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindChar) == int(PAYKAN_KIND_CHAR),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindObject) == int(PAYKAN_KIND_OBJECT),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindOptional) ==
                  int(PAYKAN_KIND_OPTIONAL),
              "native type-kind ABI drift");
static_assert(int(paykan::names::kNativeKindTuple) == int(PAYKAN_KIND_TUPLE),
              "native type-kind ABI drift");

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
      s: Str = t.1;        // a new owner retains the element's box
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

TEST(Tuple, TupleInArray) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      pairs: (int, Str)[3] = [(1, "one"), (2, "two"), (3, "three")];
      println(pairs[2].1);
      println(pairs[0]);
      println(Str<int>(pairs.len()));
      p = pairs[1];            // element copied to a new owner
      println(p.1);
      pairs[0] = (10, "ten");  // releases the old element's references
      println(pairs[0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "three\n(1, one)\n3\ntwo\n(10, ten)\n");
  g.expectNoLeaks("TupleInArray");
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
      w: Str = "w" + "x";
      s, w = ("new", "abc");          // both old boxes released
      println(s);
      println(Str<int>(w.len()));
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
      a, b = ("lit", "eral");  // temporary released after extraction
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
      println(mk().0);              // ref element retained, temporary released
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
      println(take(u));       // the callee's own copy, released there
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
      fn get() -> (int, Str) { return self.pair; }
      fn first() -> int { return self.pair.0; }
      fn second() -> Str { return self.pair.1; }
      fn reset() { self.pair = (0, "zero"); }
    }

    impl Holder {
      fn __init__(p: (int, Str)) { self.pair = p; }
    }
    fn main() -> int {
      h: Holder = Holder((9, "nine"));
      a, b = h.get();
      println(Str<int>(a) + b);
      println(h.pair);
      println(h.pair.1 + Str<int>(h.first()));
      println(h.second());
      h.reset();              // old references released by the store
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
      view fn toString() -> Str {
        return "P(" + Str<int>(self.x) + "," + Str<int>(self.y) + ")";
      }
    }

    impl Point {
      fn __init__(x: int, y: int) { self.x = x; self.y = y; }
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

// -- Equality and toString

TEST(Tuple, EqualityIsElementWise) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Thing {}
    impl Thing { fn __init__() {} }
    fn main() -> int {
      a = (1, "s");
      b = (1, "s");
      c = (2, "s");
      println(Str<bool>(a == b));         // content-equal Str elements
      println(Str<bool>(a != b));
      println(Str<bool>(a == c));
      println(Str<bool>((1, 2) == (1, 2)));   // two temporaries
      n = ((1, "x"), 2.5);
      m = ((1, "x"), 2.5);
      println(Str<bool>(n == m));          // nested tuples compare deeply
      t = Thing();
      p = (t, 1);
      q = (t, 1);
      w = (Thing(), 1);
      println(Str<bool>(p == q));          // the same Thing: identity equals
      println(Str<bool>(p == w));          // distinct Things
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nFalse\nFalse\nTrue\nTrue\nTrue\nFalse\n");
  g.expectNoLeaks("EqualityIsElementWise");
}

TEST(Tuple, ToStringAndPrint) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show(t: (int, Str?)) { println(t); }
    fn main() -> int {
      t = (1, "a");
      s: Str = t.toString();
      println(s + "!");
      print(t);
      println(" " + t.toString() + " " + t.toString());
      show((2, None));
      show(t);
      println(("q", 'z').toString());
      printerrln((3, "e"));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "(1, a)!\n(1, a) (1, a) (1, a)\n(2, None)\n(1, a)\n(q, z)\n");
  EXPECT_NE(r.StdErr.find("(3, e)\n"), std::string::npos) << r.StdErr;
  g.expectNoLeaks("ToStringAndPrint");
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
// element is the null box of its `T?` element (so `match` sees None, not a
// boxed `None` object), and array literals of tuples mixing `int` and `int?`
// take the declared element type, in every typed sink.
TEST(Tuple, LiteralsTakeTheDestinationsOptionalElementTypes) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { v: int; }
    impl Node { fn __init__(v: int) { self.v = v; } }
    class Holder {
      t: (Str, int?)[3];
      fn put(xs: (Str, int?)[3]) { self.t = xs; }
    }

    impl Holder {
      fn __init__() { self.t = [("", None), ("", None), ("", None)]; }
    }
    fn which(n: Node?) -> Str { match n { x: Node { return "node"; } None { return "none"; } } }
    fn val(n: int?) -> Str { match n { x: int { return Str<int>(x); } None { return "none"; } } }
    fn count(xs: view (Str, int?)[]) -> int { return xs.len(); }
    fn mk() -> (Node?, int)[2] { return [(None, 1), (Node(2), 2)]; }
    fn main() -> int {
      b: (Node?, int) = (None, 1);
      a: (int?, Str) = (None, "a");
      println(which(b.0) + " " + val(a.0));
      ps = mk();
      println(which(ps[0].0) + " " + which(ps[1].0));
      nb: int? = None;
      tbl: (Str, int?)[3] = [("a", 1), ("b", nb), ("c", None)];
      println(val(tbl[0].1) + " " + val(tbl[1].1) + " " + val(tbl[2].1));
      h = Holder();
      h.put([("m", 4), ("n", nb), ("q", 6)]);
      h.t[1] = ("o", 5);
      h.t[2] = ("p", None);
      println(val(h.t[0].1) + " " + val(h.t[1].1) + " " + val(h.t[2].1));
      nodes: Node?[2] = [None, Node(1)];
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

// Tuple equality is element-wise even when both sides are the same variable,
// so a tuple holding a NaN (directly, nested, or in a `float?` element) is
// unequal to itself, like the NaN.  `float?` behaves the same.  Without a NaN
// a tuple still equals itself.
TEST(Tuple, NaNElementMakesATupleUnequalToItself) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      nan: float = 0.0 / 0.0;
      t = (1, nan);
      u = (1, nan);
      alias = t;
      println(Str(t == t) + " " + Str(t != t));
      println(Str(t == u) + " " + Str(alias == t));
      n = ((1, nan), "x");
      o: (int, float?) = (1, nan);
      println(Str(n == n) + " " + Str(o == o));
      x: float? = nan;
      println(Str(x == x) + " " + Str(x != x));
      ok = (1, 2.5, "s", (2, 'c'));
      y: float? = 2.5;
      println(Str(ok == ok) + " " + Str(ok != ok) + " " + Str(y == y));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "False True\nFalse False\nFalse False\n"
                      "False True\nTrue False True\n");
  g.expectNoLeaks("NaNElementMakesATupleUnequalToItself");
}

// -- Value semantics: tuples held in place

namespace {
// A class element that shows which object it is.
const char *const kNode = R"(
  class Node {
    v: int;
    view fn toString() -> Str { return "N" + Str<int>(self.v); }
  }
  impl Node { fn __init__(v: int) { self.v = v; } }
)";
} // namespace

// An array of tuples holds each one's elements in place: a read copies one
// out (retaining its references), a store releases the old element's.
TEST(Tuple, ArrayOfTuplesWithReferenceElements) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"(
    fn main() -> int {
      xs: (Str, Node)[3] = [("a", Node(1)), ("b", Node(2)), ("c", Node(3))];
      println(xs[1]);
      println(xs[2].0 + Str<int>(xs[2].1.v));
      t = xs[1];                     // a copy: survives the store below
      xs[1] = ("y", xs[0].1);        // the element read before the store
      xs[0] = ("z" + "z", Node(26));
      println(t);
      println(xs[0]);
      println(xs[1]);
      n: Node = xs[1].1;
      n.v = 7;                       // the Node is shared, not copied
      println(xs[1]);
      i: int = 0;
      s = "";
      while (i < 3) { s = s + xs[i].0; i = i + 1; }
      println(s);
      return xs[2].1.v;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "(b, N2)\nc3\n(b, N2)\n(zz, N26)\n(y, N1)\n(y, N7)\nzzyc\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("ArrayOfTuplesWithReferenceElements");
}

// `[v; n]` copies the tuple into every element, retaining its Str each time.
TEST(Tuple, RepeatedTupleArrayLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      xs: (int, Str)[3] = [(0, "k"); 3];
      xs[1] = (1, xs[1].1 + "!");
      println(xs[0]);
      println(xs[1]);
      println(xs[2]);
      ys = [(0, "k" + "v"); 4];
      println(Str<int>(ys.len()) + ys[3].1 + ys[0].1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(0, k)\n(1, k!)\n(0, k)\n4kvkv\n");
  g.expectNoLeaks("RepeatedTupleArrayLiteral");
}

// A tuple field is assigned, read and copied out like any value; its
// references go when the object dies (here at scope end, from a function's
// result, and from an array of objects).
TEST(Tuple, TupleFieldIsReleasedWithItsObject) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"(
    class Holder {
      t: (Str, Node?);
      n: int;
    }
    impl Holder { fn __init__(s: Str) { self.t = (s, Node(1)); self.n = 0; } }
    fn make(s: Str) -> Holder {
      h = Holder(s + "!");
      h.t = (h.t.0 + "?", None);
      return h;
    }
    fn main() -> int {
      h = Holder("a");
      println(h.t);
      h.t = ("b", Node(2));
      println(h.t);
      c = h.t;
      h.t = ("c", None);
      println(c);
      println(h.t);
      m = make("m");
      println(m.t);
      hs: Holder[2] = [Holder("p"), Holder("q")];
      println(hs[1].t.0);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(a, N1)\n(b, N2)\n(b, N2)\n(c, None)\n(m!?, None)\nq\n");
  g.expectNoLeaks("TupleFieldIsReleasedWithItsObject");
}

TEST(Tuple, NestedTuplesHoldingReferences) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"(
    fn main() -> int {
      t = (("in", Node(1)), (Node(2), "out", 3));
      u = t;
      inner = t.1;
      println(t);
      println(inner);
      println(Str<bool>(t == u));    // the same Nodes
      w = (("in", Node(1)), (Node(2), "out", 3));
      println(Str<bool>(t == w));    // other Nodes: identity equals
      n: Node = t.0.1;
      n.v = 5;
      println(u.0);
      return inner.2;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "((in, N1), (N2, out, 3))\n(N2, out, 3)\nTrue\nFalse\n(in, N5)\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("NestedTuplesHoldingReferences");
}

TEST(Tuple, TernaryYieldsATuple) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn pick(c: bool, a: (Str, int), b: (Str, int)) -> (Str, int) {
      return if c then a else b;
    }
    fn main() -> int {
      a = ("a" + "1", 1);
      b = ("b" + "2", 2);
      c = if a.1 > 1 then a else b;
      println(c);
      d: (Str, int) = if a.1 < 2 then a else ("lit", 0);
      println(d);
      println(pick(True, a, b));
      println(pick(False, a, ("x", 9)));
      println(if a.1 == 1 then (1, "one") else (2, "two"));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(b2, 2)\n(a1, 1)\n(a1, 1)\n(x, 9)\n(1, one)\n");
  g.expectNoLeaks("TernaryYieldsATuple");
}

// Destructuring assigns existing variables (releasing what they held) and
// declares new ones; a target may itself be a tuple.
TEST(Tuple, DestructuringIntoExistingAndNewVariables) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn mk(n: int) -> (Str, (int, Str)) {
      return ("k" + Str<int>(n), (n, "v" + Str<int>(n)));
    }
    fn main() -> int {
      inner: (int, Str) = (0, "zero");
      k: Str = "old";
      k, inner = mk(1);              // both existing
      println(k);
      println(inner);
      k2, inner2 = mk(2);            // both new
      println(k2 + " " + inner2.1);
      _, inner = mk(3);
      println(inner);
      s, i: (int, Str) = (inner.1, inner);
      println(s + Str<int>(i.0));
      return inner2.0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "k1\n(1, v1)\nk2 v2\n(3, v3)\nv33\n");
  EXPECT_EQ(r.ExitCode, 2);
  g.expectNoLeaks("DestructuringIntoExistingAndNewVariables");
}

// The literal on the right is built before any target is assigned.
TEST(Tuple, SwapThroughATupleLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a = "a" + "1";
      b = "b" + "2";
      a, b = (b, a);
      println(a + b);
      p = ("x", "y");
      p = (p.1, p.0);
      println(p);
      t = (1, "one");
      u = (2, "two");
      t, u = (u, t);
      println(Str<int>(t.0) + t.1 + Str<int>(u.0) + u.1);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "b2a1\n(y, x)\n2two1one\n");
  g.expectNoLeaks("SwapThroughATupleLiteral");
}

TEST(Tuple, GenericClassOfATuple) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box<T> {
      item: T;
      view fn get() -> view T { return self.item; }
      fn put(v: T) { self.item = v; }
    }
    impl Box<T> { fn __init__(v: T) { self.item = v; } }
    fn main() -> int {
      b = Box<(Str, int)>(("a" + "b", 1));
      println(b.get());
      println(b.get().0);
      b.put(("c", 2));
      t: (Str, int) = b.item;
      b.put(("d", 3));
      println(t);
      println(b.item);
      return b.get().1;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(ab, 1)\nab\n(c, 2)\n(d, 3)\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("GenericClassOfATuple");
}

// `-> view T` with T a tuple: a view of plain values is copied out, one
// holding references is read in place.
TEST(Tuple, GenericViewResultOfATupleType) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn first<T>(xs: view T[]) -> view T { return xs[0]; }
    fn same<T>(x: view T) -> view T { return x; }
    fn main() -> int {
      nums: (int, float)[3] = [(1, 1.5), (2, 2.5), (3, 3.5)];
      g = first(nums);
      println(g);
      strs: (Str, int)[2] = [("p", 1), ("q", 2)];
      println(first(strs));
      println(Str<int>(first(strs).1) + first(strs).0);
      println(same(("s", 5)));
      return g.0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(1, 1.5)\n(p, 1)\n1p\n(s, 5)\n");
  EXPECT_EQ(r.ExitCode, 1);
  g.expectNoLeaks("GenericViewResultOfATupleType");
}

TEST(Tuple, ShowBoundSatisfiedByATuple) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn show<T: Show>(x: view T) -> Str { return "<" + x.toString() + ">"; }
    fn main() -> int {
      println(show((1, "x")));
      t: ((int, float), Str?) = ((1, 2.5), None);
      println(show(t));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "<(1, x)>\n<((1, 2.5), None)>\n");
  g.expectNoLeaks("ShowBoundSatisfiedByATuple");
}

// An `inout` tuple parameter is the caller's variable; a by-value one is the
// callee's own copy.
TEST(Tuple, InoutTupleParameter) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn bump(p: inout (int, Str)) { p = (p.0 + 1, p.1 + "+"); }
    fn local(p: (int, Str)) -> int { p = (p.0 + 10, "x"); return p.0; }
    fn main() -> int {
      p = (1, "v");
      bump(p);
      bump(p);
      println(p);
      println(Str<int>(local(p)));
      println(p);
      return p.0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "(3, v++)\n13\n(3, v++)\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("InoutTupleParameter");
}

TEST(Tuple, ReturnedFromAMethod) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Counter {
      n: int;
      name: Str;
      fn state() -> (Str, int) { return (self.name, self.n); }
      fn next() -> (int, Str) {
        self.n = self.n + 1;
        return (self.n, self.name + Str<int>(self.n));
      }
    }
    impl Counter { fn __init__(name: Str) { self.n = 0; self.name = name; } }
    fn main() -> int {
      c = Counter("c");
      i, s = c.next();
      println(Str<int>(i) + s);
      println(c.next());
      println(c.state());
      println(c.next().1);
      return c.state().1;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1c1\n(2, c2)\n(c, 2)\nc3\n");
  EXPECT_EQ(r.ExitCode, 3);
  g.expectNoLeaks("ReturnedFromAMethod");
}
