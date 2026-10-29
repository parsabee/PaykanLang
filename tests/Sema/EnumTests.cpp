// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests for the `enum` type: declaration, variant access, equality rules,
// non-convertibility, and enum-subject match.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string wrapMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
}
static std::string withEnums(const std::string &enums,
                             const std::string &body) {
  return enums + "\n" + wrapMain(body);
}

// --- Declaration & basic use ------------------------------------------------

TEST(Enum, DeclareAndUse) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green, Blue }", "c: Color = Color::Green;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, TrailingCommaAllowed) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green, Blue, }", "c: Color = Color::Blue;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, DuplicateVariantRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Red }", "return 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, DuplicateEnumNameRejected) {
  auto r = semaCheck(
      withEnums("enum Color { Red }\nenum Color { Blue }", "return 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, NameClashesWithClassRejected) {
  auto r =
      semaCheck("class Color {}\nenum Color { Red }\n" + wrapMain("return 0;"));
  EXPECT_FALSE(r.Ok);
}

// Enums share the top-level namespace with the builtins.
TEST(Enum, NameShadowsBuiltinClassRejected) {
  auto r = semaCheck(withEnums("enum Str { Short }", "return 0;"));
  EXPECT_FALSE(r.Ok);
  EXPECT_NE(
      r.Diagnostics.find("'Str' is a builtin class and cannot be redeclared"),
      std::string::npos)
      << r.Diagnostics;
}

// The standard library's names are not reserved: a program's own `Error`
// and `open` live beside err::Error and io::open.
TEST(Enum, TheStandardLibrarysNamesAreNotReserved) {
  auto r = semaCheck("enum Error { NotFound }\nenum open { Read }\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// --- Variant access ---------------------------------------------------------

TEST(Enum, UnknownVariantRejected) {
  auto r = semaCheck(
      withEnums("enum Color { Red, Green }", "c: Color = Color::Nope;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, UnknownEnumNameRejected) {
  auto r = semaCheck(wrapMain("c: Nope = Nope::X;"));
  EXPECT_FALSE(r.Ok);
}

// --- Equality / non-convertibility ------------------------------------------

TEST(Enum, EqualitySameEnumOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if Color::Red == Color::Green { }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, InequalitySameEnumOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if Color::Red != Color::Green { }"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, CompareDifferentEnumsRejected) {
  auto r = semaCheck("enum A { X, Y }\nenum B { P, Q }\n" +
                     wrapMain("if A::X == B::P { }"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, CompareEnumWithIntRejected) {
  auto r = semaCheck(withEnums("enum Color { Red }", "if Color::Red == 0 { }"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, AssignEnumToIntRejected) {
  auto r = semaCheck(withEnums("enum Color { Red }", "x: int = Color::Red;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, AssignIntToEnumRejected) {
  auto r = semaCheck(withEnums("enum Color { Red }", "c: Color = 0;"));
  EXPECT_FALSE(r.Ok);
}

TEST(Enum, OrderingOperatorRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }",
                               "if Color::Red < Color::Green { }"));
  EXPECT_FALSE(r.Ok);
}

// --- Enum match -------------------------------------------------------------

TEST(Enum, MatchBareVariantsOk) {
  auto r = semaCheck(withEnums("enum Color { Red, Green, Blue }", R"(
    c: Color = Color::Red;
    match c {
      Red { }
      Green { }
      Blue { }
      _ { }
    }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, MatchUnknownVariantRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }", R"(
    c: Color = Color::Red;
    match c {
      Red { }
      Nope { }
      _ { }
    }
  )"));
  EXPECT_FALSE(r.Ok);
}

// Finding 50: an arm whose type is no name (`int`, a tuple, an optional, an
// array) names it in the error, which was "'' is not a variant of enum 'E'",
// and says what an arm names; a name keeps its message.
TEST(Enum, MatchArmOfANonNameTypeNamesIt) {
  auto r = semaCheck(R"(enum E { A(x: int), B }
fn main() -> int {
  e = E::A(1);
  match e {
    a: int { }
    t: (int, Str) { }
    o: E? { }
    k: int[3] { }
    Q { }
    _ { }
  }
  return 0;
}
)");
  EXPECT_FALSE(r.Ok);
  for (const char *d :
       {"5:5: error: 'int' is not a variant of enum 'E': an arm names one of "
        "its variants, such as 'A'",
        "6:5: error: '(int, Str)' is not a variant of enum 'E': an arm names "
        "one of its variants, such as 'A'",
        "7:5: error: 'E?' is not a variant of enum 'E': an arm names one of "
        "its variants, such as 'A'",
        "8:5: error: 'int[3]' is not a variant of enum 'E': an arm names one "
        "of its variants, such as 'A'",
        "9:5: error: 'Q' is not a variant of enum 'E'\n"})
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("''"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 5) << r.Diagnostics;
}

TEST(Enum, MatchWildcardNotLastRejected) {
  auto r = semaCheck(withEnums("enum Color { Red, Green }", R"(
    c: Color = Color::Red;
    match c {
      _ { }
      Red { }
    }
  )"));
  EXPECT_FALSE(r.Ok);
}

// A wildcard-less enum match covering every variant (all arms returning)
// satisfies the non-void return requirement — no trailing return needed.
TEST(Enum, ExhaustiveMatchSatisfiesReturn) {
  auto r = semaCheck("enum Color { Red, Green, Blue }\n"
                     "fn pick(c: Color) -> Str {\n"
                     "  match c {\n"
                     "    Red   { return \"r\"; }\n"
                     "    Green { return \"g\"; }\n"
                     "    Blue  { return \"b\"; }\n"
                     "  }\n"
                     "}\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A wildcard-less enum match that misses a variant is NOT exhaustive, so the
// non-void function can fall through without returning -> error.
TEST(Enum, NonExhaustiveMatchDoesNotSatisfyReturn) {
  auto r = semaCheck("enum Color { Red, Green, Blue }\n"
                     "fn pick(c: Color) -> Str {\n"
                     "  match c {\n"
                     "    Red   { return \"r\"; }\n"
                     "    Green { return \"g\"; }\n"
                     "  }\n"
                     "}\n"
                     "fn main() -> int { return 0; }\n");
  EXPECT_FALSE(r.Ok);
}

// --- Variants with fields (a tagged union) ----------------------------------

static const char *kTagged = "class Node { n: int; }\n"
                             "enum E { A, B(x: int, s: Str), C(node: Node?) }";

/// @p body checked in a program declaring kTagged: one error, holding
/// @p message.
static void expectTaggedError(const std::string &body,
                              const std::string &message) {
  auto r = semaCheck(withEnums(kTagged, body));
  EXPECT_FALSE(r.Ok) << body;
  EXPECT_EQ(r.ErrorCount, 1u) << body << "\n" << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(message), std::string::npos) << body << "\n"
                                                            << r.Diagnostics;
}

TEST(Enum, VariantsMadeAndMatched) {
  auto r = semaCheck(withEnums(kTagged, R"(
    e = E::B(1, "one");
    f: E = E::A;
    g = E::C(None);
    n = 0;
    match e {
      A { n = 1; }
      b: B { n = b.x + b.s.len(); }
      c: view C { n = 3; }
    }
    match f {
      b: inout B { b.x += 1; }
      _ { io::println(f.toString()); }
    }
    io::println(e);
    same = e == f || e != g;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// An `inout` arm matches a call's `inout` result as it does a variable or a
// field: a method's (of a class, through a field, of a struct) and a
// function's.  What the call borrowed stays borrowed while the binding is
// live.  A call with a plain or a `view` result is no storage.  (Each
// `inout` result was `'inout' arm 'b' needs a variable or a field to match
// on`.)
TEST(Enum, AnInoutArmMatchesAnInoutResult) {
  const std::string decls = std::string(kTagged) + R"(
class K {
  e: E;
  fn slot() -> inout E { return self.e; }
  view fn peek() -> view E { return self.e; }
}
class H { k: K; }
struct S { e: E; }
impl S {
  fn slot() -> inout E { return self.e; }
}
fn pick(x: inout E, y: inout E, first: bool) -> inout E {
  return if first then x else y;
}
fn mk() -> E { return E::A; }
)";
  auto ok = semaCheck(decls + wrapMain(R"(
    k = K(E::B(1, "a"));
    match k.slot() { b: inout B { b.x += 100; b.s = b.s + "!"; } _ { } }
    h = H(K(E::A));
    match h.k.slot() { b: inout B { b.x = 2; } _ { } }
    s = S(E::B(3, "s"));
    match s.slot() { b: inout B { b.x *= 2; } A { } C { } }
    x = E::B(4, "x");
    y: E = E::A;
    match pick(x, y, True) { b: inout B { b.x = 7; } _ { } }
    match k.slot() { b: inout B { b.x += 1; } _ { } }
    io::println(k.e);
  )"));
  EXPECT_TRUE(ok.Ok) << ok.Diagnostics;
  auto r = semaCheck(decls + wrapMain(R"(
    k = K(E::B(1, "a"));
    match k.slot() { b: inout B { io::println(k.e); b.x = 1; } _ { } }
    match mk() { b: inout B { } _ { } }
    match k.peek() { b: inout B { } _ { } }
    x = E::B(4, "x");
    y: E = E::A;
    match pick(x, y, True) { b: inout B { y = E::A; b.x = 2; } _ { } }
  )"));
  for (
      const char *diag :
      {":20:47: error: 'k' is borrowed by 'inout' local 'b' until 'b' is "
       "last used",
       ":21:11: error: 'inout' arm 'b' needs a variable or a field to match on",
       ":22:11: error: 'inout' arm 'b' needs a variable or a field to match on",
       ":25:43: error: 'y' is borrowed by 'inout' local 'b' until 'b' is "
       "last used"})
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

// A variant's fields name a type, `E::B`, which a match arm's binding has.
TEST(Enum, VariantFieldsNameAType) {
  auto r = semaCheck(std::string(kTagged) + R"(
fn size(b: E::B) -> int { return b.x + b.s.len(); }
fn main() -> int {
  match E::B(2, "ab") {
    b: B { return size(b); }
    _ { return 0; }
  }
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A tagged union in a struct, a tuple, an optional, an array and a generic
// container; an enum without fields shown by name.
TEST(Enum, TaggedUnionIsAValue) {
  auto r = semaCheck("import ::collections as col;\n" + withEnums(kTagged, R"(
    t = (E::A, 1);
    o: E? = E::B(1, "s");
    xs = [E::A; 2];
    l = col::List<E>();
    l.push(xs[0]);
    io::println(t);
    io::println(o);
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto plain = semaCheck(withEnums("enum Dir { North, South }",
                                   "io::println(Dir::South.toString());"));
  EXPECT_TRUE(plain.Ok) << plain.Diagnostics;
}

TEST(Enum, VariantCallsChecked) {
  expectTaggedError("e = E::B;",
                    "variant 'B' of enum 'E' has fields: write 'E::B(x, s)'");
  expectTaggedError(
      "e = E::A(1);",
      "variant 'A' of enum 'E' has no fields: write 'E::A' without arguments");
  expectTaggedError("e = E::B(1);", "variant 'B' of enum 'E' has 2 field(s) "
                                    "(x: int, s: Str), got 1 argument(s)");
  expectTaggedError("e = E::B(\"a\", \"b\");",
                    "field 'x' of 'E::B' has type 'int', got 'Str'");
  expectTaggedError("e = E::D(1);", "enum 'E' has no variant 'D'");
}

TEST(Enum, VariantBindingsChecked) {
  expectTaggedError("e = E::A; match e { a: A { } _ { } }",
                    "variant 'A' of enum 'E' has no fields to bind: write the "
                    "arm 'A { ... }'");
  expectTaggedError("e = E::A; match e { b: B { n = b.z; } _ { } }",
                    "no field 'z' in variant 'B' of enum 'E'");
  expectTaggedError("e = E::A; b: E::B = e;",
                    "initializer of type 'E' does not match declared type "
                    "'E::B' for variable 'b'");
}

// A tagged union is held in place: its variants cannot hold it, directly or
// through a struct.
TEST(Enum, TaggedUnionCannotHoldItself) {
  auto r = semaCheck("enum L { Nil, Cons(head: int, tail: L) }\n" +
                     wrapMain("return 0;"));
  EXPECT_NE(r.Diagnostics.find("enum 'L' contains itself: variant 'Cons' "
                               "holds an 'L'"),
            std::string::npos)
      << r.Diagnostics;
  r = semaCheck("struct S { l: L; }\nenum L { Nil, Cons(s: S) }\n" +
                wrapMain("return 0;"));
  EXPECT_NE(r.Diagnostics.find("enum 'L' contains itself: variant 'Cons' "
                               "holds it through 'S'"),
            std::string::npos)
      << r.Diagnostics;
  // Once for each variant that holds it, however many times it does.
  r = semaCheck("enum Expr { Num(v: int), Add(l: Expr, r: Expr) }\n" +
                wrapMain("return 0;"));
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("enum 'Expr' contains itself: variant 'Add' "
                               "holds an 'Expr'"),
            std::string::npos)
      << r.Diagnostics;
  r = semaCheck("enum E { A(x: int), B(y: E), C(z: (E, E)) }\n" +
                wrapMain("return 0;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("variant 'B' holds an 'E'"), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("variant 'C' holds an 'E'"), std::string::npos)
      << r.Diagnostics;
  // So through instantiations of a generic struct, whose fields are resolved
  // as they come.
  r = semaCheck("struct Box<T> { v: T; }\n"
                "enum E { N, A(x: Box<E>, y: Box<E>), B(z: (Box<E>, int)) }\n" +
                wrapMain("return 0;"));
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("variant 'A' holds it in field 'x'"),
            std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("variant 'B' holds it in field 'z'"),
            std::string::npos)
      << r.Diagnostics;
  // Through a class it is a reference, which is fine.
  r = semaCheck("class Box { l: L; }\nenum L { Nil, Cons(b: Box) }\n" +
                wrapMain("return 0;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

TEST(Enum, VariantFieldsChecked) {
  auto r = semaCheck("enum E { B(x: int, x: int) }\n" + wrapMain("return 0;"));
  EXPECT_NE(
      r.Diagnostics.find("duplicate field 'x' in variant 'B' of enum 'E'"),
      std::string::npos)
      << r.Diagnostics;
  r = semaCheck("enum E { B(x: Nope) }\n" + wrapMain("return 0;"));
  EXPECT_FALSE(r.Ok);
  // A view is never stored in a variant.
  r = semaCheck(std::string(kTagged) +
                "\nfn f(s: view Str) -> E { return E::B(1, s); }\n" +
                wrapMain("return 0;"));
  EXPECT_NE(r.Diagnostics.find("parameter 2 of 'E::B' is not one"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Enum, TaggedUnionComparisonsAndConversions) {
  expectTaggedError("e = E::A; if e < E::A { }",
                    "operator '<' is not defined for types 'E' and 'E'");
  expectTaggedError("e = E::A; x: int = e;",
                    "initializer of type 'E' does not match declared type "
                    "'int' for variable 'x'");
}

// An enum whose variants have fields does not cross into C.
TEST(Enum, TaggedUnionIsNotNative) {
  auto r = semaCheck(std::string(kTagged) +
                     "\nnative fn n(e: view E) = \"pk_n\";\n" +
                     wrapMain("return 0;"));
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("parameter 'e' of type 'E' cannot cross into "
                               "C (int, float, bool, char, a sized numeric "
                               "type, an enum without fields,"),
            std::string::npos)
      << r.Diagnostics;
}

// --- Generic enums ----------------------------------------------------------

static const char *kGeneric =
    "enum Maybe<T> { Just(value: T), Nothing }\n"
    "enum Result<T, E> { Ok(value: T), Err(error: E) }";

/// @p body checked in a program declaring kGeneric: one error, holding
/// @p message.
static void expectGenericError(const std::string &body,
                               const std::string &message) {
  auto r = semaCheck(withEnums(kGeneric, body));
  EXPECT_FALSE(r.Ok) << body;
  EXPECT_EQ(r.ErrorCount, 1u) << body << "\n" << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(message), std::string::npos) << body << "\n"
                                                            << r.Diagnostics;
}

// A variant's type arguments are written, taken from the type its value
// flows into, or inferred from its fields.
TEST(Enum, GenericVariantsInstantiate) {
  auto r = semaCheck(withEnums(kGeneric, R"(
    a = Maybe::Just(3);
    b: Maybe<int> = Maybe::Nothing;
    c = Maybe<Str>::Nothing;
    d: Maybe<u8> = Maybe::Just(200);
    e: Result<int, Str> = Result::Err("bad");
    f = Result<int, Str>::Ok(1);
    a = b;
    a = Maybe::Nothing;
    xs: Maybe<Str>[2] = [Maybe::Just("x"), Maybe::Nothing];
    o: Maybe<int>? = Maybe::Nothing;
    same = a == Maybe::Nothing;
    match a { j: Just { n: int = j.value; } Nothing { } }
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Through a generic function's parameters and result, and a generic
// struct's fields.
TEST(Enum, GenericEnumInGenericCode) {
  auto r = semaCheck(std::string(kGeneric) + R"(
fn unwrapOr<T>(m: Maybe<T>, d: T) -> T {
  match m { j: Just { return j.value; } Nothing { return d; } }
}
fn wrap<T>(x: T) -> Maybe<T> => Maybe::Just(x);
fn none<T>() -> Maybe<T> { return Maybe::Nothing; }
struct Cell<T> { m: Maybe<T>; }
)" + wrapMain(R"(
    n: int = unwrapOr(Maybe::Just(1), 0);
    s: Str = unwrapOr(wrap("a"), "b");
    f: float = unwrapOr(none<float>(), 1.5);
    c = Cell(Maybe::Just(2.5));
    m: Maybe<float> = c.m;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A variant without type arguments takes them from the other side of `==`
// or `!=`, or the other arm of a conditional, whichever side it is on: the
// side that needs the other less is typed first.  `Maybe::Nothing == x` and
// `if c then Maybe::Nothing else Maybe::Just(n)` were `cannot infer the
// type arguments`, though their mirrors compiled.
TEST(Enum, GenericVariantsTakeTheOtherSideEitherWay) {
  auto r = semaCheck(withEnums(kGeneric, R"(
    x = Maybe::Just(5);
    n = 2;
    a: bool = Maybe::Nothing == x;
    b: bool = x == Maybe::Nothing;
    c: bool = Maybe::Nothing != x;
    d: bool = Maybe::Nothing != Maybe::Just(1);
    e = if n > 0 then Maybe::Nothing else Maybe::Just(n);
    f = if n > 0 then Maybe::Just(n) else Maybe::Nothing;
    g = if n > 5 then Maybe::Nothing else if n > 1 then Maybe::Nothing
        else Maybe::Just(n);
    h: bool = Maybe::Nothing == (if n > 1 then x else Maybe::Nothing);
    i: bool = x != (if n > 1 then Maybe::Nothing else Maybe::Nothing);
    k: Maybe<int> = e;
    k = f;
    k = g;
    r = Result<int, Str>::Ok(1);
    o: bool = Result::Ok(1) == r;
    p: bool = Result::Err("no") != r;
    q = if n > 0 then Result::Err("no") else r;
    small: Maybe<u8> = Maybe::Just(200);
    s: bool = Maybe::Just(200) == small;
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// A generic function's call whose parameters leave a type parameter open
// (`none()`, `ok(1)`) needs the other side's type, and so does a variant
// whose fields leave one open (`Pair::Left(1)`) or whose field needs it
// (`Maybe::Just(Maybe::Nothing)`): the side that needs it less is typed
// first, whichever it is, and a variant made with fields that can infer
// them stays first against such a call.
TEST(Enum, GenericCallsAndOpenVariantsTakeTheOtherSideEitherWay) {
  auto r = semaCheck(withEnums(std::string(kGeneric) + R"(
    enum Pair<A, B> { Both(a: A, b: B), Left(a: A), Neither }
    fn none<T>() -> Maybe<T> => Maybe::Nothing;
    fn ok<T, E>(v: T) -> Result<T, E> => Result::Ok(v);)",
                               R"(
    n = 2;
    a: bool = Maybe::Just(1) == none();
    b: bool = none() == Maybe::Just(1);
    c: bool = Maybe::Just(1) != none();
    d = if n > 0 then Maybe::Just(2) else none();
    e = if n > 0 then none() else Maybe::Just(2);
    f = if n > 0 then Maybe::Just(2) else (if n > 1 then none() else none());
    m: Maybe<int> = d;
    m = e;
    m = f;
    r = Result<int, Str>::Ok(1);
    g: bool = ok(1) == r;
    h: bool = r == ok(1);
    i = if n > 0 then ok(1) else r;
    j = if n > 0 then r else ok(1);
    k: Result<int, Str> = i;
    k = j;
    l: bool = Pair::Left(1) == Pair::Both(1, "s");
    o = if n > 0 then Pair::Left(1) else Pair::Both(1, "s");
    p: Pair<int, Str> = o;
    q: bool = Maybe::Just(Maybe::Nothing) == Maybe::Just(Maybe::Just(1));
    s = if n > 0 then Maybe::Just(Maybe::Nothing) else Maybe::Just(Maybe::Just(1));
    t: Maybe<Maybe<int>> = s;
    u: bool = Maybe::Just(none()) == Maybe::Just(Maybe::Just(1));
  )"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_EQ(r.ErrorCount, 0u) << r.Diagnostics;
}

// With nothing to take them from on either side, they are still an error.
TEST(Enum, GenericVariantsWithNoSideToTakeFrom) {
  const std::string message =
      "cannot infer the type arguments of generic enum 'Maybe' for "
      "'Maybe::Nothing'";
  for (const char *body :
       {"a = Maybe::Nothing == Maybe::Nothing;",
        "a = Maybe::Nothing != Maybe::Nothing;",
        "n = 1; a = if n > 0 then Maybe::Nothing else Maybe::Nothing;"}) {
    auto r = semaCheck(withEnums(kGeneric, body));
    EXPECT_FALSE(r.Ok) << body;
    EXPECT_NE(r.Diagnostics.find(message), std::string::npos) << body << "\n"
                                                              << r.Diagnostics;
  }
  // Nor does a generic call against another that needs it.
  for (const char *body :
       {"a = none() == none();", "n = 1; a = if n > 0 then none() else none();",
        "a = none() == Maybe::Nothing;"}) {
    auto r = semaCheck(withEnums(
        std::string(kGeneric) + "\nfn none<T>() -> Maybe<T> => Maybe::Nothing;",
        body));
    EXPECT_FALSE(r.Ok) << body;
    EXPECT_NE(r.Diagnostics.find("cannot infer type parameter 'T' of 'none' "
                                 "from the call arguments"),
              std::string::npos)
        << body << "\n"
        << r.Diagnostics;
  }
}

TEST(Enum, GenericVariantsChecked) {
  expectGenericError("x = Maybe::Nothing;",
                     "cannot infer the type arguments of generic enum 'Maybe' "
                     "for 'Maybe::Nothing': write them ('Maybe<...>::Nothing'"
                     "), or give the value a type ('x: Maybe<...> = "
                     "Maybe::Nothing')");
  expectGenericError("x = Result::Ok(1);",
                     "cannot infer type parameter 'E' of 'Result' from the "
                     "call arguments");
  expectGenericError("x = Result::Ok(1);",
                     "specify the type arguments explicitly: "
                     "'Result<...>::Ok(...)'");
  expectGenericError("x = Maybe<int, int>::Nothing;",
                     "generic enum 'Maybe' expects 1 type argument(s), got 2");
  expectGenericError("x = Maybe::Nope;", "enum 'Maybe' has no variant 'Nope'");
  expectGenericError("x = Maybe::Just();",
                     "variant 'Just' of enum 'Maybe' has 1 field(s) (value: "
                     "T), got 0 argument(s)");
  expectGenericError("x = Maybe::Nothing(1);",
                     "variant 'Nothing' of enum 'Maybe' has no fields: write "
                     "'Maybe::Nothing' without arguments");
  expectGenericError("x: Maybe<int> = Maybe::Just(\"s\");",
                     "field 'value' of 'Maybe::Just' has type 'int', got "
                     "'Str'");
  expectGenericError("x: Maybe<int> = Maybe<Str>::Nothing;",
                     "initializer of type 'Maybe<Str>' does not match declared "
                     "type 'Maybe<int>' for variable 'x'");
  expectGenericError("x: Maybe = Maybe<int>::Nothing;",
                     "names generic enum 'Maybe' without type arguments "
                     "(write 'Maybe<...>')");
  expectGenericError("Maybe = 3;",
                     "'Maybe' is a type name and cannot be used as a variable");
}

TEST(Enum, GenericEnumDeclarationsChecked) {
  struct Case {
    const char *Decls;
    const char *Message;
  };
  for (const Case &c : {
           Case{"enum M<T> { A(x: T) }\nenum M<U> { B }",
                "redefinition of generic enum 'M'"},
           Case{"enum M<T> { A(x: T) }\nclass M { }",
                "'M' is already declared as a generic enum"},
           Case{"enum M<T, T> { A(x: T) }",
                "duplicate type parameter 'T' in generic enum 'M'"},
           Case{"enum M<int> { A }", "type parameter 'int' of generic enum "
                                     "'M' shadows a type of the same name"},
           Case{"enum M<T> { A(x: T), A }",
                "duplicate variant 'A' in enum 'M'"},
           Case{"enum M<T> { A(x: T) }\nimpl M<T> { fn f() -> int { return "
                "1; } }",
                "an 'impl' block for an enum is not supported yet"},
           Case{"enum Color { Red }\nfn f(c: Color<int>) { }",
                "'Color' is not a generic enum and takes no type arguments"},
       }) {
    auto r = semaCheck(std::string(c.Decls) + "\n" + wrapMain(""));
    EXPECT_FALSE(r.Ok) << c.Decls;
    EXPECT_NE(r.Diagnostics.find(c.Message), std::string::npos)
        << c.Decls << "\n"
        << r.Diagnostics;
  }
  // A bound is checked where the enum is instantiated.
  auto r = semaCheck("trait Named { view fn name() -> Str; }\n"
                     "enum Tagged<T: Named> { One(v: T) }\n" +
                     wrapMain("t = Tagged::One(3);"));
  EXPECT_NE(r.Diagnostics.find("'int' does not implement trait 'Named', which "
                               "'Tagged' requires of 'T'"),
            std::string::npos)
      << r.Diagnostics;
}

// An instantiation is held in place as any enum is: its variants cannot hold
// it, directly or through another instantiation.
TEST(Enum, GenericEnumCannotHoldItself) {
  auto r = semaCheck("enum L<T> { Nil, Cons(head: T, tail: L<T>) }\n" +
                     wrapMain("l = L<int>::Nil;"));
  EXPECT_NE(r.Diagnostics.find(
                "enum 'L<int>' contains itself: variant 'Cons' holds it in "
                "field 'tail'; an enum is held in place, so its variants "
                "cannot hold it (hold it in a class instead)"),
            std::string::npos)
      << r.Diagnostics;
  r = semaCheck("enum A<T> { X(b: B<T>), E }\nenum B<T> { Y(a: A<T>), F }\n" +
                wrapMain("a = A<int>::E;"));
  EXPECT_NE(r.Diagnostics.find("enum 'A<int>' contains itself: variant 'X' "
                               "holds it in field 'b'"),
            std::string::npos)
      << r.Diagnostics;
  r = semaCheck("enum M<T> { J(v: T), N }\nstruct S { m: M<S>; }\n" +
                wrapMain(""));
  EXPECT_NE(r.Diagnostics.find("struct 'S' contains itself"), std::string::npos)
      << r.Diagnostics;
  // Through a class it is a reference, which is fine.
  r = semaCheck("class Node<T> { v: T; next: L<T>; }\n"
                "enum L<T> { Nil, Cons(n: Node<T>) }\n" +
                wrapMain("l = L::Cons(Node(1, L<int>::Nil));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}
