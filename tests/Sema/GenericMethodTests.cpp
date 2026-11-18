// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: generic methods and generic `static fn`s (#353): a method of
// an `impl` block, or a `static fn`, with type parameters of its own.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// A generic method's or generic `static fn`'s type parameters are its own:
// no name twice, none a type's, none its `impl` block's.  Their bounds are
// checked where they are declared, and may name the block's parameters.  A
// public one's written signature names no type private to the module.
TEST(GenericMethods, TypeParametersAreTheMembersOwn) {
  auto r = semaCheck(R"pkn(class Box<T> { v: T; }
impl Box<T> {
  fn a<U, U>(x: U) -> int { return 1; }
  fn b<Str>(x: Str) -> int { return 1; }
  fn c<T>(x: T) -> int { return 1; }
  static fn d<T>(x: T) -> int { return 1; }
}
class P { x: int; }
impl P {
  fn a<U, U>(x: U) -> int { return 1; }
  fn b<P>(x: int) -> int { return 1; }
  static fn c<V, V>(x: V) -> int { return 1; }
}
struct S { x: int; }
impl S {
  fn a<S>(x: int) -> int { return 1; }
}
trait Keyed<Q> { view fn key() -> Q; }
class W<T> { v: T; }
impl W<T> {
  view fn a<U: Nope>(x: view U) -> int { return 1; }
  static fn b<U: P>(x: view U) -> int { return 1; }
  view fn c<U: Keyed<T>>(x: view U) -> T { return x.key(); }
}
struct _Loc { n: int; }
class _Own { n: int; }
class Pub { n: int; }
impl Pub {
  view fn leak<U>(u: view U) -> _Loc { return _Loc(self.n); }
  view fn take<U>(u: view U, l: (_Loc, int)) -> int { return self.n; }
  static fn made<U>(u: view U) -> _Loc? { return None; }
  view fn __hidden<U>(u: view U) -> _Loc { return _Loc(self.n); }
  view fn fine<_U>(u: view _U) -> int { return self.n; }
}
impl _Own {
  view fn fine<U>(u: view U) -> _Loc { return _Loc(self.n); }
}
fn main() -> int { return 0; }
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 13u) << r.Diagnostics;
  for (const char *want : {
           ":3:3: error: duplicate type parameter 'U' in generic method 'a'",
           ":4:3: error: type parameter 'Str' of generic method 'b' shadows a "
           "type of the same name",
           ":5:3: error: type parameter 'T' of generic method 'c' shadows type "
           "parameter 'T' of the 'impl' block for 'Box'",
           ":6:10: error: type parameter 'T' of generic function 'Box::d' "
           "shadows type parameter 'T' of the 'impl' block for 'Box'",
           ":10:3: error: duplicate type parameter 'U' in generic method 'a'",
           ":11:3: error: type parameter 'P' of generic method 'b' shadows a "
           "type of the same name",
           ":29:3: error: method 'leak' of 'Pub' is public but its result "
           "names the private type '_Loc'; make the method private or the type "
           "public",
           ":30:3: error: method 'take' of 'Pub' is public but parameter 'l' "
           "names the private type '_Loc'; make the method private or the type "
           "public",
           ":16:3: error: type parameter 'S' of generic method 'a' shadows a "
           "type of the same name",
           ":12:10: error: duplicate type parameter 'V' in generic function "
           "'P::c'",
           ":31:10: error: function 'Pub::made' is public but its result names "
           "the private type '_Loc'; make the function private or the type "
           "public",
           ":21:3: error: unknown trait 'Nope'",
           ":22:10: error: 'P' is not a trait",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
  // `U: Keyed<T>` names the block's `T`; a private member, a type
  // parameter named like a private type and a private type's member
  // are no public surface.
  for (const char *line : {":23:", ":32:", ":33:", ":36:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << r.Diagnostics;
}

// Type arguments are written after the method's name or inferred from the
// arguments and from the type the call flows into; a method that is not
// generic, a builtin's too, and a field of function type take none.
TEST(GenericMethods, ACallNamesAnInstantiation) {
  auto r = semaCheck(R"pkn(import ::io;
class P { x: int; cb: (int) -> int; }
impl P {
  fn get() -> int { return self.x; }
  view fn map<U>(f: (view int) -> U) -> U { return f(self.x); }
  view fn same<A>(a: view A, b: view A) -> int { return 1; }
  view fn none<U>() -> U? { return None; }
  static fn zero() -> P { return P(0, fn(n: int) => n); }
}
fn main() -> int {
  p: mut = P(1, fn(n: int) => n);
  io::println(p.get<int>());
  io::println(p.cb<int>(2));
  io::println(p.map<int, int>(fn(n: view int) => n));
  io::println(p.map(fn(n: int) => n));
  io::println(p.same(1, 2.5));
  io::println(p.nope<int>(1));
  let z = P::zero<int>();
  let n = p.none();
  let ok: int? = p.none();
  let also = p.none<Str>();
  let wrong = p.map<Str>(fn(n: view int) => n);
  let five = 5;
  let xs = [1, 2, 3];
  io::println(five.toString<Str>() + Str(xs.len<int>()) + "12".toInt<int>().toString());
  let chained: Str = p.none().toString();
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 13u) << r.Diagnostics;
  for (const char *want : {
           ":12:15: error: method 'get' of 'P' is not generic and takes no "
           "type arguments",
           ":13:15: error: 'cb' is a field of function type; a call through it "
           "takes no type arguments",
           ":14:15: error: generic method 'P.map' expects 1 type argument(s), "
           "got 2",
           ":15:15: error: cannot infer type parameter 'U' of 'P.map' from the "
           "call arguments",
           ":15:15: note: specify the type arguments explicitly: "
           "'map<...>(...)'",
           ":16:15: error: cannot infer type parameter 'A' of 'P.same': "
           "deduced as both 'int' and 'float'",
           ":16:15: note: specify the type arguments explicitly: "
           "'same<...>(...)'",
           ":17:15: error: no method 'nope' on type 'P'",
           ":18:11: error: 'P::zero' is not generic and takes no type "
           "arguments",
           ":19:11: error: cannot infer type parameter 'U' of 'P.none' from "
           "the call arguments",
           ":19:11: note: specify the type arguments explicitly: "
           "'none<...>(...)'",
           ":22:26: error: argument 1 of 'map<Str>' has type '(view int) -> "
           "int', expected '(view int) -> Str'",
           ":25:15: error: method 'toString' of 'int' is not generic and takes "
           "no type arguments",
           ":25:42: error: method 'len' of 'int[3]' is not generic and takes "
           "no type arguments",
           ":25:59: error: method 'toInt' of 'Str' is not generic and takes no "
           "type arguments",
           ":26:22: error: cannot infer type parameter 'U' of 'P.none' from "
           "the call arguments",
           ":26:22: note: specify the type arguments explicitly: "
           "'none<...>(...)'",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// A generic method is an `impl` method: nothing overrides it, and its name
// is taken in its type and in the classes derived from it, whichever of
// the two is declared first.
TEST(GenericMethods, TheNameIsTakenLikeAnImplMethods) {
  auto r = semaCheck(R"pkn(class Shape { view fn area() -> float; }
impl Shape {
  view fn measured<U>(unit: view U) -> float { return self.area(); }
  static fn unit<U>(u: view U) -> int { return 1; }
}
class Square : Shape {
  side: float;
  view fn area() -> float { return self.side; }
  view fn measured(k: int) -> int { return k; }
  view fn unit() -> int { return 1; }
}
class Round : Shape { view fn area() -> float { return 1.0; } }
impl Round {
  fn measured() -> int { return 1; }
}
class Tri : Shape { view fn area() -> float { return 1.0; } }
impl Tri {
  fn measured<V>(v: view V) -> int { return 1; }
}
class One { n: int; }
impl One {
  view fn m<U>(u: view U) -> int { return 1; }
  view fn m() -> int { return 2; }
  view fn k() -> int { return 2; }
  view fn k<U>(u: view U) -> int { return 1; }
  view fn g<U>(u: view U) -> int { return 1; }
  view fn g<V>(v: view V) -> int { return 1; }
  static fn m<U>(u: view U) -> int { return 1; }
  static fn s<U>(u: view U) -> int { return 1; }
  static fn s() -> int { return 1; }
  static fn t() -> int { return 1; }
  static fn t<U>(u: view U) -> int { return 1; }
}
struct V { n: int; }
impl V {
  view fn m<U>(u: view U) -> int { return 1; }
  view fn m() -> int { return 2; }
  view fn toString<U>(u: view U) -> Str { return ""; }
}
class G<T> { v: T; view fn body() -> int { return 1; } }
impl G<T> {
  view fn body<U>(u: view U) -> int { return 1; }
  view fn twice<U>(u: view U) -> int { return 1; }
  view fn twice() -> int { return 1; }
}
fn main() -> int { let g = G<int>(1); return 0; }
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 13u) << r.Diagnostics;
  for (const char *want : {
           ":9:3: error: method 'measured' of 'Shape' is declared in an 'impl' "
           "block and cannot be overridden",
           ":14:3: error: class 'Round' already has a method 'measured' (from "
           "'Shape')",
           ":18:3: error: class 'Tri' already has a method 'measured' (from "
           "'Shape')",
           ":23:3: error: class 'One' already has a method 'm'",
           ":25:3: error: class 'One' already has a method 'k'",
           ":27:3: error: class 'One' already has a method 'g'",
           ":37:3: error: struct 'V' already has a method 'm'",
           ":38:3: error: struct 'V' already has a method 'toString'",
           ":28:10: error: class 'One' already has a method 'm'",
           ":30:10: error: class 'One' already has a method 's'",
           ":32:10: error: class 'One' already has a method 't'",
           ":42:3: error: class 'G<int>' already has a method 'body'",
           ":46:28: note: in instantiation of 'G<int>' requested here",
           ":44:3: error: class 'G<int>' already has a method 'twice'",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// No requirement is generic, so a generic method meets none.
TEST(GenericMethods, MeetNoTraitRequirement) {
  auto r = semaCheck(R"pkn(trait Mapper { view fn map(n: int) -> int; }
class Box<T> { v: T; }
impl Box<T> {
  view fn map<U>(u: view U) -> int { return 1; }
}
impl Mapper for Box<T> {}
class Q { x: int; }
impl Q { view fn map<U>(u: view U) -> int { return 1; } }
impl Mapper for Q {}
fn use<M: Mapper>(m: view M) -> int { return m.map(1); }
fn main() -> int { let b = Box<int>(1); return 0; }
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  for (const char *want : {
           ":9:1: error: method 'map' of 'Q' is generic and cannot meet the "
           "requirement 'map' of trait 'Mapper'",
           ":6:1: error: method 'map' of 'Box<int>' is generic and cannot meet "
           "the requirement 'map' of trait 'Mapper'",
           ":11:28: note: in instantiation of 'Box<int>' requested here",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// A body is checked once per instantiation, with the instantiation note;
// one never called is not checked; and a method that asks for itself with
// an ever-growing argument stops at the depth limit.
TEST(GenericMethods, ABodyIsCheckedPerInstantiation) {
  auto r = semaCheck(R"pkn(import ::io;
class Box<T> { v: T; }
impl Box<T> {
  view fn bad<U>(u: view U) -> int { y = U; let z = U(); return u.nope(); }
  view fn never<U>(u: view U) -> int { return u.nope(); }
  view fn nest<U>(u: view U) -> int { return self.nest<(U, U)>((u, u)); }
}
fn main() -> int {
  let b = Box<int>(1);
  io::println(b.bad("a"));
  io::println(b.nest<int>(1));
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  for (const char *want : {
           ":4:42: error: type parameter 'U' cannot be used as a value",
           ":10:15: note: in instantiation of 'Box<int>.bad<Str>' requested "
           "here",
           ":4:53: error: type parameter 'U' cannot be used as a value (a type "
           "parameter cannot be constructed)",
           ":4:65: error: no method 'nope' on type 'Str'",
           ":6:46: error: instantiating 'Box<",
           "exceeds the maximum instantiation depth (16)",
           ":6:46: note: in instantiation of 'Box<",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
  // `never` is called by nothing: its `u.nope()` is not checked.
  EXPECT_EQ(r.Diagnostics.find(":5:"), std::string::npos) << r.Diagnostics;
}

// A generic `static fn`, of a generic type and of one that is not: the
// owner's arguments after the owner's name, the function's own after its
// own.
TEST(GenericMethods, StaticFnsAreGenericToo) {
  auto r = semaCheck(R"pkn(import ::io;
class Box<T> { v: T; }
impl Box<T> {
  static fn of(v: T) -> Box<T> { return Box<T>(move v); }
  static fn withTag<U>(v: T, tag: U) -> (Box<T>, U) { return (Box<T>(move v), move tag); }
}
class Point { x: int; }
impl Point {
  static fn make<U>(v: U, f: (U) -> int) -> Point { return Point(f(move v)); }
  static fn zero() -> Point { return Point(0); }
  fn make2() -> int { return 1; }
  static fn make2<U>(u: view U) -> int { return 1; }
}
fn main() -> int {
  let a = Box<int>::withTag<Str>(7, "seven");
  let b = Box<int>::withTag(8, 2.5);
  let c = Box::withTag<Str>(7, "a");
  let d = Box<int>::of<Str>(1);
  let e = Box<int>::withTag<Str, int>(7, "a");
  let f = Point::make<int>(1, fn(n: int) => n);
  let g = Point::make("s", fn(s: Str) => s.len());
  let h = Point::zero<int>();
  let i = Point::make;
  let j = Point::make<int, int>(1, fn(n: int) => n);
  let k = Point::make(1, fn(s: Str) => s.len());
  let l = Point<int>::make(1, fn(n: int) => n);
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
  for (const char *want : {
           ":12:10: error: class 'Point' already has a method 'make2'",
           ":17:11: error: 'Box::withTag' is a function of the generic class "
           "'Box': write 'Box<...>::withTag(...)' with its type arguments",
           ":18:11: error: 'Box<int>::of' is not generic and takes no type "
           "arguments",
           ":19:11: error: generic function 'Box<int>::withTag' expects 1 type "
           "argument(s), got 2",
           ":22:11: error: 'Point::zero' is not generic and takes no type "
           "arguments",
           ":23:11: error: 'Point::make' is a generic function; a generic "
           "function cannot be a function value yet (wrap a call to it in a "
           "lambda)",
           ":24:11: error: generic function 'Point::make' expects 1 type "
           "argument(s), got 2",
           ":25:11: error: cannot infer type parameter 'U' of 'Point::make': "
           "deduced as both 'int' and 'Str'",
           ":25:11: note: specify the type arguments explicitly: "
           "'Point::make<...>(...)'",
           ":26:11: error: 'Point' is not a generic class and takes no type "
           "arguments",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// A private generic method or `static fn` is private by its template's
// name, on a type that is generic too.
TEST(GenericMethods, PrivateOnesAreTheirTypesOwn) {
  auto r = semaCheck(R"pkn(class C { n: int; }
impl C {
  view fn __secret<U>(u: view U) -> int { return self.n; }
  view fn open<U>(u: view U) -> int { return self.__secret(u); }
  static fn __hidden<U>(u: view U) -> int { return 1; }
  static fn shown<U>(u: view U) -> int { return C::__hidden(u); }
}
class Box<T> { v: T; }
impl Box<T> {
  static fn __mk<U>(u: view U) -> int { return 42; }
  static fn __plain() -> int { return 41; }
  static fn open<U>(u: view U) -> int { return Box<T>::__mk(u) + Box<T>::__plain(); }
}
fn main() -> int {
  let c = C(1);
  let a = c.open(1) + C::shown(2);
  let b = c.__secret(1);
  let d = C::__hidden(1);
  let e = Box<int>::open(1);
  let f = Box<int>::__mk<int>(1);
  let g = Box<int>::__mk(1);
  let h = Box<int>::__plain();
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 5u) << r.Diagnostics;
  for (const char *want : {
           ":17:11: error: '__secret' is private to 'C': it is reachable only "
           "from its own methods",
           ":18:11: error: '__hidden' is private to 'C': it is reachable only "
           "from its own methods",
           ":20:11: error: '__mk' is private to 'Box<int>': it is reachable "
           "only from its own methods",
           ":21:11: error: '__mk' is private to 'Box<int>': it is reachable "
           "only from its own methods",
           ":22:11: error: '__plain' is private to 'Box<int>': it is reachable "
           "only from its own methods",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// The archetype check makes each generic member once with a move-only
// type for each parameter without `Copyable`: its own and its owner's, also
// where two bounded blocks give one name.
TEST(GenericMethods, TheArchetypeCheckReadsTheirCode) {
  auto r = semaCheck(R"pkn(class Box<T> { v: T; }
impl Box<T> {
  fn __init__(v: T) { self.v = move v; }
  view fn twice<U>(u: U) -> (U, U) { return (u, u); }
  view fn both<U>(u: U) -> (T, U) { return (self.v, move u); }
  view fn ok<U: Copyable>(u: U) -> (U, U) { return (u, u); }
  static fn dup<U>(u: U) -> (U, U) { return (u, u); }
}
class P { x: int; }
impl P {
  view fn twice<U>(u: U) -> (U, U) { return (u, u); }
  static fn dup<U>(u: U) -> (U, U) { return (u, u); }
  view fn fine<U: Copyable>(u: U) -> (U, U) { return (u, u); }
  async fn send<U>(u: U) -> int { return 1; }
}
struct S<T: Copyable> { v: T; }
impl S<T> {
  view fn pair<U>(u: U) -> (T, U) { return (self.v, move u); }
}
class Two<T> { v: T; }
impl Two<T> { view fn dupT() -> (T, T) { return (self.v, self.v); } }
impl Two<T: Hashable> { view fn which<U>(u: U) -> (U, U) { return (u, u); } }
impl Two<T: Cloneable> { view fn which<U>(u: U) -> int { return 2; } }
fn main() -> int { return 0; }
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 13u) << r.Diagnostics;
  for (const char *want : {
           ":4:46: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
           "it",
           ":4:49: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
           "it",
           ":5:45: error: 'T' is not 'Copyable': declare 'T: Copyable' to copy "
           "it",
           ":7:46: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
           "it",
           ":7:49: error: 'U' is not 'Copyable': declare 'U: Copyable' to copy "
           "it",
           ":11:46: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
           ":11:49: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
           ":12:46: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
           ":12:49: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
           ":21:50: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
           "copy it, or put the method in 'impl Two<T: Copyable>'",
           ":21:58: error: 'T' is not 'Copyable': declare 'T: Copyable' to "
           "copy it, or put the method in 'impl Two<T: Copyable>'",
           ":22:68: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
           ":22:71: error: 'U' is not 'Copyable': declare 'U: Copyable' to "
           "copy it",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
  // A parameter bounded `Copyable` copies; so does the `T: Copyable` of the
  // struct.
  for (const char *line : {":6:", ":13:", ":18:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << r.Diagnostics;
}

// A generic member of a bounded block is on the instances that meet the
// block's bounds: two blocks may give one name under different bounds, and
// a name an instance gets twice is a clash there, wherever the instance is
// first named.
TEST(GenericMethods, ABoundedBlocksAreOnTheInstancesThatMeetIt) {
  auto r = semaCheck(R"pkn(import ::io;
class Box<T> { v: T; }
impl Box<T: Copyable> {
  view fn show<U: Showable>(u: view U) -> Str { return u.toString(); }
  static fn tag<U: Showable>(u: view U) -> Str { return u.toString(); }
}
// The same name under two bounds: an instance has the one it meets.
class Two<T> { v: T; }
impl Two<T: Copyable> {
  view fn which<U: Showable>(u: view U) -> Str { return "copyable " + u.toString(); }
}
impl Two<T: Hashable> {
  view fn which<U: Showable>(u: view U) -> Str { return "hashable " + u.toString(); }
}
// A plain method and a bounded block's generic one of its name: a clash on
// the instances that meet the bound, wherever the instance is first named.
class Bag<T> { v: T; }
impl Bag<T> {
  view fn dup() -> Str { return "plain"; }
}
impl Bag<T: Cloneable> {
  view fn dup<U: Showable>(u: view U) -> Str { return "generic"; }
}
class Item { n: int; }
impl Cloneable for Item { view fn clone() -> Self { return Item(self.n); } }
class Opaque { n: int; }
class Holder { b: Bag<Item>; o: Bag<Opaque>; }
fn main() -> int {
  let a = Box<int>(1);
  io::println(a.show(2.5) + Box<int>::tag(1));
  let b = Box<Str>("s");
  io::println(b.show(1));
  io::println(Box<Str>::tag(1));
  let f = Two<float>(1.5);
  let s = Two<Str>("s");
  io::println(f.which("x") + s.which("y"));
  let both = Two<int>(1);
  let h = Holder(Bag<Item>(Item(1)), Bag<Opaque>(Opaque(2)));
  io::println(h.o.dup());
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  for (const char *want : {
           ":22:3: error: class 'Bag<Item>' already has a method 'dup'",
           ":27:16: note: in instantiation of 'Bag<Item>' requested here",
           ":32:15: error: no method 'show' on type 'Box<Str>'",
           ":33:15: error: 'Box<Str>' has no static function 'tag'",
           ":13:3: error: class 'Two<int>' already has a method 'which'",
           ":37:14: note: in instantiation of 'Two<int>' requested here",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
  // `Two<float>` and `Two<Str>` meet one bound each, and `Bag<Opaque>`
  // none: no clash there, and no call on them is an error.
  for (const char *line : {":30:", ":36:", ":39:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << r.Diagnostics;
}

// The instantiations of a generic method are one method: the `view fn`
// suggestion comes once, by its template's name, and only when no
// instantiation changes `self`.
TEST(GenericMethods, TheViewFnSuggestionIsOnePerMethod) {
  auto r = semaCheck(R"pkn(import ::io;
class C { n: int; }
impl C {
  fn a<U>(u: view U) -> int { return self.n; }
  fn b<U>(u: view U) -> int { return self.a(u); }
  fn c<U>(u: view U) -> int { self.n = self.n + 1; return self.n; }
  fn d<U>(u: view U) -> int { return self.c(u); }
}
class Box<T> { v: T; }
impl Box<T> {
  fn size<U>(u: view U) -> int { return 1; }
}
fn main() -> int {
  x: mut = C(0);
  io::println(x.a(1) + x.a("s") + x.b(2.5) + x.c(1) + x.d(1));
  b: mut = Box<int>(1); c: mut = Box<Str>("a");
  io::println(b.size(1) + c.size("x") + c.size(2));
  return 0;
}
)pkn");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
  for (const char *want : {
           ":11:3: warning: 'size' never changes 'self': make it a 'view fn' "
           "so 'view' parameters can call it",
           ":4:3: warning: 'a' never changes 'self': make it a 'view fn' so "
           "'view' parameters can call it",
           ":5:3: warning: 'b' never changes 'self': make it a 'view fn' so "
           "'view' parameters can call it",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
  // `c` changes `self` and `d` calls it: no suggestion for either.
  for (const char *line : {":6:3:", ":7:3:"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << r.Diagnostics;
}

// A lambda in a generic method captures as in any method: `self` in a
// `shared fn` of a class, never a `view fn`'s or a struct's.
TEST(GenericMethods, ALambdaCapturesAsInAnyMethod) {
  auto r = semaCheck(R"pkn(import ::io;
class Counter { n: int; }
impl Counter {
  shared fn each<U>(xs: view U[], f: (view U) -> int) -> int {
    let add = fn(k: int) { self.n = self.n + k; };
    for i in 0..xs.len() { add(f(xs[i])); }
    return self.n;
  }
  view fn peek<U>(u: view U) -> int {
    let get = fn() => self.n;
    return get();
  }
}
struct V { x: int; }
impl V {
  view fn peek<U>(u: view U) -> int {
    let get = fn() => self.x;
    return get();
  }
}
fn main() -> int {
  c: shared mut = Counter(0);
  io::println(c.each([1, 2, 3], fn(n: view int) => n * 2));
  io::println(c.peek(1));
  let v = V(1);
  io::println(v.peek(1));
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  for (const char *want : {
           ":10:23: error: 'self' is read-only in 'view fn peek<int>'; a "
           "function cannot capture it (its copy could change the object)",
           ":24:15: note: in instantiation of 'Counter.peek<int>' requested "
           "here",
           ":17:23: error: 'self' is the receiver's storage in a method of "
           "struct 'V'; a function cannot capture it (copy it into a variable "
           "first)",
           ":26:15: note: in instantiation of 'V.peek<int>' requested here",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}

// A generic method is inherited, sits next to a field of function type of
// another name, infers from a lambda's result and from the type its call
// flows into (a slot, the arm before it, the operand to its left; through
// a receiver that is itself a call), bounds its own parameters by its
// type's, and takes a module-private type as an argument.  A `static fn`
// named like a builtin method is no method.
TEST(GenericMethods, AreInheritedInferredAndBounded) {
  auto r = semaCheck(R"pkn(import ::io;
class Shape { view fn area() -> float; }
impl Shape {
  view fn measured<U>(unit: view U, show: (float, view U) -> Str) -> Str {
    return show(self.area(), unit);
  }
  view fn none<U>() -> U? { return None; }
  view fn me() -> view Shape { return self; }
  static fn empty<U>() -> U? { return None; }
}
class Square : Shape {
  side: float;
  view fn area() -> float { return self.side * self.side; }
  view fn twice() -> Str { return self.measured(2, fn(a: float, n: view int) => Str(a * float(n))); }
}
class Holder { map: (int) -> int; n: int; }
impl Holder {
  view fn apply<U>(u: U, f: (U) -> int) -> int { return f(move u) + self.n; }
}
struct Table<V> { v: V; }
impl Table<V> {
  view fn slot<K: Hashable>(k: view K) -> int { return k.hash() % 8; }
  fn put<K: Hashable>(k: view K, v: V) -> int { self.v = move v; return k.hash() % 8; }
  view fn peek<U>(u: view U) -> view V { return self.v; }
}
// A bound of a method's own parameter may name its type's.
trait Keyed<Q> { view fn key() -> Q; }
class It { n: int; view fn key() -> int { return self.n; } }
impl Keyed<int> for It {}
class Slot<T: Copyable> { v: T; }
impl Slot<T> {
  view fn of<U: Keyed<T>>(u: view U) -> T { return u.key(); }
  static fn from<U: Keyed<T>>(u: view U) -> T { return u.key(); }
}
// The block may call the type's parameters by other names than the type does.
trait Conv<X> { view fn conv() -> X; }
class N { n: int; }
impl Conv<int> for N { view fn conv() -> int { return self.n; } }
class M<K, V: Conv<K>> { k: K; v: V; }
impl M<A, B> {
  view fn show<U: Showable>(u: view U) -> Str { return u.toString() + Str(self.v.conv()); }
}
class Wide<T> { v: T; }
impl Wide<T> { static fn empty<U>() -> U? { return None; } }
fn gone(s: Square) -> Str { return Str(s.side); }
// A `static fn` named like a builtin method is no method.
struct Copy { n: int; }
impl Copy {
  static fn clone<U>(u: U) -> U { return move u; }
  static fn toString<U>(u: view U) -> int { return 1; }
}
struct CopyOf<T> { n: T; }
impl CopyOf<T> { static fn toString<U>(u: view U) -> int { return 2; } }
// A type private to the module is a type argument like any.
struct _Loc { n: int; }
// A struct's own generic `clone` is the one called.
struct Once { n: int; }
impl Once { view fn clone<U>() -> int { return 7; } }
fn first() -> int { return 0; }
fn main() -> int {
  let s = Square(1.5);
  io::println(s.measured("m2", fn(a: float, u: view Str) => Str(a) + " " + u));
  io::println(s.twice());
  // From the type the call flows into: a slot, a receiver that is a chain,
  // an arm, the other operand.
  let n: int? = s.none();
  let m = s.none<Str>();
  let viaCall: int? = s.me().none();
  let squares = [Square(1.0), Square(2.0)];
  let viaIndex: int? = squares[first()].none();
  let x: int? = 3;
  let c = True;
  let arm1: int? = if c then s.none() else 3;
  let arm3 = if c then x else s.none();
  let eq1 = x == s.none();
  let st1: int? = if c then Shape::empty() else 3;
  let st2 = if c then Shape::empty() else x;
  let st3 = x == Shape::empty();
  let st4 = Shape::empty() == x;
  let st5 = Wide<int>::empty() == x;
  let st6 = if c then Wide<int>::empty() else x;
  // An arm that moves what the other arm's method call reads.
  let spare = Square(3.0);
  let area = if c then s.measured(spare, fn(a: float, o: view Square) => Str(a)) else gone(move spare);
  let h = Holder(fn(n: int) => n + 1, 10);
  io::println(h.map(1) + h.apply("abc", fn(s: Str) => s.len()));
  io::println(h.apply<_Loc>(_Loc(41), fn(l: _Loc) => l.n));
  t = Table<Str>("a");
  io::println(t.slot(1) + t.slot("k") + t.put<int>(2, "b") + t.peek(1.5).len());
  let slot = Slot<int>(1);
  io::println(slot.of<It>(It(4)) + slot.of(It(5)) + Slot<int>::from<It>(It(6)));
  let once = Once(1);
  io::println(once.clone<int>());
  let mm = M<int, N>(1, N(2));
  io::println(mm.show(3));
  let copy = Copy(1);
  io::println(copy.clone().n + Copy::clone<int>(2) + Copy::toString(1) + CopyOf<int>::toString(1));
  io::println(copy.toString() + CopyOf<int>(3).toString());
  return 0;
}
)pkn");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// The type a call flows into is that call's: not its receiver's, and not
// one the left operand of `==` or the first arm has when it is typed.
TEST(GenericMethods, TheExpectedTypeIsTheCallsOwn) {
  auto r = semaCheck(R"pkn(import ::io;
class K { n: int; }
impl K {
  view fn none<U>() -> U? { return None; }
}
struct Maker { n: int; }
struct Outer<T> { m: Maker; v: T?; }
impl Maker { view fn again() -> Outer<int> { return Outer<int>(Maker(2), 7); } }
fn outer<T>() -> Outer<T> { return Outer<T>(Maker(1), None); }
fn main() -> int {
  let k = K(1);
  let x: int? = 3;
  // The slot is the outer call's, not its receiver's.
  let s: Str = k.none().toString();
  let w: Outer<int> = outer().m.again();
  // The left of `==` is typed first, and so is the first arm.
  let e = k.none() == x;
  let c = True;
  let a = if c then k.none() else x;
  return 0;
}
)pkn");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
  for (const char *want : {
           ":14:16: error: cannot infer type parameter 'U' of 'K.none' from "
           "the call arguments",
           ":14:16: note: specify the type arguments explicitly: "
           "'none<...>(...)'",
           ":15:23: error: cannot infer type parameter 'T' of 'outer' from the "
           "call arguments",
           ":15:23: note: specify the type arguments explicitly: "
           "'outer<...>(...)'",
           ":17:11: error: cannot infer type parameter 'U' of 'K.none' from "
           "the call arguments",
           ":17:11: note: specify the type arguments explicitly: "
           "'none<...>(...)'",
           ":19:21: error: cannot infer type parameter 'U' of 'K.none' from "
           "the call arguments",
           ":19:21: note: specify the type arguments explicitly: "
           "'none<...>(...)'",
       })
    EXPECT_NE(r.Diagnostics.find(want), std::string::npos) << want << "\n"
                                                           << r.Diagnostics;
}
