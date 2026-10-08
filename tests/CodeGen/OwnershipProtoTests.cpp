// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The lowering of the ownership prototype's `inout` value parameters
// (--ownership, docs/design/ownership-proto.md), on every backend.  Every
// test also checks that the program leaves no live heap block.

#include "CodeGenTestUtils.h"
#include "paykan/pir/Printer.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectRun(const std::string &src, const std::string &out,
               const char *label) {
  LeakGuard g;
  auto r = compileAndRunOwnership(src);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0) << r.StdErr;
  EXPECT_EQ(r.StdOut, out);
  g.expectNoLeaks(label);
}

} // namespace

// References take no qualifiers: passing or assigning one shares the object,
// and any reference may change it, as without --ownership.
TEST(OwnershipProto, ReferencesAreShared) {
  expectRun(R"(
    class Counter {
      n: int;
      fn __init__() { self.n = 0; }
      fn tick() { self.n = self.n + 1; }
    }
    fn poke(c: Counter, xs: int[], s: Str) {
      c.tick(); xs.push(9); s.concat("!");
    }
    fn main() -> int {
      a = Counter(); b = a; xs = [1]; ys = xs; s = "hi"; t = s;
      poke(b, ys, t);
      println(Str<int>(a.n) + " " + Str<int>(xs.len()) + " " + s);
      return 0;
    }
  )",
            "1 2 hi!\n", "ReferencesAreShared");
}

// An `inout` parameter is the caller's storage, for every scalar type: the
// spec's `bump(k)` leaves `k == 2`.  A `view` parameter is passed by value.
TEST(OwnershipProto, InoutParametersChangeTheCaller) {
  expectRun(R"(
    enum Color { Red, Green }
    fn bump(inout n: int) { n = n + 1; }
    fn scale(inout x: float, view by: float) { x = x * by; }
    fn flip(inout b: bool) { b = !b; }
    fn next(inout c: char) { c = char<int>(int<char>(c) + 1); }
    fn green(inout c: Color) { c = Color::Green; }
    fn twice(inout n: int) { bump(n); bump(n); }
    fn swap(inout a: int, inout b: int) { t = a; a = b; b = t; }
    fn split(inout a: int, inout b: int) { a, b = (b * 2, a); }
    fn main() -> int {
      k = 1;
      bump(k);
      println(Str<int>(k));
      f = 1.5;
      scale(f, 2.0);
      b = False;
      flip(b);
      c = 'a';
      next(c);
      col = Color::Red;
      green(col);
      println(Str<float>(f) + " " + Str<bool>(b) + " " + Str<char>(c) + " " +
              Str<bool>(col == Color::Green));
      twice(k);
      i = 0;
      while (i < 10) { bump(k); i = i + 1; }
      x = 7;
      swap(k, x);
      println(Str<int>(k) + " " + Str<int>(x));
      split(k, x);
      println(Str<int>(k) + " " + Str<int>(x));
      return 0;
    }
  )",
            "2\n3 True b True\n7 14\n28 7\n", "InoutParametersChangeTheCaller");
}

// Methods and constructors take `inout` parameters too; a field or an array
// element argument is copied in and written back after the call.
TEST(OwnershipProto, InoutArgumentsFromFieldsAndElements) {
  expectRun(R"(
    class Counter {
      n: int;
      xs: int[];
      fn __init__(inout start: int) { self.n = start; start = 0;
        self.xs = [1, 2]; }
      fn add(inout to: int, k: int) { to = to + k; }
      fn grow() { self.add(self.xs[1], 10); }
    }
    class Sub : Counter {
      fn __init__(inout start: int) { __super__(start); start = 9; }
      fn add(inout to: int, k: int) { to = to + 2 * k; }
    }
    fn bump(inout n: int) { n = n + 1; }
    fn make() -> Counter { s = 5; return Counter(s); }
    fn main() -> int {
      s = 4;
      c = Counter(s);
      c.add(s, 3);
      bump(c.n);
      c.add(c.n, 2);
      bump(c.xs[0]);
      c.grow();
      i = 0;
      while (i < 3) { bump(c.xs[i % 2]); c.add(c.xs[1], 1); i = i + 1; }
      bump(make().n);
      println(Str<int>(s) + " " + Str<int>(c.n) + " " + Str<int>(c.xs[0]) +
              " " + Str<int>(c.xs[1]));
      d: Counter = Sub(s);
      d.add(s, 1);
      println(Str<int>(s) + " " + Str<int>(d.n));
      return 0;
    }
  )",
            "3 7 4 16\n11 3\n", "InoutArgumentsFromFieldsAndElements");
}

