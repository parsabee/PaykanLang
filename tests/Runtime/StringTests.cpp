// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PaykanString runtime functions.

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"
#include "VTableTestHelper.h"

// -- PaykanString_new

TEST(StringNew, StoresDataAndLength) {
  PaykanString *s = PaykanString_new("hello", 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->len, 5);
  EXPECT_STREQ(s->data, "hello");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringNew, NulTerminated) {
  PaykanString *s = PaykanString_new("abc", 3);
  EXPECT_EQ(s->data[3], '\0');
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringNew, EmptyString) {
  PaykanString *s = PaykanString_new("", 0);
  EXPECT_EQ(s->len, 0);
  EXPECT_STREQ(s->data, "");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringNew, VtableIsStringVtable) {
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ((void *)s->vtable, (void *)&PaykanString_vtable);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringNew, EmbeddedNulBytes) {
  // Paykan strings are length-delimited; internal NULs are valid.
  PaykanString *s = PaykanString_new("a\0b", 3);
  EXPECT_EQ(s->len, 3);
  EXPECT_EQ(s->data[0], 'a');
  EXPECT_EQ(s->data[1], '\0');
  EXPECT_EQ(s->data[2], 'b');
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_from_int / from_float / from_bool

TEST(StringFromInt, Zero) {
  PaykanString *s = PaykanString_from_int(0);
  EXPECT_STREQ(s->data, "0");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromInt, Positive) {
  PaykanString *s = PaykanString_from_int(42);
  EXPECT_STREQ(s->data, "42");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromInt, Negative) {
  PaykanString *s = PaykanString_from_int(-7);
  EXPECT_STREQ(s->data, "-7");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromFloat, BasicValue) {
  PaykanString *s = PaykanString_from_float(3.14);
  ASSERT_NE(s, nullptr);
  // %g formatting — just confirm it contains "3.14".
  EXPECT_NE(std::string(s->data).find("3.14"), std::string::npos);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromFloat, Zero) {
  PaykanString *s = PaykanString_from_float(0.0);
  EXPECT_STREQ(s->data, "0");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromBool, True) {
  PaykanString *s = PaykanString_from_bool(1);
  EXPECT_STREQ(s->data, "True");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromBool, False) {
  PaykanString *s = PaykanString_from_bool(0);
  EXPECT_STREQ(s->data, "False");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringFromBool, NonZeroIsTrue) {
  PaykanString *s = PaykanString_from_bool(99);
  EXPECT_STREQ(s->data, "True");
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_toString

TEST(StringToString, ReturnsFreshCopy) {
  PaykanString *s = PaykanString_new("hi", 2);
  PaykanShared *shared = PaykanString_toString((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_NE(copy, s);
  EXPECT_STREQ(copy->data, "hi");
  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringToString, CopyIsIndependent) {
  PaykanString *s = PaykanString_new("hi", 2);
  PaykanShared *shared = PaykanString_toString((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_NE(copy->data, s->data);
  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_equals

TEST(StringEquals, SameContent) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("foo", 3);
  EXPECT_EQ(paykanTestEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            1);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringEquals, DifferentContent) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  EXPECT_EQ(paykanTestEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            0);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringEquals, DifferentLength) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("fo", 2);
  EXPECT_EQ(paykanTestEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            0);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringEquals, EmptyStrings) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("", 0);
  EXPECT_EQ(paykanTestEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            1);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringEquals, NonStringOtherFallsBackToIdentity) {
  PaykanString *s = PaykanString_new("x", 1);
  PaykanObject *obj = PaykanObject_new();
  // Different types -> identity comparison -> not equal.
  EXPECT_EQ(paykanTestEquals(PaykanString_equals, (PaykanObject *)s, obj), 0);
  Paykan_dispose((PaykanObject *)s);
  Paykan_dispose(obj);
}

// -- PaykanString_length

TEST(StringLength, ReturnsLen) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_length((PaykanObject *)s), 5);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringLength, EmptyIsZero) {
  PaykanString *s = PaykanString_new("", 0);
  EXPECT_EQ(PaykanString_length((PaykanObject *)s), 0);
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_concat

TEST(StringConcat, TwoStrings) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString *c =
      (PaykanString *)PaykanString_concat((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->len, 6);
  EXPECT_STREQ(c->data, "foobar");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
  Paykan_dispose((PaykanObject *)c);
}

TEST(StringConcat, WithEmptyLeft) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString *c =
      (PaykanString *)PaykanString_concat((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_STREQ(c->data, "bar");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
  Paykan_dispose((PaykanObject *)c);
}

TEST(StringConcat, WithEmptyRight) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString *c =
      (PaykanString *)PaykanString_concat((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_STREQ(c->data, "foo");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
  Paykan_dispose((PaykanObject *)c);
}

TEST(StringConcat, BothEmpty) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString *c =
      (PaykanString *)PaykanString_concat((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->len, 0);
  EXPECT_STREQ(c->data, "");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
  Paykan_dispose((PaykanObject *)c);
}

TEST(StringConcat, ResultIsNulTerminated) {
  PaykanString *a = PaykanString_new("ab", 2);
  PaykanString *b = PaykanString_new("cd", 2);
  PaykanString *c =
      (PaykanString *)PaykanString_concat((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->data[4], '\0');
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
  Paykan_dispose((PaykanObject *)c);
}

// -- Vtable dispatch

TEST(StringVtable, ToStringViaVtable) {
  PaykanString *s = PaykanString_new("test", 4);
  PaykanShared *shared = vtToString(s)((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_STREQ(copy->data, "test");
  Paykan_release(shared);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringVtable, LengthViaVtable) {
  PaykanString *s = PaykanString_new("hello", 5);
  auto length = vtSlot<PaykanLengthFn>(s, PAYKAN_SLOT_STRING_LENGTH);
  EXPECT_EQ(length((PaykanObject *)s), 5);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringVtable, DestroyViaVtable) {
  PaykanString *s = PaykanString_new("bye", 3);
  vtDestroy(s)((PaykanObject *)s);
  // Should not crash.
}

// -- PaykanString_at

TEST(StringAt, FirstChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 0);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->len, 1);
  EXPECT_EQ(ch->data[0], 'h');
  Paykan_release(sh);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, LastChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 4);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[0], 'o');
  Paykan_release(sh);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, MiddleChar) {
  PaykanString *s = PaykanString_new("paykan", 6);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 3);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[0], 'k');
  EXPECT_EQ(ch->len, 1);
  Paykan_release(sh);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, ResultIsNulTerminated) {
  PaykanString *s = PaykanString_new("abc", 3);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 1);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[1], '\0');
  Paykan_release(sh);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, ResultHasStringVtable) {
  PaykanString *s = PaykanString_new("xyz", 3);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 0);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ((void *)ch->vtable, (void *)&PaykanString_vtable);
  Paykan_release(sh);
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, ReturnsOwnedShared) {
  // Verify the returned PaykanShared* has an independent lifetime.
  PaykanString *s = PaykanString_new("hi", 2);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 0);
  EXPECT_NE(sh, nullptr);
  EXPECT_NE(PaykanShared_get(sh), nullptr);
  Paykan_release(sh);
  // Original string is still alive and untouched.
  EXPECT_STREQ(s->data, "hi");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, OutOfBoundsDiesInDebug) {
  PaykanString *s = PaykanString_new("ab", 2);
  EXPECT_DEATH(PaykanString_at((PaykanObject *)s, 2), "");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringAt, NegativeIndexDies) {
  PaykanString *s = PaykanString_new("ab", 2);
  EXPECT_DEATH(PaykanString_at((PaykanObject *)s, -1), "");
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_char_at

TEST(StringCharAt, FirstChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 0), 'h');
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringCharAt, LastChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 4), 'o');
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringCharAt, MidChar) {
  PaykanString *s = PaykanString_new("paykan", 6);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 3), 'k');
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringCharAt, SingleChar) {
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 0), 'x');
  Paykan_dispose((PaykanObject *)s);
}

