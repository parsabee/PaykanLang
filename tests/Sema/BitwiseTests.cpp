// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the bitwise operators (docs/design/bitwise.md) — defined on
// the unsigned types only, `& | ^` with the one-type rule of `+`, a shift's
// count of any unsigned type, `~` of a literal in an unsigned slot, the
// hint every rejected operand's error names, and the warning of a literal
// shift count at or above the width.

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

TEST(Bitwise, TheOperatorsTakeUnsignedTypes) {
  for (const char *ty : {"u8", "u16", "u32", "unsigned", "u64"}) {
    SCOPED_TRACE(ty);
    // Each result has the operands' type: it initializes a `T` variable.
    std::string body = R"(let a: T = 12; let b: T = 10;
      let c: T = a & b; let d: T = a | b; let e: T = a ^ b; let f: T = ~a;
      let g: T = a << b; let h: T = a >> b;)";
    for (size_t at = body.find(": T "); at != std::string::npos;
         at = body.find(": T ", at))
      body.replace(at + 2, 1, ty);
    expectOk(inMain(body));
  }
  // `u8 & u8` is a `u8`, not a `u16`.
  expectError(inMain("let a: u8 = 1; let b: u8 = 2; let z: u16 = a & b;"),
              "initializer of type 'u8' does not match declared type 'u16' "
              "for variable 'z'");
  expectError(inMain("let a: u32 = 1; let n: u8 = 2; let z: u8 = a << n;"),
              "initializer of type 'u32' does not match declared type 'u8' "
              "for variable 'z'");
}

TEST(Bitwise, SignedOperandsNameTheConversion) {
  expectError(inMain("let a = 6; let b = 3; let c = a & b;"),
              "operator '&' is not defined for types 'int' and 'int' (the "
              "bitwise operators take unsigned operands: convert with "
              "'unsigned(...)')");
  expectError(inMain("let x: u8 = 1; let n = 2; let c = x | n;"),
              "operator '|' is not defined for types 'u8' and 'int' (the "
              "bitwise operators take unsigned operands: convert the 'int' "
              "with 'u8(...)')");
  expectError(inMain("let n = 2; let x: u8 = 1; let c = n | x;"),
              "operator '|' is not defined for types 'int' and 'u8' (the "
              "bitwise operators take unsigned operands: convert the 'int' "
              "with 'u8(...)')");
  expectError(inMain("let i: i32 = 1; let j: i32 = 2; let c = i ^ j;"),
              "operator '^' is not defined for types 'i32' and 'i32' (the "
              "bitwise operators take unsigned operands: convert with "
              "'u32(...)'; '^' is exclusive or, not a power)");
  expectError(inMain("let k: i16 = 1; let c = k | k;"),
              "operator '|' is not defined for types 'i16' and 'i16' (the "
              "bitwise operators take unsigned operands: convert with "
              "'u16(...)')");
  expectError(inMain("let k: i8 = 1; let c = k & k;"),
              "operator '&' is not defined for types 'i8' and 'i8' (the "
              "bitwise operators take unsigned operands: convert with "
              "'u8(...)')");
  expectError(inMain("let c = 'a'; let d = c & c;"),
              "operator '&' is not defined for types 'char' and 'char' (the "
              "bitwise operators take unsigned operands: convert with "
              "'u8(...)')");
  expectError(inMain("let c = 'a'; let x: u16 = 1; let d = x & c;"),
              "operator '&' is not defined for types 'u16' and 'char' (the "
              "bitwise operators take unsigned operands: convert the 'char' "
              "with 'u16(...)')");
  expectError(inMain("let i = 3; let c = ~i;"),
              "unary '~' is not defined for type 'int' (the bitwise "
              "operators take unsigned operands: convert with "
              "'unsigned(...)')");
  expectError(inMain("let h: i8 = 1; let c = ~h;"),
              "unary '~' is not defined for type 'i8' (the bitwise operators "
              "take unsigned operands: convert with 'u8(...)')");
  expectError(inMain("let c = 'a'; let d = ~c;"),
              "unary '~' is not defined for type 'char' (the bitwise "
              "operators take unsigned operands: convert with 'u8(...)')");
}

