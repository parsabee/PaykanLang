// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for the runtime functions behind the sized numeric types
// (docs/design/sized-types.md): `unsigned` printing and parsing, and the
// f16 rounding.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

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
  for (const char *bad : {"", "-1", "+1", " 1", "1 ", "18446744073709551616",
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
