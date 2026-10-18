// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: private members (docs/design/visibility.md).  A `__` field or
// method is its class's or struct's own, reachable from that type's
// methods, `impl` blocks, trait impls and `static fn`s only; a public
// declaration cannot name a `__` type.  Module privacy is tested through
// the driver (tests/Driver/PkmTests.cpp, Pkm.PrivateNamesAreTheModulesOwn).

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {
void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}
} // namespace

TEST(Visibility, PrivateMembersAreReachableFromTheirOwnMethods) {
  auto r = semaCheck(R"(
class Counter {
  __count: int;
  step: int;
  fn tick() { self.__bump(self.step); }
  view fn value() -> int { return self.__count; }
  fn __bump(by: int) { self.__count = self.__count + by; }
}
impl Counter {
  static fn make(n: int) -> Counter { c = Counter(0, n); c.__count = 1; c.__bump(1); return c; }
  fn reset() { self.__count = 0; }
  fn copyFrom(o: view Counter) { self.__count = o.__count; }
  fn later() -> (int) -> int { return fn(d: int) -> int { return self.__count + d; }; }
}
trait Named { view fn name() -> Str; }
impl Named for Counter { view fn name() -> Str { return "C" + Str(self.__count); } }
struct P { __x: int; y: int; }
impl P {
  fn __set(v: int) { self.__x = v; }
  fn bump() { self.__set(self.__x + 1); }
  view fn x() -> int { return self.__x; }
  static fn origin() -> P { p = P(0, 0); p.__x = 1; return p; }
}
class __Node { v: int; next: __Node?; }
fn __helper(n: view __Node) -> int { return n.v; }
class Box<T> { __v: T; view fn get() -> T { return self.__v; } fn set(v: T) { self.__v = v; } }
impl Box<T> { fn __init__(v: T) { self.__v = v; } fn take(o: Box<T>) { self.__v = o.__v; } }
fn main() -> int {
  c = Counter::make(3);
  c.tick();
  d = Counter(0, 1);
  d.copyFrom(c);
  p = P::origin();
  p.bump();
  n = __Node(5, None);
  b = Box(7);
  b.set(8);
  return c.value() + d.value() + c.name().len() + p.x() + __helper(n) + b.get() + c.later()(1);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics, "");
}

TEST(Visibility, PrivateMembersAreNotReachableElsewhere) {
  auto r = semaCheck(R"(
class Counter {
  __count: int;
  fn tick() { self.__count = self.__count + 1; }
  fn __secret() -> int { return self.__count; }
}
impl Counter { static fn __make() -> Counter { return Counter(0); } }
class Sub : Counter {
  fn peek() -> int { return self.__count; }
  fn poke() { self.__secret(); }
}
class Other { c: Counter; fn look() -> int { return self.c.__count; } }
struct P { __x: int; }
fn main() -> int {
  c = Counter(0);
  c.__count = 5;
  n = c.__count;
  m = c.__secret();
  k = Counter::__make();
  g = Counter::__make;
  p = P(1);
  p.__x = 2;
  f = fn() -> int { return c.__count; };
  return n + m + f();
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {":9:29: error: '__count' is private to 'Counter': it is "
                "reachable only from its own methods",
                ":10:15: error: '__secret' is private to 'Counter': it is "
                "reachable only from its own methods",
                ":12:53: error: '__count' is private to 'Counter'",
                ":16:3: error: '__count' is private to 'Counter'",
                ":17:7: error: '__count' is private to 'Counter'",
                ":18:7: error: '__secret' is private to 'Counter'",
                ":19:7: error: '__make' is private to 'Counter'",
                ":20:7: error: '__make' is private to 'Counter'",
                ":22:3: error: '__x' is private to 'P': it is reachable only "
                "from its own methods",
                ":23:28: error: '__count' is private to 'Counter'"});
  EXPECT_EQ(r.ErrorCount, 10u) << r.Diagnostics;
}

TEST(Visibility, PublicSurfaceCannotNameAPrivateType) {
  auto r = semaCheck(R"(
class __Hidden { v: int; }
struct __Pt { x: int; }
fn make() -> __Hidden { return __Hidden(1); }
fn take(h: __Hidden, k: int) {}
fn takeList(hs: __Pt[2]) -> int { return hs[0].x; }
class Pub {
  h: __Hidden;
  __mine: __Hidden;
  fn get() -> __Hidden { return self.h; }
  fn __peek() -> __Hidden { return self.h; }
}
impl Pub { fn __init__(h: __Hidden) { self.h = h; self.__mine = h; } }
fn __mk() -> __Hidden { return __Hidden(2); }
class __Holder { h: __Hidden; fn get() -> __Hidden { return self.h; } }
fn main() -> int { return take(make(), 1).len(); }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'make' is public but its result names the private type "
                "'__Hidden'; make the function private or the type public",
                "'take' is public but parameter 'h' names the private type "
                "'__Hidden'; make the function private or the type public",
                "'takeList' is public but parameter 'hs' names the private "
                "type '__Pt'",
                "field 'h' of 'Pub' is public but it names the private type "
                "'__Hidden'; make the field private or the type public",
                "method 'get' of 'Pub' is public but its result names the "
                "private type '__Hidden'; make the method private or the "
                "type public",
                "method '__init__' of 'Pub' is public but parameter 'h' names "
                "the private type '__Hidden'"});
  // `__mine`, `__peek`, `__mk` and `__Holder` may name it.
  EXPECT_EQ(r.Diagnostics.find("'__mine'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'__peek'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'__mk'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'__Holder'"), std::string::npos)
      << r.Diagnostics;
}

TEST(Visibility, ReservedAndSingleUnderscoreNamesAreNotPrivate) {
  auto r = semaCheck(R"(
class C { _x: int; __y: int; }
impl C {
  fn __init__(x: int) { self._x = x; self.__y = x; }
  fn __repr() -> int { return self.__y; }
}
class D : C { fn x() -> int { return self._x; } }
impl D { fn __init__() { __super__(7); } }
fn main() -> int {
  c = C(1);
  d = D();
  return c._x + d.x() + c.__repr();
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'__repr' is private to 'C': it is reachable only from its "
                "own methods"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(Visibility, AnInstantiationsMethodsAreTheTemplates) {
  // `List<int>`'s private members are reachable from `List<Str>`'s
  // methods: both are the template's code.
  auto r = semaCheck(R"(
class Box<T> { __v: T; }
impl Box<T> {
  fn __init__(v: T) { self.__v = v; }
  fn get() -> T { return self.__v; }
  view fn sameAs(o: view Box<int>) -> bool { return o.__v == 1; }
}
fn main() -> int {
  a = Box("s");
  b = Box(1);
  if (a.sameAs(b)) { return a.get().len(); }
  return b.__v;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {":12:10: error: '__v' is private to 'Box<int>': it is "
                "reachable only from its own methods"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}