TEST(Bitwise, BoolAndFloatOperands) {
  expectError(inMain("let p = True; let q = False; let c = p & q;"),
              "operator '&' is not defined for types 'bool' and 'bool' (for "
              "a logical and, write '&&')");
  expectError(inMain("let p = True; let q = False; let c = p | q;"),
              "operator '|' is not defined for types 'bool' and 'bool' (for "
              "a logical or, write '||')");
  expectError(inMain("let p = True; let q = False; let c = p ^ q;"),
              "operator '^' is not defined for types 'bool' and 'bool' (for "
              "a logical exclusive or, write '!=')");
  expectError(inMain("let b = True; let c = ~b;"),
              "unary '~' is not defined for type 'bool' (for a logical not, "
              "write '!')");
  expectError(inMain("let f = 1.5; let c = f ^ 2.0;"),
              "operator '^' is not defined for types 'float' and 'float' "
              "('^' is exclusive or, not a power)");
  expectError(inMain("let f = 1.5; let c = f & 2.0;"),
              "operator '&' is not defined for types 'float' and 'float' "
              "(the bitwise operators take unsigned operands, not floats)");
  expectError(inMain("let g: f32 = 1; let c = ~g;"),
              "unary '~' is not defined for type 'f32' (the bitwise "
              "operators take unsigned operands, not floats)");
  expectError(inMain("let x: u8 = 1; let c = x << 1.5;"),
              "operator '<<' is not defined for types 'u8' and 'float' (the "
              "bitwise operators take unsigned operands, not floats)");
  // A string has no bitwise operator and no hint.
  expectError(inMain("let s = \"a\"; let c = s & s;"),
              "operator '&' is not defined for types 'Str' and 'Str'\n");
}

TEST(Bitwise, TwoLiteralsAreInt) {
  expectError(inMain("let m: u32 = 1 << 5;"),
              "operator '<<' is not defined for types 'int' and 'int' (two "
              "literals are 'int': give one a type, as in 'unsigned(1) << "
              "5')");
  expectError(inMain("let c = 2 ^ 8;"),
              "operator '^' is not defined for types 'int' and 'int' (two "
              "literals are 'int': give one a type, as in 'unsigned(2) ^ 8'; "
              "'^' is exclusive or, not a power)");
  expectError(inMain("let c: u8 = 1 & 2;"),
              "operator '&' is not defined for types 'int' and 'int' (two "
              "literals are 'int': give one a type, as in 'unsigned(1) & "
              "2')");
  expectError(inMain("let c = -1 | 2;"),
              "(two literals are 'int': give one a type, as in "
              "'unsigned(-1) | 2')");
  // The spelling the hint gives.
  expectOk(inMain("let m: u32 = u32(1) << 5; let c = unsigned(2) ^ 8;"));
}

TEST(Bitwise, LiteralOperandsTakeTheOtherType) {
  expectOk(inMain("let x: u8 = 3;\n"
                  "let a: u8 = x & 0xF0; let b: u8 = 0xF0 & x;\n"
                  "let c: u8 = x | 0b1000_0000; let d: u8 = 0xFF ^ x;"));
  expectError(inMain("let x: u8 = 3; let c = x & 0x100;"),
              "the literal 256 does not fit in 'u8' (0 to 255)");
  expectError(inMain("let x: u16 = 3; let c = 0x1_0000 | x;"),
              "the literal 65536 does not fit in 'u16' (0 to 65535)");
  // An `unsigned` takes every literal up to 2^64-1.
  expectOk(inMain("let x: unsigned = 3; let c = x & 0xFFFF_FFFF_FFFF_FFFF;"));
}

TEST(Bitwise, ShiftCounts) {
  // A count of any unsigned type: the result is the value's type.
  expectOk(inMain("let x: u8 = 1; let n: unsigned = 3; let m: u32 = 300;\n"
                  "let k: u8 = 2; let w: unsigned = 7;\n"
                  "let a: u8 = x << n; let b: u8 = x << m;\n"
                  "let c: unsigned = w >> k; let d: u8 = x << 3;\n"
                  "let e: u8 = x << ~0;"));
  expectError(inMain("let x: u32 = 1; let i = 3; let c = x << i;"),
              "operator '<<' is not defined for types 'u32' and 'int' (a "
              "shift count is unsigned: convert it with 'unsigned(...)')");
  expectError(inMain("let x: u8 = 1; let i: i8 = 3; let c = x >> i;"),
              "operator '>>' is not defined for types 'u8' and 'i8' (a shift "
              "count is unsigned: convert it with 'unsigned(...)')");
  expectError(inMain("let x: u8 = 1; let c = 'a'; let d = x << c;"),
              "operator '<<' is not defined for types 'u8' and 'char' (a "
              "shift count is unsigned: convert it with 'unsigned(...)')");
  // The value takes no type from the count.
  expectError(inMain("let n: u8 = 3; let c = 1 << n;"),
              "operator '<<' is not defined for types 'int' and 'u8' (the "
              "value shifted takes no type from the count: write its type, "
              "as in 'unsigned(1)')");
  expectError(inMain("let x: u8 = 1; let c = x << -1;"),
              "the literal -1 does not fit in 'unsigned' (0 to "
              "18446744073709551615)");
  // `& | ^` keep the one-type rule.
  expectError(inMain("let x: u8 = 1; let y: u16 = 2; let c = x & y;"),
              "operator '&' is not defined for types 'u8' and 'u16' (the "
              "operands must have one type: convert one with 'u8(...)' or "
              "'u16(...)')");
  // An optional is unwrapped first, as for every operator.
  expectError(inMain("let x: u8? = 1; let c = x << 1;"),
              "cannot use optional 'u8?' as 'u8' without unwrapping (use "
              "match)");
  expectError(inMain("let x: u8? = 1; let c = x & 1;"),
              "cannot use optional 'u8?' as 'u8' without unwrapping (use "
              "match)");
}