// -- PaykanString_concat_inplace

TEST(StringConcatInplace, AppendToEmpty) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("hello", 5);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 5);
  EXPECT_STREQ(a->data, "hello");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringConcatInplace, AppendNonEmpty) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 6);
  EXPECT_STREQ(a->data, "foobar");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringConcatInplace, AppendEmpty) {
  PaykanString *a = PaykanString_new("hello", 5);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 5);
  EXPECT_STREQ(a->data, "hello");
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(StringConcatInplace, ResultIsNulTerminated) {
  PaykanString *a = PaykanString_new("ab", 2);
  PaykanString *b = PaykanString_new("cd", 2);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->data[4], '\0');
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

// -- PaykanString_parse_int / _float / _bool (Str's `toInt()` & co.)

namespace {

/// Parse @p text (of @p len bytes) with @p parse; returns whether it was
/// valid and stores the value in @p out.
template <typename T>
bool parseWith(int64_t (*parse)(PaykanObject *, T *), const char *text,
               int64_t len, T &out) {
  PaykanString *s = PaykanString_new(text, len);
  out = static_cast<T>(42); // overwritten either way
  int64_t ok = parse((PaykanObject *)s, &out);
  Paykan_dispose((PaykanObject *)s);
  return ok != 0;
}

} // namespace

