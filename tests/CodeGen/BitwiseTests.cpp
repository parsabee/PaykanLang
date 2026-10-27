// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// CodeGen tests: the bitwise operators on the unsigned types
// (docs/design/bitwise.md) — `& | ^ ~` on every width, shifts at and past
// the width (0 for every count from the width to 2^64-1, whether the count
// is a run-time value or a constant, and two constants folding to the same
// value), a count of another width, the mask after a narrow `<<`, the five
// compound assignments on a variable, a field and an element, the order
// operands are evaluated in, and the masked counts of the rotate idiom.
// Every test checks zero live heap blocks.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace paykan::test;

namespace {

std::string wrapMain(const std::string &body) {
  return "import ::io;\nfn main() -> int {\n" + body + "\n  return 0;\n}\n";
}

/// An unsigned type with a value that sets its top and bottom bits.
struct Width {
  const char *Type;
  unsigned Bits;
  uint64_t Value;
};

constexpr Width kWidths[] = {
    {"u8", 8, 0x81},
    {"u16", 16, 0x8001},
    {"u32", 32, 0x80000001},
    {"unsigned", 64, 0x8000000000000001},
};

uint64_t ones(unsigned bits) {
  return bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
}

} // namespace

TEST(Bitwise, AndOrXorNotOnEveryWidth) {
  LeakGuard g;
  std::string body;
  std::string expected;
  for (const Width &w : kWidths) {
    const uint64_t a = w.Value | 0x0C;
    const uint64_t b = (w.Value >> 1) | 0x0A;
    const std::string t = w.Type;
    const std::string sa = std::to_string(a);
    const std::string sb = std::to_string(b);
    body += "  let a" + std::to_string(w.Bits) + ": " + t + " = " + sa +
            "; let b" + std::to_string(w.Bits) + ": " + t + " = " + sb + ";\n";
    const std::string va = "a" + std::to_string(w.Bits);
    const std::string vb = "b" + std::to_string(w.Bits);
    // On variables, then on two constants (folded).
    body += "  io::println(Str(" + va + " & " + vb + ") + \" \" + Str(" + va +
            " | " + vb + ") + \" \" + Str(" + va + " ^ " + vb +
            ") + \" \" + Str(~" + va + "));\n";
    body += "  io::println(Str(" + t + "(" + sa + ") & " + t + "(" + sb +
            ")) + \" \" + Str(" + t + "(" + sa + ") | " + sb +
            ") + \" \" + Str(" + t + "(" + sa + ") ^ " + sb +
            ") + \" \" + Str(~" + t + "(" + sa + ")));\n";
    const std::string line =
        std::to_string(a & b) + " " + std::to_string(a | b) + " " +
        std::to_string(a ^ b) + " " + std::to_string(~a & ones(w.Bits)) + "\n";
    expected += line + line;
  }
  auto r = compileAndRun(wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks("Bitwise.AndOrXorNotOnEveryWidth");
}

// Each width shifted by counts at, below and past its width, read from an
// array at run time (the guarded shift), written as literals (the constant
// count) and with a constant value too (the fold): the three agree, and
// every count from the width up is 0.
TEST(Bitwise, ShiftsAtAndPastTheWidth) {
  LeakGuard g;
  std::string body;
  std::string expected;
  for (const Width &w : kWidths) {
    const std::vector<uint64_t> counts = {
        0,  1,  w.Bits - 1, w.Bits, w.Bits + 1,        63,
        64, 65, 255,        65535,  uint64_t{1} << 63, UINT64_MAX};
    const std::string t = w.Type;
    const std::string x = "x" + std::to_string(w.Bits);
    const std::string n = "n" + std::to_string(w.Bits);
    const std::string value = std::to_string(w.Value);
    body += "  let " + x + ": " + t + " = " + value + ";\n  let " + n +
            ": unsigned[" + std::to_string(counts.size()) + "] = [";
    for (size_t i = 0; i < counts.size(); ++i)
      body += (i ? ", " : "") + std::to_string(counts[i]);
    body += "];\n";
    for (size_t i = 0; i < counts.size(); ++i) {
      const std::string c = std::to_string(counts[i]);
      const std::string at = n + "[" + std::to_string(i) + "]";
      body += "  io::println(Str(" + x + " << " + at + ") + \" \" + Str(" + x +
              " << " + c + ") + \" \" + Str(" + t + "(" + value + ") << " + c +
              ") + \" \" + Str(" + x + " >> " + at + ") + \" \" + Str(" + x +
              " >> " + c + ") + \" \" + Str(" + t + "(" + value + ") >> " + c +
              "));\n";
      const uint64_t k = counts[i];
      const std::string shl =
          std::to_string(k >= w.Bits ? 0 : (w.Value << k) & ones(w.Bits));
      const std::string shr = std::to_string(k >= w.Bits ? 0 : w.Value >> k);
      expected += shl + " " + shl + " " + shl + " " + shr + " " + shr + " " +
                  shr + "\n";
    }
  }
  auto r = compileAndRun(wrapMain(body));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, expected);
  g.expectNoLeaks("Bitwise.ShiftsAtAndPastTheWidth");
}