TEST(Bitwise, TildeLiterals) {
  expectOk(inMain("let x: u8 = 0xFF; let a: u8 = x & ~0x0F;\n"
                  "let y: u8 = ~0; let z: unsigned = ~0;\n"
                  "let w: u16 = ~0xFF; let o: u32? = ~1;"));
  expectError(inMain("let z: u16 = ~0x1_0000;"),
              "the literal 65536 does not fit in 'u16' (0 to 65535)");
  expectError(inMain("let z: u8 = ~256;"),
              "the literal 256 does not fit in 'u8' (0 to 255)");
  // With no unsigned context, `~5` is `~` of an `int`.
  for (const char *src :
       {"let w = ~5;", "let v: i8 = ~1;", "let f: float = ~1;",
        "let a: u8 = -~5;", "let b: u8 = ~~5;"}) {
    SCOPED_TRACE(src);
    expectError(inMain(src), "unary '~' is not defined for type 'int' (the "
                             "bitwise operators take unsigned operands: "
                             "convert with 'unsigned(...)')");
  }
}

TEST(Bitwise, OvershiftWarns) {
  const std::string warn8 = "warning: the shift count 8 is not less than the "
                            "width of 'u8' (8 bits): the result is always 0";
  const std::string warn64 =
      "warning: the shift count 64 is not less than the width of 'unsigned' "
      "(64 bits): the result is always 0";
  auto r = semaCheck(inMain("let x: u8 = 1; let a = x << 8;\n"
                            "let u: unsigned = 1; let b = u >> 64;\n"
                            "let n: unsigned = 9; let c = x << 7;\n"
                            "let d = x << n; let e = u >> 63;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":2:29: " + warn8), std::string::npos)
      << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":3:35: " + warn64), std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("warning: the shift count 7"), std::string::npos)
      << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find("warning: the shift count 63"),
            std::string::npos)
      << r.Diagnostics;
  // Up to 2^64-1.
  r = semaCheck(inMain("let x: u32 = 1; let a = x >> 0xFFFF_FFFF_FFFF_FFFF;"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("the shift count 18446744073709551615 is not "
                               "less than the width of 'u32' (32 bits)"),
            std::string::npos)
      << r.Diagnostics;
  // A generic body's place warns once, though two instantiations reach it.
  r = semaCheck("import ::io;\nfn sh<T>(x: T) -> T => x << 16;\n" +
                inMain("let a = sh(u8(1)); let b = sh(u16(1));\n"
                       "let c = sh(unsigned(1));"));
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  size_t warnings = 0;
  for (size_t at = r.Diagnostics.find("warning: "); at != std::string::npos;
       at = r.Diagnostics.find("warning: ", at + 1))
    ++warnings;
  EXPECT_EQ(warnings, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find(":2:29: warning: the shift count 16 is not "
                               "less than the width of 'u8' (8 bits)"),
            std::string::npos)
      << r.Diagnostics;
}

TEST(Bitwise, CompoundAssignments) {
  expectOk(inMain("f: u8 = 0;\n"
                  "f &= 0x0F; f |= 1; f ^= 0xFF; f <<= 1; f >>= 2;\n"
                  "f &= ~1; n: unsigned = 3; f <<= n; f >>= n;"));
  // The binary operator's error, at the statement's start.
  expectError(inMain("i = 8;\n  i >>= 1;"),
              ":3:3: error: operator '>>' is not defined for types 'int' and "
              "'int' (the bitwise operators take unsigned operands: convert "
              "with 'unsigned(...)')");
  expectError(inMain("i = 8;\n  i &= 1;"),
              ":3:3: error: operator '&' is not defined for types 'int' and "
              "'int' (the bitwise operators take unsigned operands: convert "
              "with 'unsigned(...)')");
}

TEST(Bitwise, GenericBodies) {
  const std::string clear = "fn clear<T>(x: T, m: T) -> T => x & ~m;\n";
  expectOk("import ::io;\n" + clear +
           inMain("let a = clear(u8(0xFF), u8(0x0F));\n"
                  "let b = clear(unsigned(0xFFFF), unsigned(0xFF));"));
  expectError("import ::io;\n" + clear + inMain("let a = clear(5, 3);"),
              ":2:37: error: unary '~' is not defined for type 'int' (the "
              "bitwise operators take unsigned operands: convert with "
              "'unsigned(...)')");
}