TEST(StringParse, IntValidAndLimits) {
  int64_t v;
  EXPECT_TRUE(parseWith(PaykanString_parse_int, "-7", 2, v));
  EXPECT_EQ(v, -7);
  EXPECT_TRUE(parseWith(PaykanString_parse_int, "9223372036854775807", 19, v));
  EXPECT_EQ(v, INT64_MAX);
  EXPECT_TRUE(parseWith(PaykanString_parse_int, "-9223372036854775808", 20, v));
  EXPECT_EQ(v, INT64_MIN);
}

TEST(StringParse, IntInvalidStoresZero) {
  int64_t v;
  for (const char *text : {"", "abc", "12x", " 12", "12 "}) {
    EXPECT_FALSE(parseWith(PaykanString_parse_int, text,
                           static_cast<int64_t>(std::strlen(text)), v))
        << text;
    EXPECT_EQ(v, 0) << text;
  }
  EXPECT_FALSE(parseWith(PaykanString_parse_int, "1\0002", 4, v));
  EXPECT_FALSE(parseWith(PaykanString_parse_int, "9223372036854775808", 19, v));
}

TEST(StringParse, Float) {
  double d;
  EXPECT_TRUE(parseWith(PaykanString_parse_float, "2.5", 3, d));
  EXPECT_EQ(d, 2.5);
  EXPECT_TRUE(parseWith(PaykanString_parse_float, "-inf", 4, d));
  EXPECT_TRUE(d < 0 && d * 0 != 0); // -inf
  EXPECT_FALSE(parseWith(PaykanString_parse_float, "1e999", 5, d));
  EXPECT_EQ(d, 0.0);
  EXPECT_FALSE(parseWith(PaykanString_parse_float, " 1", 2, d));
}

// The range rule (docs/language/02-basics.md): a value that
// rounds to a finite float, subnormals included, is accepted; one too large
// (rounds to +-inf) or too small (nonzero, rounds to 0) is None.  strtod
// flags subnormal results with ERANGE too, which must not make them None.
TEST(StringParse, FloatRange) {
  struct Case {
    const char *text;
    double value;
  } ok[] = {{"1e-310", 1e-310},
            {"5e-324", 4.9406564584124654e-324},
            {"-5e-324", -4.9406564584124654e-324},
            {"4.9406564584124654e-324", 4.9406564584124654e-324},
            {"2.2250738585072009e-308", 2.2250738585072009e-308},
            {"2.2250738585072014e-308", 2.2250738585072014e-308},
            {"1.7976931348623157e308", 1.7976931348623157e308},
            {"0e-999", 0.0},
            {"-0.0", -0.0},
            {"nan", 0.0}};
  for (const auto &c : ok) {
    double v;
    ASSERT_TRUE(parseWith(PaykanString_parse_float, c.text,
                          static_cast<int64_t>(std::strlen(c.text)), v))
        << c.text;
    if (std::string(c.text) == "nan") {
      EXPECT_NE(v, v);
      continue;
    }
    EXPECT_EQ(v, c.value) << c.text;
    EXPECT_EQ(std::signbit(v), std::signbit(c.value)) << c.text;
  }
  for (const char *bad :
       {"1e999", "-1e999", "1.8e308", "1e-400", "-1e-400", "2e-324"}) {
    double v;
    EXPECT_FALSE(parseWith(PaykanString_parse_float, bad,
                           static_cast<int64_t>(std::strlen(bad)), v))
        << bad;
  }
}

