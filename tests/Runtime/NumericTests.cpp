// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the runtime functions behind the sized numeric types
// (docs/design/sized-types.md): `unsigned` printing and parsing, the f32 and
// f16 parses (`s.toF32()`, `s.toF16()`), and the f16 rounding.

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <utility>

extern "C" {
#include "Runtime.h"
}

namespace {

std::string fromUint(int64_t bits) {
  PaykanString *s = PaykanString_from_uint(bits);
  std::string text(s->data, static_cast<size_t>(s->len));
  PaykanString_destroy((PaykanObject *)s);
  return text;
}

/// The parse of @p text as an unsigned: {ok, bits}.
std::pair<int64_t, int64_t> parseUint(const std::string &text) {
  PaykanString *s =
      PaykanString_new(text.data(), static_cast<int64_t>(text.size()));
  int64_t out = -1;
  int64_t ok = PaykanString_parse_uint((PaykanObject *)s, &out);
  PaykanString_destroy((PaykanObject *)s);
  return {ok, out};
}

/// The parse of @p text by @p parse (PaykanString_parse_f32 or _f16): {ok,
/// value}.
std::pair<int64_t, double> parseFloatWith(int64_t (*parse)(PaykanObject *,
                                                           double *),
                                          const std::string &text) {
  PaykanString *s =
      PaykanString_new(text.data(), static_cast<int64_t>(text.size()));
  double out = -1.0;
  int64_t ok = parse((PaykanObject *)s, &out);
  PaykanString_destroy((PaykanObject *)s);
  return {ok, out};
}

/// The f32 parse of @p text, which must be accepted.
double f32Of(const std::string &text) {
  auto [ok, out] = parseFloatWith(PaykanString_parse_f32, text);
  EXPECT_EQ(ok, 1) << text;
  return out;
}

/// The f16 parse of @p text, which must be accepted.
double f16Of(const std::string &text) {
  auto [ok, out] = parseFloatWith(PaykanString_parse_f16, text);
  EXPECT_EQ(ok, 1) << text;
  return out;
}

/// Whether @p parse refuses @p text, storing 0.
bool refused(int64_t (*parse)(PaykanObject *, double *),
             const std::string &text) {
  auto [ok, out] = parseFloatWith(parse, text);
  return ok == 0 && out == 0.0 && !std::signbit(out);
}

} // namespace

TEST(Numeric, FromUintPrintsTheBitsUnsigned) {
  EXPECT_EQ(fromUint(0), "0");
  EXPECT_EQ(fromUint(7), "7");
  EXPECT_EQ(fromUint(-1), "18446744073709551615");
  EXPECT_EQ(fromUint(INT64_MIN), "9223372036854775808");
  EXPECT_EQ(fromUint(INT64_MAX), "9223372036854775807");
}

TEST(Numeric, ParseUintAcceptsDigitsWithinUint64) {
  EXPECT_EQ(parseUint("0"), (std::pair<int64_t, int64_t>{1, 0}));
  EXPECT_EQ(parseUint("18446744073709551615"),
            (std::pair<int64_t, int64_t>{1, -1}));
  EXPECT_EQ(parseUint("9223372036854775808"),
            (std::pair<int64_t, int64_t>{1, INT64_MIN}));
  EXPECT_EQ(parseUint("007"), (std::pair<int64_t, int64_t>{1, 7}));
}

TEST(Numeric, ParseUintRejectsSignsSpacesAndOverflow) {
  for (const char *bad :
       {"", "-1", "+1", "-0", "+0", " 1", "1 ", "18446744073709551616",
        "99999999999999999999", "1.5", "abc", "0x10"}) {
    auto [ok, out] = parseUint(bad);
    EXPECT_EQ(ok, 0) << bad;
    EXPECT_EQ(out, 0) << bad;
  }
}

TEST(Numeric, RoundF16RoundsToNearestEven) {
  EXPECT_EQ(Paykan_round_f16(0.0), 0.0);
  EXPECT_EQ(Paykan_round_f16(1.0), 1.0);
  EXPECT_EQ(Paykan_round_f16(-1.5), -1.5);
  // 0.1 is 0x2E66 in f16: 0.0999755859375.
  EXPECT_DOUBLE_EQ(Paykan_round_f16(0.1), 0.0999755859375);
  // Above 2048 the ulp is 2: 2049 is a tie, rounded to the even 2048; 2051
  // is a tie too, rounded to 2052.
  EXPECT_EQ(Paykan_round_f16(2049.0), 2048.0);
  EXPECT_EQ(Paykan_round_f16(2050.0), 2050.0);
  EXPECT_EQ(Paykan_round_f16(2051.0), 2052.0);
  EXPECT_EQ(Paykan_round_f16(2049.5), 2050.0);
  // The largest finite f16 and the overflow threshold.
  EXPECT_EQ(Paykan_round_f16(65504.0), 65504.0);
  EXPECT_EQ(Paykan_round_f16(65519.0), 65504.0);
  EXPECT_EQ(Paykan_round_f16(65519.999), 65504.0);
  EXPECT_TRUE(std::isinf(Paykan_round_f16(65520.0)));
  EXPECT_GT(Paykan_round_f16(65520.0), 0.0);
  EXPECT_TRUE(std::isinf(Paykan_round_f16(-70000.0)));
  EXPECT_LT(Paykan_round_f16(-70000.0), 0.0);
  EXPECT_TRUE(std::isinf(Paykan_round_f16(1e300)));
}