// An optional is a value: an `inout` optional is the caller's slot, which
// the callee sets to a value, to None or to another object.  The slot owns
// its box (released when replaced), so nothing leaks or dangles.
TEST(OwnershipProto, InoutOptionalsRebindTheCallersSlot) {
  expectRun(R"(
    class Counter {
      n: int;
      fn __init__(n: int) { self.n = n; }
    }
    class Holder {
      c: Counter?;
      k: int?;
      fn __init__() { self.c = Counter(1); self.k = None; }
    }
    fn setInt(inout x: int?, v: int) { x = v; }
    fn clearInt(inout x: int?) { x = None; }
    fn renew(inout c: Counter?, n: int) { c = Counter(n); }
    fn drop(inout c: Counter?) { c = None; }
    fn swap(inout a: Counter?, inout b: Counter?) { t = a; a = b; b = t; }
    fn twice(inout c: Counter?, n: int) { renew(c, n); renew(c, n + 1); }
    fn shout(inout s: Str?) {
      match s { v: Str { s = v + "!"; } None { s = "none"; } }
    }
    fn num(view x: int?) -> Str {
      match x { v: int { return Str<int>(v); } None { return "-"; } }
    }
    fn show(view c: Counter?) -> Str {
      match c { v: Counter { return Str<int>(v.n); } None { return "-"; } }
    }
    fn main() -> int {
      x: int? = 1;
      setInt(x, 5);
      a = num(x);
      clearInt(x);
      println(a + " " + num(x));
      c: Counter? = Counter(1);
      keep = c;
      d: Counter? = None;
      renew(c, 2);
      swap(c, d);
      println(show(c) + " " + show(d) + " " + show(keep));
      drop(d);
      twice(c, 7);
      println(show(c) + " " + show(d));
      s: Str? = "hi";
      shout(s);
      t: Str? = None;
      shout(t);
      match s { v: Str { println(v); } None { } }
      match t { v: Str { println(v); } None { } }
      h = Holder();
      renew(h.c, 3);
      setInt(h.k, 4);
      println(show(h.c) + " " + num(h.k));
      drop(h.c);
      xs: int?[] = [1, None];
      setInt(xs[1], 9);
      clearInt(xs[0]);
      cs: Counter?[] = [Counter(1), None];
      renew(cs[1], 6);
      drop(cs[0]);
      println(show(h.c) + " " + num(xs[0]) + " " + num(xs[1]) + " " +
              show(cs[0]) + " " + show(cs[1]));
      return 0;
    }
  )",
            "5 -\n- 2 1\n8 -\nhi!\nnone\n3 4\n- - 9 - 6\n",
            "InoutOptionalsRebindTheCallersSlot");
}

// The lowering passes a variable's address (local.addr), passes an `inout`
// parameter's address on unchanged, and reads and writes through it, for a
// scalar and for an optional's box.
TEST(OwnershipProto, InoutParameterIsAPointer) {
  auto a = detail::analyse(R"(
    fn bump(inout n: int) { n = n + 1; }
    fn twice(inout n: int) { bump(n); }
    fn read(view n: int) -> int { return n; }
    fn none(inout s: Str?) { s = None; }
    fn main() -> int { k = 1; twice(k); s: Str? = "a"; none(s);
      return read(k); }
  )",
                           "", "recursive-descent", true);
  ASSERT_TRUE(a.Ok) << a.Failure.StdErr;
  paykan::pir::Program program;
  std::ostringstream errs, text;
  ASSERT_TRUE(paykan::lowering::lowerProgram(a.Ctx, a.Driver->getRoot(), a.Path,
                                             "", program, errs))
      << errs.str();
  paykan::pir::print(program, text);
  std::string pir = text.str();
  EXPECT_NE(pir.find("fn @bump(%n.1: ptr)"), std::string::npos) << pir;
  EXPECT_NE(pir.find("fn @read(%n.1: i64)"), std::string::npos) << pir;
  EXPECT_NE(pir.find("ptr.load i64, %n."), std::string::npos) << pir;
  EXPECT_NE(pir.find("ptr.store %n."), std::string::npos) << pir;
  EXPECT_NE(pir.find("local.addr %k."), std::string::npos) << pir;
  EXPECT_EQ(pir.find("local.addr %n."), std::string::npos) << pir;
  // An optional's slot: the old box is released, the new one stored.
  EXPECT_NE(pir.find("fn @none(%s.1: ptr)"), std::string::npos) << pir;
  EXPECT_NE(pir.find("ptr.load box, %"), std::string::npos) << pir;
  EXPECT_NE(pir.find("local.addr %s."), std::string::npos) << pir;
}