TEST(StringParse, BoolAcceptsExactlyTrueAndFalse) {
  int64_t b;
  EXPECT_TRUE(parseWith(PaykanString_parse_bool, "True", 4, b));
  EXPECT_EQ(b, 1);
  EXPECT_TRUE(parseWith(PaykanString_parse_bool, "False", 5, b));
  EXPECT_EQ(b, 0);
  for (const char *text : {"true", "TRUE", "1", "", "Truex"}) {
    EXPECT_FALSE(parseWith(PaykanString_parse_bool, text,
                           static_cast<int64_t>(std::strlen(text)), b))
        << text;
    EXPECT_EQ(b, 0) << text;
  }
}

// -- PaykanString_hash / PaykanInt_hash

namespace {
int64_t hashOf(const char *data, int64_t len) {
  PaykanString *s = PaykanString_new(data, len);
  int64_t h = PaykanString_hash((PaykanObject *)s);
  Paykan_dispose((PaykanObject *)s);
  return h;
}
} // namespace

TEST(StringHash, EqualBytesHashAlike) {
  EXPECT_EQ(hashOf("hello", 5), hashOf("hello", 5));
  EXPECT_NE(hashOf("hello", 5), hashOf("hellp", 5));
  EXPECT_NE(hashOf("ab", 2), hashOf("ba", 2));
  // The length counts: an embedded NUL is a byte like any other.
  EXPECT_NE(hashOf("", 0), hashOf("\0", 1));
  EXPECT_NE(hashOf("a", 1), hashOf("a\0", 2));
}

TEST(IntHash, IsDeterministicAndMixed) {
  EXPECT_EQ(PaykanInt_hash(42), PaykanInt_hash(42));
  // Consecutive values spread over the low bits a table indexes by.
  int seen[16] = {0};
  for (int64_t n = 0; n < 1600; ++n)
    ++seen[PaykanInt_hash(n) & 15];
  for (int bucket : seen) {
    EXPECT_GT(bucket, 60);
    EXPECT_LT(bucket, 140);
  }
}

// -- PaykanString_slice / PaykanString_find (::collections's string functions)

namespace {

/// The bytes of @p text from @p from up to @p to, by PaykanString_slice.
std::string sliceOf(const std::string &text, int64_t from, int64_t to) {
  PaykanString *s =
      PaykanString_new(text.data(), static_cast<int64_t>(text.size()));
  PaykanShared *r = PaykanString_slice((PaykanObject *)s, from, to);
  const auto *rs = (const PaykanString *)PaykanShared_get(r);
  EXPECT_EQ(rs->data[rs->len], '\0');
  std::string out(rs->data, static_cast<size_t>(rs->len));
  Paykan_release(r);
  Paykan_dispose((PaykanObject *)s);
  return out;
}

/// PaykanString_find of @p needle in @p text from @p from.
int64_t findIn(const std::string &text, const std::string &needle,
               int64_t from) {
  PaykanString *s =
      PaykanString_new(text.data(), static_cast<int64_t>(text.size()));
  PaykanString *n =
      PaykanString_new(needle.data(), static_cast<int64_t>(needle.size()));
  int64_t at = PaykanString_find((PaykanObject *)s, (PaykanObject *)n, from);
  Paykan_dispose((PaykanObject *)n);
  Paykan_dispose((PaykanObject *)s);
  return at;
}

} // namespace

TEST(StringSlice, TakesTheBytesFromUpTo) {
  EXPECT_EQ(sliceOf("hello", 0, 5), "hello");
  EXPECT_EQ(sliceOf("hello", 1, 4), "ell");
  EXPECT_EQ(sliceOf("hello", 4, 5), "o");
  // An empty slice anywhere from 0 to the length, of an empty string too.
  EXPECT_EQ(sliceOf("hello", 0, 0), "");
  EXPECT_EQ(sliceOf("hello", 2, 2), "");
  EXPECT_EQ(sliceOf("hello", 5, 5), "");
  EXPECT_EQ(sliceOf("", 0, 0), "");
  // Bytes, an embedded NUL among them.
  EXPECT_EQ(sliceOf(std::string("a\0b", 3), 1, 3), std::string("\0b", 2));
}

