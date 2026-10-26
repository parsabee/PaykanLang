// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: `Str(x)` of every value with a text
// (docs/language/04-strings-and-conversions.md, "To a string"), which is
// `x.toString()`: the same text, the same box (a fresh one the caller owns)
// and the same holds — an object whose `toString()` lets go of the only
// field that holds it stays alive until it returns, as it does for
// `h.c.toString()`.  The scalars' `Str<int>` & co. are in ConversionTests.cpp.
// Every test checks zero live heap blocks; samples/codegen/77_str_of_value
// and samples/leak-check/32_str_of_value run the same shapes on every path
// (SamplesParity, CStrictC11).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

const char *const kShapes = R"(
  class P {
    x: int;
    view fn toString() -> Str { return "P" + Str(self.x); }
  }
  class Q : P { }
  class Plain { n: int; }
  struct S { a: int; b: Str; }
  enum E { A, B(n: int, s: Str) }
  enum Color { Red, Green }
)";

} // namespace

// `Str(x) == x.toString()` for every kind of value with a text, each a box
// of its own the comparison releases.
TEST(StrOfValue, IsTheTextToStringGives) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShapes) + R"(
    import ::io;
    fn same<T>(x: T) -> Str {
      return Str(Str(x) == x.toString());
    }
    fn main() -> int {
      o: int? = None;
      p: int? = 5;
      s: Str? = "s" + Str(1);
      t: Str? = None;
      a: P? = P(1);
      b: P? = None;
      io::println(same(o) + same(p) + same(s) + same(t) + same(a) + same(b));
      io::println(same((1, "a" + Str(2), 2.5)) + same(S(1, "b" + Str(3)))
          + same(E::A) + same(E::B(4, "c" + Str(5))) + same(Color::Green));
      ob: Obj = Q(6);
      io::println(same(P(7)) + same(Q(8)) + same(Plain(9)) + same(ob));
      n = None;
      io::println(Str(Str(None) == n.toString()) + " " + Str(None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "TrueTrueTrueTrueTrueTrue\n"
                      "TrueTrueTrueTrueTrue\n"
                      "TrueTrueTrueTrue\n"
                      "True None\n");
  g.expectNoLeaks("StrOfValue.IsTheTextToStringGives");
}

// The texts themselves: what println prints.
TEST(StrOfValue, PrintsWhatPrintlnPrints) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShapes) + R"(
    import ::io;
    fn main() -> int {
      o: int? = None;
      p: int? = 5;
      s: Str? = "hi";
      a: P? = P(1);
      io::println(Str(o) + " " + Str(p) + " " + Str(s) + " " + Str(a));
      io::println(o); io::println(p); io::println(s); io::println(a);
      io::println(Str((1, "a", 2.5)) + " " + Str(S(1, "b")) + " "
          + Str(E::B(4, "c")) + " " + Str(Color::Red));
      io::println((1, "a", 2.5)); io::println(S(1, "b"));
      io::println(E::B(4, "c")); io::println(Color::Red);
      ob: Obj = Q(3);
      io::println(Str(ob) + " " + Str<P>(P(2)) + " " + Str<int?>(p) + " "
          + Str<(int, Str)>((1, "x")));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "None 5 hi P1\nNone\n5\nhi\nP1\n"
                      "(1, a, 2.5) S(a: 1, b: b) B(n: 4, s: c) Red\n"
                      "(1, a, 2.5)\nS(a: 1, b: b)\nB(n: 4, s: c)\nRed\n"
                      "P3 P2 5 (1, x)\n");
  g.expectNoLeaks("StrOfValue.PrintsWhatPrintlnPrints");
}