TEST(Bitwise, CountOfAnotherWidth) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
  let wide: u32 = 256;
  let far: u8 = 255;
  let seven: u16 = 7;
  let big: unsigned = 0xFFFF_FFFF_FFFF_FFFF;
  io::println(Str(u8(1) << u32(256)) + " " + Str(u8(0x80) >> u8(255)) + " "
      + Str(u8(1) << u16(7)));
  io::println(Str(u8(1) << wide) + " " + Str(u8(0x80) >> far) + " "
      + Str(u8(1) << seven));
  let x: u16 = 0x8000;
  io::println(Str(x >> u8(15)) + " " + Str(x >> big) + " " + Str(x << far));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0 0 128\n0 0 128\n1 0 0\n");
  g.expectNoLeaks("Bitwise.CountOfAnotherWidth");
}

// A `<<` on a `u8`, `u16` or `u32` drops the bits it shifts past the
// width, so a `>>` after it reads only what is left.
TEST(Bitwise, NarrowShlMasks) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
  io::println(Str(u8(0xFF) << 4) + " " + Str(u16(0xFFFF) << 8) + " "
      + Str(u32(0xFFFF_FFFF) << 16));
  let a: u8 = 0xFF;
  let b: u16 = 0xFFFF;
  let c: u32 = 0xFFFF_FFFF;
  io::println(Str(a << 4) + " " + Str(b << 8) + " " + Str(c << 16));
  let four: unsigned = 4;
  let eight: u8 = 8;
  let sixteen: u16 = 16;
  io::println(Str(a << four) + " " + Str(b << eight) + " " + Str(c << sixteen));
  io::println(Str((a << four) >> four) + " " + Str((c << sixteen) >> sixteen));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "240 65280 4294901760\n240 65280 4294901760\n"
                      "240 65280 4294901760\n15 65535\n");
  g.expectNoLeaks("Bitwise.NarrowShlMasks");
}

// `~` stays within the width: its result shifted right shows no bit above
// it, and a `~` literal takes its unsigned slot's width.
TEST(Bitwise, ComplementStaysInTheWidth) {
  LeakGuard g;
  auto r = compileAndRun(wrapMain(R"(
  let a: u8 = 0x0F;
  let b: u16 = 0x00FF;
  let c: u32 = 1;
  let d: unsigned = 0;
  io::println(Str(~a) + " " + Str(~b) + " " + Str(~c) + " " + Str(~d));
  io::println(Str(~~a) + " " + Str(~~b) + " " + Str(~~c) + " " + Str(~~d));
  io::println(Str(~a >> 4) + " " + Str(~b >> 8) + " " + Str(~c >> 31) + " "
      + Str(~d >> 63));
  let y: u8 = ~0;
  let z: u32 = ~0x0F;
  let w: unsigned = ~0x0F;
  io::println(Str(y) + " " + Str(z) + " " + Str(w) + " " + Str(a & ~0x0E));
  io::println(Str(~u8(0x0F)) + " " + Str(~u32(1)));
  )"));
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "240 65280 4294967294 18446744073709551615\n"
                      "15 255 1 0\n"
                      "15 255 1 1\n"
                      "255 4294967280 18446744073709551600 1\n"
                      "240 4294967294\n");
  g.expectNoLeaks("Bitwise.ComplementStaysInTheWidth");
}