TEST(Numeric, RoundF16KeepsSubnormalsAndSpecials) {
  const double minSubnormal = std::ldexp(1.0, -24);
  const double minNormal = std::ldexp(1.0, -14);
  EXPECT_EQ(Paykan_round_f16(minSubnormal), minSubnormal);
  EXPECT_EQ(Paykan_round_f16(minSubnormal * 0.5), 0.0); // a tie, to even
  EXPECT_EQ(Paykan_round_f16(minSubnormal * 0.75), minSubnormal);
  EXPECT_EQ(Paykan_round_f16(minSubnormal * 1.5), minSubnormal * 2);
  EXPECT_EQ(Paykan_round_f16(minNormal), minNormal);
  EXPECT_EQ(Paykan_round_f16(minNormal * 1.0001), minNormal);
  EXPECT_EQ(Paykan_round_f16(-minSubnormal * 0.75), -minSubnormal);
  EXPECT_TRUE(std::isnan(Paykan_round_f16(std::nan(""))));
  EXPECT_TRUE(std::isinf(Paykan_round_f16(INFINITY)));
  EXPECT_EQ(std::signbit(Paykan_round_f16(-0.0)), true);
}

// `f32<int>` and `f32<unsigned>`: the integer rounded once to the nearest
// f32, ties to even (new-F3c-2), where its f64 rounded again can be the
// other neighbour.
TEST(Numeric, IntToF32RoundsOnce) {
  // 2^60 + 2^36 + 1: just above the midpoint 2^60 + 2^36 between 2^60 and
  // 2^60 + 2^37, which is the f64 of it (so twice rounded is 2^60).
  const int64_t n = (INT64_C(1) << 60) + (INT64_C(1) << 36) + 1;
  EXPECT_EQ(Paykan_int_to_f32(n), std::ldexp(1.0, 60) + std::ldexp(1.0, 37));
  EXPECT_EQ(static_cast<double>(static_cast<float>(static_cast<double>(n))),
            std::ldexp(1.0, 60));
  EXPECT_EQ(Paykan_int_to_f32(-n),
            -(std::ldexp(1.0, 60) + std::ldexp(1.0, 37)));
  EXPECT_EQ(Paykan_int_to_f32(INT64_MIN), -std::ldexp(1.0, 63));
  EXPECT_EQ(Paykan_int_to_f32(INT64_MAX), std::ldexp(1.0, 63));
  EXPECT_EQ(Paykan_int_to_f32(16777217), 16777216.0); // a tie, to even
  EXPECT_EQ(Paykan_int_to_f32(16777219), 16777220.0); // a tie, to even
  EXPECT_EQ(Paykan_int_to_f32(-3), -3.0);
  EXPECT_EQ(Paykan_int_to_f32(0), 0.0);
  EXPECT_FALSE(std::signbit(Paykan_int_to_f32(0)));
  // The uint64 of the bits: 2^64 - 1 is 2^64; 2^63 + 2^39 + 1 is
  // 2^63 + 2^40 (its f64, 2^63 + 2^39, is the midpoint, rounded to 2^63).
  EXPECT_EQ(Paykan_uint_to_f32(-1), std::ldexp(1.0, 64));
  const uint64_t u = (UINT64_C(1) << 63) + (UINT64_C(1) << 39) + 1;
  EXPECT_EQ(Paykan_uint_to_f32(static_cast<int64_t>(u)),
            std::ldexp(1.0, 63) + std::ldexp(1.0, 40));
  EXPECT_EQ(static_cast<double>(static_cast<float>(static_cast<double>(u))),
            std::ldexp(1.0, 63));
  EXPECT_EQ(Paykan_uint_to_f32(7), 7.0);
}

