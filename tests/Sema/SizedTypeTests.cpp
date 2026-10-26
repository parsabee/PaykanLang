// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the sized numeric types (docs/design/sized-types.md) — the
// names and their other spellings, literals taking the type their context
// expects and the fit check, the one-type rule for operators with the `int`
// / `float` promotion kept, the result types of arithmetic and comparisons,
// the conversions between every numeric pair with their result types, the
// builtin methods and traits, optionals, match arms and generic arguments.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

static std::string inMain(const std::string &body) {
  return "import ::io; fn main() -> int {\n" + body + "\n  return 0;\n}\n";
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

// -- names

TEST(SizedTypes, TheOtherSpellingsNameTheSameTypes) {
  // `i64` is `int`, `f64` is `float`, `u64` is `unsigned`: a value of one
  // spelling initializes a variable of the other, and the type prints as
  // the first.
  expectOk(inMain(R"(
    a: i64 = 1;  b: int = a;  c: i64 = b;
    d: f64 = 1.5;  e: float = d;  f: f64 = e;
    g: u64 = 1;  h: unsigned = g;  i: u64 = h;
    j: int = a + b;  k: float = d * e;  l: unsigned = g + h;
  )"));
  expectError(inMain("a: u64 = 1; b: int = a;"),
              "initializer of type 'unsigned' does not match declared type "
              "'int' for variable 'b'");
  // `i64` to `f64` is the `int` to `float` promotion; `u64` has none.
  expectOk(inMain("a: i64 = 1; b: f64 = a;"));
  expectError(inMain("a: u64 = 1; b: f64 = a;"),
              "initializer of type 'unsigned' does not match declared type "
              "'float' for variable 'b'");
}

TEST(SizedTypes, TheTypeNamesAreNotVariableNames) {
  expectError(inMain("u8 = 1;"), "u8");
  expectError(inMain("unsigned = 1;"), "unsigned");
}

// -- literals

TEST(SizedTypes, ALiteralTakesTheTypeItsContextExpects) {
  expectOk("struct P { x: u8; y: i16; }\n"
           "fn takes(n: u8, m: f32) -> u16 { return 65535; }\n" +
           inMain(R"(
    a: u8 = 255;  b: i8 = -128;  c: u16 = 65535;  d: i32 = -2147483648;
    e: u32 = 4294967295;  f: unsigned = 9223372036854775807;
    g: f32 = 1.5;  h: f16 = 65504.0;  i: f32 = 3;  j: f16 = -2;
    k = takes(0, 1);  l = takes(255, 0.5);
    p = P(1, -2);
    a = 0;  b = -1;  g = 2.5;
    m: u8[2] = [1, 2];
    n: (u8, f16) = (3, 0.5);
    o: u8? = 7;
    q: i8 = -(-5);
    if a == 0 && b < 0 && 1 < c && g > 1 && 0.5 <= h { }
    r: u8 = a + 1;  s: i8 = 1 - b;  t: f32 = g * 2;
    match a { 0 { } 255 { } _ { } }
  )"));
}

TEST(SizedTypes, ALiteralOutsideTheTypesRangeIsAnError) {
  expectError(inMain("a: u8 = 256;"),
              "the literal 256 does not fit in 'u8' (0 to 255)");
  expectError(inMain("a: u8 = -1;"),
              "the literal -1 does not fit in 'u8' (0 to 255)");
  expectError(inMain("a: i8 = 128;"),
              "the literal 128 does not fit in 'i8' (-128 to 127)");
  expectError(inMain("a: i8 = -129;"),
              "the literal -129 does not fit in 'i8' (-128 to 127)");
  expectError(inMain("a: i16 = 32768;"),
              "the literal 32768 does not fit in 'i16' (-32768 to 32767)");
  expectError(inMain("a: u16 = 65536;"),
              "the literal 65536 does not fit in 'u16' (0 to 65535)");
  expectError(inMain("a: i32 = 2147483648;"),
              "the literal 2147483648 does not fit in 'i32' (-2147483648 to "
              "2147483647)");
  expectError(inMain("a: u32 = 4294967296;"),
              "the literal 4294967296 does not fit in 'u32' (0 to 4294967295)");
  expectError(inMain("a: unsigned = -1;"),
              "the literal -1 does not fit in 'unsigned' (0 to "
              "18446744073709551615)");
  expectError(inMain("a: u8 = 1; b = a + 256;"),
              "the literal 256 does not fit in 'u8' (0 to 255)");
  expectError(inMain("a: u8 = 1; match a { 256 { } _ { } }"),
              "the literal 256 does not fit in 'u8' (0 to 255)");
  expectError("fn takes(n: i8) -> int { return 0; }\n" + inMain("takes(200);"),
              "the literal 200 does not fit in 'i8' (-128 to 127)");
  expectError(inMain("a: f16 = 70000.0;"),
              "the literal 70000 does not fit in 'f16'");
  expectError(inMain("a: f32 = 1e39;"),
              "the literal 1e+39 does not fit in 'f32'");
}

// The midpoint above the largest f32, 2^128 - 2^103, is a tie that rounds to
// even, which is infinity: the literal does not fit, as
// `"3.4028235677973366e38".toF32()` is None.  Below it, a literal rounds to
// the largest f32.
TEST(SizedTypes, AnF32LiteralFitsBelowTheOverflowMidpoint) {
  expectError(inMain("a: f32 = 3.4028235677973366e38;"),
              "the literal 3.40282e+38 does not fit in 'f32'");
  expectError(inMain("a: f32 = -3.4028235677973366e38;"),
              "the literal -3.40282e+38 does not fit in 'f32'");
  expectOk(inMain("a: f32 = 3.4028235e38; b: f32 = -3.4028235e38;"));
  expectOk(inMain("a: f32 = 3.4028235677973362e38;"));
}

TEST(SizedTypes, OnlyALiteralAdapts) {
  // A compound expression of literals is an `int`; an `int` variable is one.
  expectError(inMain("a: i8 = 100 + 100;"),
              "initializer of type 'int' does not match declared type 'i8' "
              "for variable 'a'");
  expectError(inMain("n = 5; a: u8 = n;"),
              "initializer of type 'int' does not match declared type 'u8' "
              "for variable 'a'");
  expectError(inMain("a: u8 = 1.5;"),
              "initializer of type 'float' does not match declared type 'u8' "
              "for variable 'a'");
  // `int` and `float` slots leave a literal its own type.
  expectOk(inMain("a: int = 5; b: float = 5; c: float = 2.5;"));
}

// -- operators

TEST(SizedTypes, TheOperandsOfAnOperationHaveOneType) {
  expectError(inMain("a: i32 = 5; b: int = 6; c = a + b;"),
              "operator '+' is not defined for types 'i32' and 'int' (the "
              "operands must have one type: convert one with 'i32(...)' or "
              "'int(...)')");
  expectError(inMain("a: u8 = 5; b: u16 = 6; c = a < b;"),
              "operator '<' is not defined for types 'u8' and 'u16'");
  expectError(inMain("a: f32 = 5.0; b: float = 6.0; c = a * b;"),
              "operator '*' is not defined for types 'f32' and 'float'");
  expectError(inMain("a: f32 = 5.0; b: int = 6; c = a + b;"),
              "operator '+' is not defined for types 'f32' and 'int'");
  expectError(inMain("a: u8 = 5; b: u8? = 6; c = a == b;"),
              "cannot compare optional 'u8?' with non-optional 'u8'");
  // The `int` with `float` promotion stays.
  expectOk(inMain("a: int = 5; b: float = 6.0; c: float = a + b; d = a < b;"));
  // Conversions spell the mix.
  expectOk(inMain("a: i32 = 5; b: int = 6; c: int = int(a) + b; "
                  "d: i32 = a + i32(b);"));
}

TEST(SizedTypes, ArithmeticHasTheOperandsTypeAndComparisonsAreBool) {
  expectOk(inMain(R"(
    a: u8 = 1;  b: u8 = a + a;  c: u8 = a * a - a;  d: u8 = a / a;  e: u8 = a % a;
    f: i16 = -3;  g: i16 = -f;  h: i16 = f * f;
    u: unsigned = 7;  v: unsigned = u / 2;  w: unsigned = u % 2;
    x: f32 = 1.5;  y: f32 = x / x;  z: f32 = -x;
    p: f16 = 0.5;  q: f16 = p + p;
    lt: bool = a < b;  eq: bool = f == g;  ge: bool = u >= v;  ne: bool = x != y;
  )"));
  expectError(inMain("a: u8 = 1; b: int = a + a;"),
              "initializer of type 'u8' does not match declared type 'int' "
              "for variable 'b'");
  expectError(inMain("a: f16 = 1.5; b: float = a * a;"),
              "initializer of type 'f16' does not match declared type 'float' "
              "for variable 'b'");
}

TEST(SizedTypes, AnUnsignedTypeHasNoNegation) {
  expectError(inMain("a: u8 = 1; b = -a;"),
              "unary '-' is not defined for type 'u8'");
  expectError(inMain("a: unsigned = 1; b = -a;"),
              "unary '-' is not defined for type 'unsigned'");
  expectOk(inMain("a: i8 = 1; b: i8 = -a; c: f16 = 1.0; d: f16 = -c;"));
}

TEST(SizedTypes, AnIndexIsAnInt) {
  expectError(inMain("xs: int[3] = [1, 2, 3]; i: u8 = 1; x = xs[i];"), "u8");
  expectOk(inMain("xs: u8[3] = [1, 2, 3]; i: u8 = 1; x: u8 = xs[int(i)];"));
}

// -- conversions

// Each sized type takes its family's sources
// (docs/language/04-strings-and-conversions.md, "Between numbers"; every
// pair: Conversion.SourceSetsAreTheFamilies).
TEST(SizedTypes, EachTypeConvertsFromItsFamilysSources) {
  expectOk(inMain(R"(
    i: int = 1;  f: float = 1.5;  u: unsigned = 2;
    a: i8 = 1;  b: i16 = 2;  c: i32 = 3;  d: u8 = 4;  e: u16 = 5;  g: u32 = 6;
    h: f16 = 0.5;  k: f32 = 1.5;  t: bool = True;  z: char = 'z';
    a1: i8 = i8(i);  a2: i8 = i8(f);  a3: i8 = i8(d);  a4: i8 = i8(b);  a5: i8 = i8(k);
    b1: u8 = u8(i);  b2: u8 = u8(a);  b3: u8 = u8(u);  b4: u8 = u8(g);
    c1: int = int(a);  c2: int = int(d);  c3: int = int(g);  c4: int = int(k);
    d1: unsigned = unsigned(i);  d2: unsigned = unsigned(d);  d3: unsigned = unsigned(a);
    e1: float = float(a);  e2: float = float(u);  e3: float = float(k);  e4: float = float(h);
    f1: f32 = f32(i);  f2: f32 = f32(f);  f3: f32 = f32(h);  f4: f32 = f32(u);
    g1: f16 = f16(f);  g2: f16 = f16(k);  g3: f16 = f16(a);
    h1: u32 = u32(c);  h2: i32 = i32(g);  h3: u16 = u16(b);  h4: i16 = i16(e);
    // A bool and a char into every integer type, an integer into both.
    l1: u8 = u8(z);  l2: i8 = i8(z);  l3: unsigned = unsigned(t);  l4: u16 = u16(t);
    m1: bool = bool(d);  m2: bool = bool(u);  m3: char = char(d);  m4: char = char(c);
    // Each type converts from itself.
    n1: u8 = u8(d);  n2: f16 = f16(h);  n3: unsigned = unsigned(u);
    // The explicit form and the other spellings.
    k1: i8 = i8<int>(i);  k2: unsigned = u64<u8>(d);  k3: int = i64<u8>(d);
    k4: float = f64<f32>(k);  k5: unsigned = unsigned<i8>(a);  k6: int = i64<int>(i);
  )"));
}

TEST(SizedTypes, ConversionsToStr) {
  expectOk(inMain(R"(
    a: i8 = 1;  u: unsigned = 2;  k: f32 = 1.5;  h: f16 = 0.5;
    s1: Str = Str(a) + Str(u) + Str(k) + Str(h) + Str<u16>(3) + Str<unsigned>(u);
  )"));
}

// A sized type parses a `Str` with the method of its name, which the error
// of the older conversion names, under the type's canonical name.
TEST(SizedTypes, TheirParsesAreMethodsOfStr) {
  const std::string s = "s: Str = \"1\"; ";
  expectError(inMain(s + "p = i8(s);"),
              "no specialization of 'i8' for 'Str': parse a 'Str' with its "
              "method 'toI8()', which returns 'i8?'");
  expectError(inMain(s + "p = u32<Str>(s);"),
              "no specialization of 'u32' for 'Str': parse a 'Str' with its "
              "method 'toU32()', which returns 'u32?'");
  expectError(inMain(s + "p = u64(s);"),
              "no specialization of 'unsigned' for 'Str': parse a 'Str' with "
              "its method 'toUnsigned()', which returns 'unsigned?'");
  expectError(inMain(s + "p = unsigned<Str>(s);"),
              "no specialization of 'unsigned' for 'Str': parse a 'Str' with "
              "its method 'toUnsigned()', which returns 'unsigned?'");
  expectError(inMain(s + "p = f32(s);"),
              "no specialization of 'f32' for 'Str': parse a 'Str' with its "
              "method 'toF32()', which returns 'f32?'");
  expectError(inMain(s + "p = f16<Str>(s);"),
              "no specialization of 'f16' for 'Str': parse a 'Str' with its "
              "method 'toF16()', which returns 'f16?'");
  expectOk(inMain(s + R"(
    p1: i8? = s.toI8();  p2: unsigned? = s.toUnsigned();  p3: f32? = s.toF32();
    match p1 { x: i8 { t: Str = Str(x); } None { } }
  )"));
}

// An explicit conversion expects one argument of its written source type:
// extra arguments are the arity error, with no expectation to read for
// them (the sanitizers caught an out-of-bounds read here).
TEST(SizedTypes, AnExplicitConversionWithExtraArgumentsIsAnArityError) {
  expectError(inMain("s: Str = Str<u16>(3, 4);"),
              "conversion 'Str<u16>' takes exactly one argument, got 2");
  expectError(inMain("n: int = int<float>(1.5, 2.5, 3.5);"),
              "conversion 'int<float>' takes exactly one argument, got 3");
  expectError(inMain("n: u8 = u8<int>();"),
              "conversion 'u8<int>' takes exactly one argument, got 0");
}

TEST(SizedTypes, TheConversionSetIsClosedAndListed) {
  // Errors before the families, conversions now.
  expectOk(inMain("a: u8 = u8('a'); b: u8 = u8(a); t: bool = bool(a); "
                  "c: char = char(a);"));
  // A pair the families leave out names the rewrite, under the canonical
  // names (`u64` is `unsigned`, `i64` is `int`).
  expectError(inMain("u: unsigned = 1; a: i8 = i8(u);"),
              "no specialization of 'i8' for 'unsigned': an 'unsigned' can be "
              "larger than any 'i8'; 'i8(u8(...))' keeps its low 8 bits\n");
  expectError(inMain("u: u64 = 1; n: i64 = i64(u);"),
              "no specialization of 'int' for 'unsigned': an 'unsigned' can be "
              "larger than any 'int'; its method 'toInt()' returns 'int?', "
              "None above 9223372036854775807\n");
  expectError(inMain("k: f32 = 1.5; a: u16 = u16<f32>(k);"),
              "no specialization of 'u16' for 'f32': convert the 'f32' to an "
              "'int' first: 'u16(int(...))' truncates toward zero, then keeps "
              "the low 16 bits\n");
  expectError(inMain("a: u64 = u64(2.5);"),
              "no specialization of 'unsigned' for 'float': convert the "
              "'float' to an 'int' first: 'unsigned(int(...))' truncates "
              "toward zero, then keeps the low 64 bits\n");
  expectError(inMain("h: f16 = f16('a');"),
              "no specialization of 'f16' for 'char': convert the 'char' to an "
              "'int' first: 'f16(int(...))'\n");
  expectError(inMain("h: f16 = 0.5; b: bool = bool(h);"),
              "no specialization of 'bool' for 'f16'; its specializations are "
              "int, i8, i16, i32, u8, u16, u32, unsigned, bool\n");
  expectError(inMain("k: f32 = 0.5; c: char = char(k);"),
              "no specialization of 'char' for 'f32'; its specializations are "
              "int, i8, i16, i32, u8, u16, u32, unsigned, char\n");
  // A literal argument takes the written source type; a variable keeps its.
  expectOk(inMain("b: int = int<u32>(5);"));
  expectError(inMain("n = 5; b: int = int<u32>(n);"),
              "argument of 'int<u32>' has type 'int', expected 'u32'");
  expectError(inMain("a: i8? = 1; b: int = int(a);"),
              "no specialization of 'int' for 'i8?'");
}

// -- methods, traits, generics

TEST(SizedTypes, BuiltinMethodsAndTraits) {
  expectOk("fn show<T: Show>(v: T) -> Str { return v.toString(); }\n"
           "fn key<T: Hashable>(v: T) -> int { return v.hash(); }\n"
           "fn copy<T: ValueType>(v: T) -> T { return v; }\n" +
           inMain(R"(
    a: u8 = 1;  u: unsigned = 2;  k: f32 = 1.5;  h: f16 = 0.5;  c: i32 = -3;
    s: Str = a.toString() + u.toString() + k.toString() + h.toString();
    n: int = a.hash() + u.hash() + c.hash();
    t: Str = show(a) + show(k) + show(h);
    m: int = key(a) + key(u) + key(c);
    b: u8 = copy(a);  f: f16 = copy(h);
  )"));
  expectError(inMain("k: f32 = 1.5; n = k.hash();"), "f32");
  expectError("fn key<T: Hashable>(v: T) -> int { return v.hash(); }\n" +
                  inMain("h: f16 = 0.5; n = key(h);"),
              "Hashable");
}

TEST(SizedTypes, SizedTypesAreTypeArgumentsOptionalsAndMatchSubjects) {
  expectOk("class Box<T> { v: T; }\n"
           "impl Box<T> { fn __init__(v: T) { self.v = v; } "
           "view fn get() -> T { return self.v; } }\n"
           "struct Pair<A, B> { a: A; b: B; }\n" +
           inMain(R"(
    b = Box<u8>(200);  c: u8 = b.get() + 50;
    d = Box<unsigned>(unsigned(-1));  e: unsigned = d.get();
    p = Pair<i16, f32>(-1, 0.5);  q: i16 = p.a;  r: f32 = p.b;
    o: u8? = 5;
    match o { v: u8 { w: u8 = v + 1; } None { } }
    n: i32 = -7;
    match n { 7 { } 0 { } _ { } }
    f: f32 = 1.5;
    match f { 1.5 { } _ { } }
  )"));
  expectError(inMain("n: i32 = -7; match n { 1.5 { } _ { } }"),
              "match arm literal of type 'float' does not match subject type "
              "'i32'");
}

// A negative number is a match pattern, one literal (docs/grammar.md
// section 6), which takes a sized subject's type when it fits, as one
// assigned to it does: `-1` on an unsigned or `-129` on an i8 is the error a
// `-1` or `-129` assigned to one is.
TEST(SizedTypes, NegativePatternsFitTheSubjectAsAssignedLiteralsDo) {
  expectOk(inMain(R"(
    n: i32 = -7;  match n { -7 { } - 8 { } _ { } }
    b: i8 = -128;  match b { -128 { } 127 { } _ { } }
    f: f32 = -1.5;  match f { -1.5 { } -0.0 { } _ { } }
    m = 0;  match m { -9223372036854775808 { } _ { } }
  )"));
  for (const auto &[pattern, assigned] :
       {std::pair<const char *, const char *>{
            "u: unsigned = 5; match u { -1 { } _ { } }", "u: unsigned = -1;"},
        {"b: i8 = 5; match b { -129 { } _ { } }", "b: i8 = -129;"},
        {"h: u8 = 5; match h { -1 { } _ { } }", "h: u8 = -1;"}}) {
    auto byPattern = semaCheck(inMain(pattern));
    auto byAssignment = semaCheck(inMain(assigned));
    EXPECT_FALSE(byPattern.Ok) << pattern;
    EXPECT_FALSE(byAssignment.Ok) << assigned;
    // The same message, but for its column.
    auto message = [](const std::string &diags) {
      size_t at = diags.find("error: ");
      return at == std::string::npos
                 ? diags
                 : diags.substr(at, diags.find('\n', at) - at);
    };
    EXPECT_EQ(message(byPattern.Diagnostics), message(byAssignment.Diagnostics))
        << pattern;
  }
  expectError(inMain("u: unsigned = 5; match u { -1 { } _ { } }"),
              "error: the literal -1 does not fit in 'unsigned' (0 to "
              "18446744073709551615)");
}

TEST(SizedTypes, TheSizedTypesTakeEightByteSlots) {
  // A fixed-size array's element size is the type's slot: 8 bytes for every
  // numeric type, 1 for bool and char, so `u8[3]` is three 8-byte slots and
  // initializes from three elements like `int[3]`.
  expectOk(inMain("a: u8[3] = [1, 2, 3]; b: f16[2] = [0.5, 1.5]; "
                  "c: unsigned[1] = [0]; n = a.len() + b.len() + c.len();"));
}