TEST(Bitwise, CompoundAssignmentsOnEveryTarget) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
struct P { f: u8; }
class C { g: u16; }
fn main() -> int {
  v: u8 = 0b1010;
  v &= 0b0110;
  v |= 0x80;
  v ^= 0xFF;
  v <<= 2;
  v >>= 1;
  p = P(0x0F);
  p.f <<= 4;
  p.f |= 1;
  p.f >>= 1;
  p.f ^= 0x0F;
  p.f &= 0xF0;
  let c = C(0x00FF);
  c.g <<= 12;
  c.g ^= 0x0F00;
  c.g >>= 8;
  xs: unsigned[2] = [1, 2];
  xs[1] <<= 62;
  xs[0] |= xs[1];
  xs[1] >>= 61;
  xs[0] ^= 1;
  let n: u8 = 70;
  v <<= n;
  io::println(Str(p.f) + " " + Str(c.g) + " " + Str(xs[0]) + " " + Str(xs[1])
      + " " + Str(v));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "112 255 9223372036854775808 4 0\n");
  g.expectNoLeaks("Bitwise.CompoundAssignmentsOnEveryTarget");
}

// The value is evaluated before the count, and still evaluated when a
// constant count makes the shift 0.
TEST(Bitwise, EvaluationOrder) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
fn f() -> u8 { io::println("f"); return 1; }
fn g() -> unsigned { io::println("g"); return 3; }
fn h() -> u8 { io::println("h"); return 6; }
fn main() -> int {
  io::println(Str(f() << g()));
  io::println(Str(f() >> g()));
  io::println(Str(f() | h()));
  io::println(Str(h() & f()));
  io::println(Str(f() << 8));
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "f\ng\n8\nf\ng\n0\nf\nh\n7\nh\nf\n0\nf\n0\n");
  g.expectNoLeaks("Bitwise.EvaluationOrder");
}

// A count masked with `& 63` or reduced `% 64` is below 64 (no guard), and
// the rotate idiom built on it.
TEST(Bitwise, MaskedCountsAndRotate) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
fn rotl(x: unsigned, k: unsigned) -> unsigned
    => (x << (k & 63)) | (x >> ((0 - k) & 63));
fn rotr(x: unsigned, k: unsigned) -> unsigned
    => (x >> (k % 64)) | (x << ((64 - k % 64) & 63));
fn main() -> int {
  let x: unsigned = 0x8000_0000_0000_0001;
  let ks: unsigned[5] = [0, 1, 63, 64, 65];
  for i in 0..5 {
    io::println(Str(rotl(x, ks[i])) + " " + Str(rotr(x, ks[i])));
  }
  let ns: unsigned[3] = [3, 64, 65];
  let ms: unsigned[3] = [3, 64, 127];
  for i in 0..3 {
    io::println(Str(x << (ns[i] & 63)) + " " + Str(x >> (ms[i] % 64)));
  }
  return 0;
}
)");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9223372036854775809 9223372036854775809\n"
                      "3 13835058055282163712\n"
                      "13835058055282163712 3\n"
                      "9223372036854775809 9223372036854775809\n"
                      "3 13835058055282163712\n"
                      "8 1152921504606846976\n"
                      "9223372036854775809 9223372036854775809\n"
                      "2 1\n");
  g.expectNoLeaks("Bitwise.MaskedCountsAndRotate");
}
