// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: private names (docs/design/visibility.md).  A `__` field or
// method is its class's or struct's own, reachable from that type's
// methods, `impl` blocks, trait impls and `static fn`s only; a `_` name is
// its module's own, and a public declaration cannot name a `_` type.  Module
// privacy across an import is tested through the driver
// (tests/Driver/PkmTests.cpp, Pkm.PrivateNamesAreTheModulesOwn).

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
class _Node { v: int; next: _Node?; }
fn _helper(n: view _Node) -> int { return n.v; }
class Box<T> { __v: T; view fn get() -> T { return self.__v; } fn set(v: T) { self.__v = v; } }
impl Box<T> { fn __init__(v: T) { self.__v = v; } fn take(o: Box<T>) { self.__v = o.__v; } }
fn main() -> int {
  c = Counter::make(3);
  c.tick();
  d = Counter(0, 1);
  d.copyFrom(c);
  p = P::origin();
  p.bump();
  n = _Node(5, None);
  b = Box(7);
  b.set(8);
  return c.value() + d.value() + c.name().len() + p.x() + _helper(n) + b.get() + c.later()(1);
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
class _Hidden { v: int; }
struct _Pt { x: int; }
fn make() -> _Hidden { return _Hidden(1); }
fn take(h: _Hidden, k: int) {}
fn takeList(hs: _Pt[2]) -> int { return hs[0].x; }
class Pub {
  h: _Hidden;
  __mine: _Hidden;
  fn get() -> _Hidden { return self.h; }
  fn __peek() -> _Hidden { return self.h; }
}
impl Pub { fn __init__(h: _Hidden) { self.h = h; self.__mine = h; } }
fn _mk() -> _Hidden { return _Hidden(2); }
class _Holder { h: _Hidden; fn get() -> _Hidden { return self.h; } }
fn main() -> int { return take(make(), 1).len(); }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'make' is public but its result names the private type "
                "'_Hidden'; make the function private or the type public",
                "'take' is public but parameter 'h' names the private type "
                "'_Hidden'; make the function private or the type public",
                "'takeList' is public but parameter 'hs' names the private "
                "type '_Pt'",
                "field 'h' of 'Pub' is public but it names the private type "
                "'_Hidden'; make the field private or the type public",
                "method 'get' of 'Pub' is public but its result names the "
                "private type '_Hidden'; make the method private or the "
                "type public",
                "method '__init__' of 'Pub' is public but parameter 'h' names "
                "the private type '_Hidden'"});
  // `__mine`, `__peek`, `_mk` and `_Holder` may name it.
  EXPECT_EQ(r.Diagnostics.find("'__mine'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'__peek'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'_mk'"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("'_Holder'"), std::string::npos)
      << r.Diagnostics;
}

TEST(Visibility, ReservedNamesAreNotPrivateAndUnderscoreMembersAreTheModules) {
  // `_x` is reachable anywhere in its module: a subclass, a free function.
  auto r = semaCheck(R"(
class C { _x: int; __y: int; }
impl C {
  fn __init__(x: int) { self._x = x; self.__y = x; }
  fn __repr() -> int { return self.__y; }
}
class D : C { fn x() -> int { return self._x + self._half(); } }
impl C { view fn _half() -> int { return self._x / 2; } static fn _one() -> C { return C(1); } }
impl D { fn __init__() { __super__(7); } }
struct S { _n: int; }
fn main() -> int {
  c = C::_one();
  d = D();
  s = S(2);
  s._n = 3;
  return c._x + d.x() + c._half() + s._n + c.__repr();
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
  if a.sameAs(b) { return a.get().len(); }
  return b.__v;
}
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {":12:10: error: '__v' is private to 'Box<int>': it is "
                "reachable only from its own methods"});
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
}

TEST(Visibility, DoubleUnderscoreIsForMembersOnly) {
  auto r = semaCheck(R"(
fn __f() -> int { return 1; }
class __C { v: int; }
struct __S { v: int; }
enum __E { A }
trait __T { view fn t() -> int; }
class Box<T> { v: T; }
fn __g<T>(v: T) -> T { return v; }
fn main() -> int { return 0; }
)");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"function '__f': the '__' prefix makes a member private to "
                "its class or struct; a module's own function is spelled "
                "'_f'",
                "class '__C': the '__' prefix", "struct '__S': the '__' prefix",
                "enum '__E': the '__' prefix", "trait '__T': the '__' prefix",
                "function '__g': the '__' prefix"});
}

// `___init__` is the type's initializer, private to its module: the module's
// own code constructs the type, extends it, and calls the factories that do;
// it is still the one initializer, and still not a top-level name.
TEST(Visibility, ModulePrivateInitializerIsTheModulesOwn) {
  auto r = semaCheck(R"(
class Conn { __h: int; }
impl Conn {
  fn ___init__(h: int) { self.__h = h; }
  static fn open(h: int) -> Conn { return Conn(h); }
  view fn h() -> int { return self.__h; }
}
class Sub : Conn {}
impl Sub { fn __init__() { __super__(9); } }
class Pool { c: Conn; }
impl Pool { fn __init__() { self.c = Conn(1); } fn renew() { self.c = Conn(2); } }
struct P { x: int; }
impl P {
  fn ___init__(x: int) { self.x = x; }
  static fn origin() -> P { return P(0); }
}
class Box<T> { v: T; }
impl Box<T> { fn ___init__(v: T) { self.v = v; } }
fn boxOf(v: Str) -> Box<Str> { return Box<Str>(v); }
fn mk() -> Conn { return Conn(3); }
fn main() -> int {
  a = Conn(4);
  b = Conn::open(5);
  c = mk();
  d = Sub();
  p = P(6);
  q = P::origin();
  e = Box<int>(7);
  f = boxOf("s");
  g = fn() -> Conn { return Conn(8); };
  return a.h() + b.h() + c.h() + d.h() + p.x + q.x + e.v + g().h();
})");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;

  auto bad = semaCheck(R"(
class Twice { n: int; }
impl Twice {
  fn __init__(n: int) { self.n = n; }
  fn ___init__(n: int) { self.n = n; }
}
fn ___init__() {}
fn main() -> int { return 0; })");
  EXPECT_FALSE(bad.Ok);
  expectAll(bad, {"class 'Twice' already has a method '__init__'",
                  "'___init__' is the initializer of a class or a struct, "
                  "private to its module; it goes in an 'impl' block"});
}