// Of a call's result, a field, a list's element and a generic value; kept
// in a variable, a field and a tuple, joined with `+`, passed on, dropped.
TEST(StrOfValue, EveryPlaceAValueComesFromOrGoesTo) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kShapes) + R"(
    import ::io;
    import ::collections;
    class Box { p: P; text: Str; }
    fn make(n: int) -> P { return P(n); }
    fn show<T>(x: T) -> Str { return Str(x); }
    fn len(s: Str) -> int { return s.len(); }
    fn main() -> int {
      bx = Box(make(1), "x");
      xs = collections::List<P>();
      xs.push(make(2));
      io::println(Str(make(3)) + Str(bx.p) + Str(xs.at(0)) + show(make(4))
          + show((make(5), 6)) + show(Color::Green));
      kept = Str(make(7));
      bx.text = Str(make(8));
      pair = (Str(make(9)), 1);
      io::println(kept + bx.text + pair.0 + Str(len(Str(make(10)))));
      Str(make(11));
      bx.text = Str(bx);
      io::println(Str(bx.text.len() > 0));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "P3P1P2P4(P5, 6)Green\nP7P8P93\nTrue\n");
  g.expectNoLeaks("StrOfValue.EveryPlaceAValueComesFromOrGoesTo");
}

// `Str(s)` and `Str<Str>(s)` of a `Str` are the string itself.
TEST(StrOfValue, AStrIsItself) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    import ::io;
    fn main() -> int {
      s = "a" + Str(1);
      t = Str<Str>(s);
      u = Str<Str>("b" + Str(2));
      o: Str? = s;
      io::println(t + " " + u + " " + Str(t == s) + " " + Str(o) + " "
          + Str<Str?>(o));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "a1 b2 True a1 a1\n");
  g.expectNoLeaks("StrOfValue.AStrIsItself");
}

// An object whose `toString()` replaces the only field that holds it,
// through a closure it calls: `Str(h.c)`, `Str(a.b.c)` and `Str(self.c)`
// hold it until the call returns, as `h.c.toString()` does.  Without the
// hold its `self.name` is read after the object is freed (the C path under
// AddressSanitizer reports it).
TEST(StrOfValue, AnObjectItsToStringLetsGoOfStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    import ::io;
    class C {
      name: Str;
      fire: () -> void;
      view fn toString() -> Str {
        self.fire();
        y = C("y" + Str(9), fn() {});
        return self.name + ".";
      }
    }
    class H {
      c: C;
      view fn show() -> Str { return Str(self.c); }
      view fn told() -> Str { return self.c.toString(); }
    }
    class B { c: C; }
    class A { b: B; }
    fn arm(h: H, n: int) {
      v = h;
      h.c = C("two" + Str(n), fn() { v.c = C("three" + Str(n), fn() {}); });
    }
    fn armB(b: B, n: int) {
      v = b;
      b.c = C("four" + Str(n), fn() { v.c = C("five" + Str(n), fn() {}); });
    }
    fn shown(h: H) -> Str { return Str(h.c); }
    fn main() -> int {
      h = H(C("one" + Str(1), fn() {}));
      arm(h, 1);
      io::println("Str(h.c): " + Str(h.c));
      arm(h, 2);
      io::println("h.c.toString(): " + h.c.toString());
      a = A(B(C("one" + Str(1), fn() {})));
      armB(a.b, 3);
      io::println("Str(a.b.c): " + Str(a.b.c));
      armB(a.b, 4);
      io::println("a.b.c.toString(): " + a.b.c.toString());
      arm(h, 5);
      io::println("Str(self.c): " + h.show());
      arm(h, 6);
      io::println("self.c.toString(): " + h.told());
      arm(h, 7);
      io::println("in a callee: " + shown(h) + " " + h.c.toString());
      arm(h, 8);
      o: C? = h.c;
      io::println("optional: " + Str(o));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "Str(h.c): two1.\nh.c.toString(): two2.\n"
                      "Str(a.b.c): four3.\na.b.c.toString(): four4.\n"
                      "Str(self.c): two5.\nself.c.toString(): two6.\n"
                      "in a callee: two7. three7.\noptional: two8.\n");
  g.expectNoLeaks("StrOfValue.AnObjectItsToStringLetsGoOfStaysAlive");
}
