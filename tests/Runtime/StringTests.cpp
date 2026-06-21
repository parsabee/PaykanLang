// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanString runtime functions.

#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

// ============================================================================
// PaykanString_new
// ============================================================================

TEST(StringNew, StoresDataAndLength) {
  PaykanString *s = PaykanString_new("hello", 5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->len, 5);
  EXPECT_STREQ(s->data, "hello");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringNew, NulTerminated) {
  PaykanString *s = PaykanString_new("abc", 3);
  EXPECT_EQ(s->data[3], '\0');
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringNew, EmptyString) {
  PaykanString *s = PaykanString_new("", 0);
  EXPECT_EQ(s->len, 0);
  EXPECT_STREQ(s->data, "");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringNew, VtableIsStringVtable) {
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ((void *)s->vtable, (void *)&PaykanString_vtable);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringNew, EmbeddedNulBytes) {
  // Paykan strings are length-delimited; internal NULs are valid.
  PaykanString *s = PaykanString_new("a\0b", 3);
  EXPECT_EQ(s->len, 3);
  EXPECT_EQ(s->data[0], 'a');
  EXPECT_EQ(s->data[1], '\0');
  EXPECT_EQ(s->data[2], 'b');
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_from_int / from_float / from_bool
// ============================================================================

TEST(StringFromInt, Zero) {
  PaykanString *s = PaykanString_from_int(0);
  EXPECT_STREQ(s->data, "0");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromInt, Positive) {
  PaykanString *s = PaykanString_from_int(42);
  EXPECT_STREQ(s->data, "42");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromInt, Negative) {
  PaykanString *s = PaykanString_from_int(-7);
  EXPECT_STREQ(s->data, "-7");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromFloat, BasicValue) {
  PaykanString *s = PaykanString_from_float(3.14);
  ASSERT_NE(s, nullptr);
  // %g formatting — just confirm it contains "3.14".
  EXPECT_NE(std::string(s->data).find("3.14"), std::string::npos);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromFloat, Zero) {
  PaykanString *s = PaykanString_from_float(0.0);
  EXPECT_STREQ(s->data, "0");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromBool, True) {
  PaykanString *s = PaykanString_from_bool(1);
  EXPECT_STREQ(s->data, "True");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromBool, False) {
  PaykanString *s = PaykanString_from_bool(0);
  EXPECT_STREQ(s->data, "False");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringFromBool, NonZeroIsTrue) {
  PaykanString *s = PaykanString_from_bool(99);
  EXPECT_STREQ(s->data, "True");
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_toString
// ============================================================================

TEST(StringToString, ReturnsFreshCopy) {
  PaykanString *s = PaykanString_new("hi", 2);
  PaykanShared *shared = PaykanString_toString((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_NE(copy, s);
  EXPECT_STREQ(copy->data, "hi");
  Paykan_release(shared);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringToString, CopyIsIndependent) {
  PaykanString *s = PaykanString_new("hi", 2);
  PaykanShared *shared = PaykanString_toString((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_NE(copy->data, s->data);
  Paykan_release(shared);
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_equals
// ============================================================================

TEST(StringEquals, SameContent) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("foo", 3);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)a, (PaykanObject *)b), 1);
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringEquals, DifferentContent) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)a, (PaykanObject *)b), 0);
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringEquals, DifferentLength) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("fo", 2);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)a, (PaykanObject *)b), 0);
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringEquals, EmptyStrings) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("", 0);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)a, (PaykanObject *)b), 1);
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringEquals, NonStringOtherFallsBackToIdentity) {
  PaykanString *s  = PaykanString_new("x", 1);
  PaykanObject *obj = PaykanObject_new();
  // Different types -> identity comparison -> not equal.
  EXPECT_EQ(PaykanString_equals((PaykanObject *)s, obj), 0);
  PaykanString_destroy((PaykanObject *)s);
  PaykanObject_destroy(obj);
}

// ============================================================================
// PaykanString_length
// ============================================================================

TEST(StringLength, ReturnsLen) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_length((PaykanObject *)s), 5);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringLength, EmptyIsZero) {
  PaykanString *s = PaykanString_new("", 0);
  EXPECT_EQ(PaykanString_length((PaykanObject *)s), 0);
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_concat
// ============================================================================

TEST(StringConcat, TwoStrings) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString *c = (PaykanString *)PaykanString_concat(
      (PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->len, 6);
  EXPECT_STREQ(c->data, "foobar");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
  PaykanString_destroy((PaykanObject *)c);
}

TEST(StringConcat, WithEmptyLeft) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString *c = (PaykanString *)PaykanString_concat(
      (PaykanObject *)a, (PaykanObject *)b);
  EXPECT_STREQ(c->data, "bar");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
  PaykanString_destroy((PaykanObject *)c);
}

