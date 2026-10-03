// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: optional primitives `int?`, `float?`, `bool?`, `char?` (#66) —
// boxing conversions, what does not convert, `match` with a primitive arm,
// equality, literals, generics, and the remaining rejections (`Enum?`,
// `void?`, `T??`).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string inMain(const std::string &body) {
  return "fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

static void expectOk(const std::string &src) {
  auto r = semaCheck(src);
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

static void expectError(const std::string &src, const std::string &needle) {
  auto r = semaCheck(src);
  EXPECT_FALSE(r.Ok) << "expected an error containing: " << needle;
  EXPECT_NE(r.Diagnostics.find(needle), std::string::npos)
      << "diagnostics were:\n"
      << r.Diagnostics;
}

// ─── accepted ───────────────────────────────────────────────────────────────

TEST(OptionalPrimitive, EveryPrimitiveWidensAndTakesNone) {
  expectOk(inMain(R"(
    a: int? = 5;      a = None;   a = 7;
    f: float? = 2.5;  f = None;   f = 3;     // int -> float, then boxed
    b: bool? = True;  b = None;
    c: char? = 'x';   c = None;
    n: int = 4;
    a = n;
    a2: int? = a;                            // same type: shares the box
  )"));
}

TEST(OptionalPrimitive, ParamsReturnsFieldsArraysTuples) {
  expectOk(R"(
    class H { n: int?; c: char?; fn __init__() { } }
    fn half(n: int) -> int? { if (n % 2 != 0) { return None; } return n / 2; }
    fn take(x: float?) -> bool { return x == None; }
    fn main() -> int {
      h = H();                               // optional fields start as None
      h.n = 3;
      h.c = None;
      r = half(4);
      t = take(1);                           // int literal -> float?
      xs: int?[] = [1, 2];                   // literal elements are boxed
      xs.push(None);
      xs.push(3);
      xs[0] = None;
      p: (int?, Str) = (1, "a");
      q = if t then 3 else None;             // int?
      return 0;
    }
  )");
}

TEST(OptionalPrimitive, MatchBindsThePlainValue) {
  expectOk(R"(
    fn value(o: int?) -> int {
      match o {
        n: int { n = n + 1; return n; }       // a plain, reassignable int
        None   { return -1; }
      }
    }
    fn main() -> int {
      c: char? = 'a';
      match c { ch: char { x: char = ch; } _ { } }
      b: bool? = False;
      match b { v: bool { if (v) { } } None { } }
      return value(None);
    }
  )");
}

TEST(OptionalPrimitive, EqualityAndObj) {
  expectOk(inMain(R"(
    a: int? = 1;
    b: int? = None;
    x = a == b;
    y = a != None;
    z = None == b;
    o: Obj = a;                              // prints / matches as an Int box
    println(b);
  )"));
}

TEST(OptionalPrimitive, GenericsInstantiateWithOptionalPrimitives) {
  expectOk(R"(
    class Cell<T> { v: T; fn __init__(x: T) { self.v = x; } }
    class Slot<T> { v: T?; fn __init__() { } fn put(x: T) { self.v = x; } }
    fn wrap<T>(x: T) -> T? { return x; }
    fn main() -> int {
      c = Cell<int?>(None);
      c.v = 4;
      s = Slot<float>();
      s.put(1);
      w: int? = wrap(3);
      return 0;
    }
  )");
}

// ─── rejected ───────────────────────────────────────────────────────────────

TEST(OptionalPrimitive, NeverNarrowsWithoutMatch) {
  expectError(inMain("a: int? = 1;\n n: int = a;"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain("a: int? = 1;\n n = a + 1;"),
              "cannot use optional 'int?' as 'int' without unwrapping");
  expectError(inMain("a: bool? = True;\n if (a) { }"),
              "if condition must be 'bool', got 'bool?'");
}

TEST(OptionalPrimitive, ComparingWithAPresentValueIsAnError) {
  expectError(inMain("a: int? = 1;\n b = a == 1;"),
              "cannot compare optional 'int?' with non-optional 'int'");
  expectError(inMain("a: int? = 1;\n f: float? = 1.0;\n b = a == f;"),
              "operands of '==' have mismatched types 'int?' and 'float?'");
}

TEST(OptionalPrimitive, NoReboxingBetweenOptionals) {
  expectError(inMain("a: int? = 1;\n f: float? = a;"),
              "does not match declared type 'float?'");
}

TEST(OptionalPrimitive, PrimitiveArrayIsNotAnOptionalArray) {
  expectError(inMain("xs: int[] = [1];\n ys: int?[] = xs;"),
              "does not match declared type 'int?[]'");
}

TEST(OptionalPrimitive, MatchArmMustNameThePrimitive) {
  expectError(inMain("a: int? = 1;\n match a { f: float { } None { } }"),
              "match arm type 'float' does not match the optional subject "
              "type 'int?'");
  expectError(inMain("a: int? = 1;\n match a { 1 { } None { } }"),
              "match on optional 'int?' requires type-name arms or 'None'");
  expectError(inMain("a: int? = 1;\n match a { n: int { } m: int { } }"),
              "unreachable arm: the 'int' arm above already matches every "
              "non-None value");
}

TEST(OptionalPrimitive, MatchWithoutNoneArmIsNotExhaustive) {
  expectError(R"(
    fn f(o: int?) -> int { match o { n: int { return n; } } }
    fn main() -> int { return 0; }
  )",
              "does not always return a value");
}

TEST(OptionalPrimitive, DestructuringDoesNotBox) {
  expectError(inMain("t = (1, \"a\");\n a: int?, s: Str = t;"),
              "does not match declared type 'int?'");
}

TEST(OptionalPrimitive, EnumVoidAndNestedStayRejected) {
  expectError(R"(
    enum Color { Red, Green }
    fn main() -> int { c: Color? = None; return 0; }
  )",
              "optional enum types are not supported yet");
}