TEST(StringSlice, IsANewStringOwnedByTheCaller) {
  PaykanString *s = PaykanString_new("abc", 3);
  PaykanShared *r = PaykanString_slice((PaykanObject *)s, 0, 3);
  PaykanObject *o = PaykanShared_get(r);
  EXPECT_NE(o, (PaykanObject *)s);
  EXPECT_EQ((void *)o->vtable, (void *)&PaykanString_vtable);
  EXPECT_EQ(o->strong, 1);
  EXPECT_NE(((PaykanString *)o)->data, s->data);
  Paykan_release(r);
  EXPECT_STREQ(s->data, "abc");
  Paykan_dispose((PaykanObject *)s);
}

TEST(StringSliceDeath, OutOfBoundsPanics) {
  PaykanString *s = PaykanString_new("abc", 3);
  auto *o = (PaykanObject *)s;
  EXPECT_DEATH(PaykanString_slice(o, 2, 7),
               "string slice 2\\.\\.7 out of bounds \\(len=3\\)");
  EXPECT_DEATH(PaykanString_slice(o, 4, 4),
               "string slice 4\\.\\.4 out of bounds \\(len=3\\)");
  // A slice that ends before it starts.
  EXPECT_DEATH(PaykanString_slice(o, 2, 1),
               "string slice 2\\.\\.1 out of bounds \\(len=3\\)");
  Paykan_dispose(o);
}

// An index is the standard library's `unsigned`, in an int64_t: one of 2^63
// or more is negative here, and a panic prints it as the program wrote it.
TEST(StringSliceDeath, AnIndexPastINT64MaxPanicsUnsigned) {
  PaykanString *s = PaykanString_new("abc", 3);
  auto *o = (PaykanObject *)s;
  EXPECT_DEATH(PaykanString_slice(o, 1, -1),
               "string slice 1\\.\\.18446744073709551615 out of bounds "
               "\\(len=3\\)");
  EXPECT_DEATH(PaykanString_slice(o, INT64_MIN, 2),
               "string slice 9223372036854775808\\.\\.2 out of bounds "
               "\\(len=3\\)");
  Paykan_dispose(o);
}

TEST(StringFind, FirstIndexFromOn) {
  EXPECT_EQ(findIn("a,b,c", ",", 0), 1);
  EXPECT_EQ(findIn("a,b,c", ",", 1), 1);
  EXPECT_EQ(findIn("a,b,c", ",", 2), 3);
  EXPECT_EQ(findIn("a,b,c", ",", 4), -1);
  EXPECT_EQ(findIn("hello", "hello", 0), 0);
  EXPECT_EQ(findIn("hello", "lo", 0), 3);
  EXPECT_EQ(findIn("hello", "x", 0), -1);
  // The first byte matches before the whole needle does.
  EXPECT_EQ(findIn("aaab", "aab", 0), 1);
  EXPECT_EQ(findIn("abcabd", "abd", 0), 3);
  // Occurrences may overlap: each search starts where it is told.
  EXPECT_EQ(findIn("aaa", "aa", 0), 0);
  EXPECT_EQ(findIn("aaa", "aa", 1), 1);
  EXPECT_EQ(findIn("aaa", "aa", 2), -1);
  // Bytes, an embedded NUL among them.
  EXPECT_EQ(findIn(std::string("a\0b\0c", 5), std::string("\0c", 2), 0), 3);
}

TEST(StringFind, ANeedleLongerThanWhatIsLeftIsNotFound) {
  EXPECT_EQ(findIn("abc", "abcd", 0), -1);
  EXPECT_EQ(findIn("abc", "bc", 2), -1);
  EXPECT_EQ(findIn("", "a", 0), -1);
}

TEST(StringFind, AnEmptyNeedleIsFoundWhereTheSearchStarts) {
  EXPECT_EQ(findIn("abc", "", 0), 0);
  EXPECT_EQ(findIn("abc", "", 2), 2);
  EXPECT_EQ(findIn("abc", "", 3), 3);
  EXPECT_EQ(findIn("", "", 0), 0);
}

// From past the end, an `unsigned` of 2^63 or more included, nothing is
// found: there is no index there.
TEST(StringFind, FromPastTheEndFindsNothing) {
  EXPECT_EQ(findIn("abc", "", 4), -1);
  EXPECT_EQ(findIn("abc", "a", 4), -1);
  EXPECT_EQ(findIn("abc", "", -1), -1);
  EXPECT_EQ(findIn("abc", "a", INT64_MIN), -1);
}