TEST(StringConcat, WithEmptyRight) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString *c = (PaykanString *)PaykanString_concat(
      (PaykanObject *)a, (PaykanObject *)b);
  EXPECT_STREQ(c->data, "foo");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
  PaykanString_destroy((PaykanObject *)c);
}

TEST(StringConcat, BothEmpty) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString *c = (PaykanString *)PaykanString_concat(
      (PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->len, 0);
  EXPECT_STREQ(c->data, "");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
  PaykanString_destroy((PaykanObject *)c);
}

TEST(StringConcat, ResultIsNulTerminated) {
  PaykanString *a = PaykanString_new("ab", 2);
  PaykanString *b = PaykanString_new("cd", 2);
  PaykanString *c = (PaykanString *)PaykanString_concat(
      (PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(c->data[4], '\0');
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
  PaykanString_destroy((PaykanObject *)c);
}

// ============================================================================
// Vtable dispatch
// ============================================================================

TEST(StringVtable, ToStringViaVtable) {
  PaykanString *s = PaykanString_new("test", 4);
  PaykanShared *shared = s->vtable->toString((PaykanObject *)s);
  PaykanString *copy = (PaykanString *)PaykanShared_get(shared);
  EXPECT_STREQ(copy->data, "test");
  Paykan_release(shared);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringVtable, LengthViaVtable) {
  PaykanString *s = PaykanString_new("hello", 5);
  auto *vt = (PaykanStringVTable *)s->vtable;
  EXPECT_EQ(vt->length((PaykanObject *)s), 5);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringVtable, DestroyViaVtable) {
  PaykanString *s = PaykanString_new("bye", 3);
  s->vtable->destroy((PaykanObject *)s);
  // Should not crash.
}

// ============================================================================
// PaykanString_at
// ============================================================================

TEST(StringAt, FirstChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 0);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->len, 1);
  EXPECT_EQ(ch->data[0], 'h');
  Paykan_release(sh);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, LastChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 4);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[0], 'o');
  Paykan_release(sh);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, MiddleChar) {
  PaykanString *s = PaykanString_new("paykan", 6);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 3);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[0], 'k');
  EXPECT_EQ(ch->len, 1);
  Paykan_release(sh);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, ResultIsNulTerminated) {
  PaykanString *s = PaykanString_new("abc", 3);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 1);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ(ch->data[1], '\0');
  Paykan_release(sh);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, ResultHasStringVtable) {
  PaykanString *s = PaykanString_new("xyz", 3);
  PaykanShared *sh = PaykanString_at((PaykanObject *)s, 0);
  PaykanString *ch = (PaykanString *)PaykanShared_get(sh);
  EXPECT_EQ((void *)ch->vtable, (void *)&PaykanString_vtable);
  Paykan_release(sh);
  PaykanString_destroy((PaykanObject *)s);
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
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, OutOfBoundsDiesInDebug) {
  PaykanString *s = PaykanString_new("ab", 2);
  EXPECT_DEATH(PaykanString_at((PaykanObject *)s, 2), "");
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringAt, NegativeIndexDies) {
  PaykanString *s = PaykanString_new("ab", 2);
  EXPECT_DEATH(PaykanString_at((PaykanObject *)s, -1), "");
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_char_at
// ============================================================================

TEST(StringCharAt, FirstChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 0), 'h');
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringCharAt, LastChar) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 4), 'o');
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringCharAt, MidChar) {
  PaykanString *s = PaykanString_new("paykan", 6);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 3), 'k');
  PaykanString_destroy((PaykanObject *)s);
}

TEST(StringCharAt, SingleChar) {
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ(PaykanString_char_at((PaykanObject *)s, 0), 'x');
  PaykanString_destroy((PaykanObject *)s);
}

// ============================================================================
// PaykanString_concat_inplace
// ============================================================================

TEST(StringConcatInplace, AppendToEmpty) {
  PaykanString *a = PaykanString_new("", 0);
  PaykanString *b = PaykanString_new("hello", 5);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 5);
  EXPECT_STREQ(a->data, "hello");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringConcatInplace, AppendNonEmpty) {
  PaykanString *a = PaykanString_new("foo", 3);
  PaykanString *b = PaykanString_new("bar", 3);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 6);
  EXPECT_STREQ(a->data, "foobar");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringConcatInplace, AppendEmpty) {
  PaykanString *a = PaykanString_new("hello", 5);
  PaykanString *b = PaykanString_new("", 0);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->len, 5);
  EXPECT_STREQ(a->data, "hello");
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}

TEST(StringConcatInplace, ResultIsNulTerminated) {
  PaykanString *a = PaykanString_new("ab", 2);
  PaykanString *b = PaykanString_new("cd", 2);
  PaykanString_concat_inplace((PaykanObject *)a, (PaykanObject *)b);
  EXPECT_EQ(a->data[4], '\0');
  PaykanString_destroy((PaykanObject *)a);
  PaykanString_destroy((PaykanObject *)b);
}