TEST(Numeric, ParseF32RoundsTheFloatOfTheText) {
  EXPECT_EQ(f32Of("0.1"), static_cast<double>(0.1f));
  EXPECT_EQ(f32Of("2.5"), 2.5);
  // The largest f32 and the text of it that rounds to it.
  EXPECT_EQ(f32Of("3.4028235e38"), static_cast<double>(FLT_MAX));
  EXPECT_EQ(f32Of("-3.4028235e38"), -static_cast<double>(FLT_MAX));
  // Below the f32 range: 0 (or -0), as an f32 literal is.
  EXPECT_EQ(f32Of("1e-46"), 0.0);
  EXPECT_FALSE(std::signbit(f32Of("1e-46")));
  EXPECT_EQ(f32Of("-1e-50"), 0.0);
  EXPECT_TRUE(std::signbit(f32Of("-1e-50")));
  // The smallest f32 subnormal is kept.
  EXPECT_EQ(f32Of("1.401298464324817e-45"), std::ldexp(1.0, -149));
  EXPECT_TRUE(std::signbit(f32Of("-0")));
  EXPECT_TRUE(std::signbit(f32Of("-0.0")));
  // The documented double rounding: the f64 nearest the text, then the f32
  // nearest that (1 + 2^-24, a tie, to even), where one rounding of the text
  // gives 1 + 2^-23.
  EXPECT_EQ(f32Of("1.0000000596046448"), 1.0);
  // An integer text beyond 2^53 is rounded once: 2^60 + 2^36 + 1 is above the
  // midpoint 2^60 + 2^36 between the f32s 2^60 and 2^60 + 2^37.
  EXPECT_EQ(f32Of("1152921573326323713"),
            std::ldexp(1.0, 60) + std::ldexp(1.0, 37));
  EXPECT_EQ(f32Of("-1152921573326323713"),
            -(std::ldexp(1.0, 60) + std::ldexp(1.0, 37)));
  EXPECT_EQ(f32Of("9223372036854775807"), std::ldexp(1.0, 63));
  EXPECT_EQ(f32Of("16777217"), 16777216.0);
}

TEST(Numeric, ParseF32KeepsInfAndNanAndRefusesOverflow) {
  EXPECT_TRUE(std::isinf(f32Of("inf")));
  EXPECT_GT(f32Of("inf"), 0.0);
  EXPECT_TRUE(std::isinf(f32Of("-inf")));
  EXPECT_LT(f32Of("-inf"), 0.0);
  EXPECT_TRUE(std::isnan(f32Of("nan")));
  // Finite text that rounds to infinity in f32 (from the midpoint above
  // FLT_MAX, 2^128 - 2^103, which is a tie rounded to even, that is up).
  for (const char *big : {"1e39", "-1e39", "3.4028235677973366e38",
                          "-3.4028235677973366e38", "1e300"})
    EXPECT_TRUE(refused(PaykanString_parse_f32, big)) << big;
  // What the f64 parse refuses (overflow, underflow to 0, not a number).
  for (const char *bad : {"1e999", "1e-400", "", " 1", "1 ", "x", "1.5f"})
    EXPECT_TRUE(refused(PaykanString_parse_f32, bad)) << bad;
}

TEST(Numeric, ParseF16RoundsTheFloatOfTheText) {
  EXPECT_EQ(f16Of("0.1"), 0.0999755859375);
  EXPECT_EQ(f16Of("65519"), 65504.0);
  EXPECT_EQ(f16Of("65504"), 65504.0);
  EXPECT_EQ(f16Of("-65519.99"), -65504.0);
  // 1 + 2^-11 is the tie between 1 and 1 + 2^-10, broken to the even 1.
  EXPECT_EQ(f16Of("1.00048828125"), 1.0);
  // Below the f16 range: 0 (or -0), as an f16 literal is.
  EXPECT_EQ(f16Of("1e-9"), 0.0);
  EXPECT_FALSE(std::signbit(f16Of("1e-9")));
  EXPECT_TRUE(std::signbit(f16Of("-1e-9")));
  EXPECT_EQ(f16Of("6e-8"), std::ldexp(1.0, -24)); // the smallest subnormal
  EXPECT_TRUE(std::signbit(f16Of("-0")));
  EXPECT_TRUE(std::isinf(f16Of("inf")));
  EXPECT_TRUE(std::isinf(f16Of("-Infinity")));
  EXPECT_TRUE(std::isnan(f16Of("nan")));
}

TEST(Numeric, ParseF16RefusesFiniteTextThatRoundsToInfinity) {
  for (const char *big :
       {"70000", "65520", "-65520", "65520.0", "1e5", "1e300"})
    EXPECT_TRUE(refused(PaykanString_parse_f16, big)) << big;
  for (const char *bad : {"1e999", "1e-400", "", " 1", "1 ", "x"})
    EXPECT_TRUE(refused(PaykanString_parse_f16, bad)) << bad;
}
