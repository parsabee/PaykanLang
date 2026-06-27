// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Unit tests for boxed primitive runtime types: PaykanInt, PaykanFloat,
// PaykanBool, and PaykanError.

#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

// ============================================================================
// Helpers
// ============================================================================

static const char *sharedStr(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

// ============================================================================
// PaykanInt
// ============================================================================

TEST(BoxedInt, New) {
  PaykanInt *i = PaykanInt_new(42);
  ASSERT_NE(i, nullptr);
  EXPECT_EQ(i->value, 42);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, VtableIsIntVtable) {
  PaykanInt *i = PaykanInt_new(0);
  EXPECT_EQ(i->vtable, &PaykanInt_vtable);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, Negative) {
  PaykanInt *i = PaykanInt_new(-99);
  EXPECT_EQ(i->value, -99);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, ToString) {
  PaykanInt *i = PaykanInt_new(7);
  PaykanShared *s = PaykanInt_toString((PaykanObject *)i);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(sharedStr(s), "7");
  Paykan_release(s);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, ToStringNegative) {
  PaykanInt *i = PaykanInt_new(-123);
  PaykanShared *s = PaykanInt_toString((PaykanObject *)i);
  EXPECT_STREQ(sharedStr(s), "-123");
  Paykan_release(s);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, EqualsSelf) {
  PaykanInt *i = PaykanInt_new(5);
  EXPECT_EQ(PaykanInt_equals((PaykanObject *)i, (PaykanObject *)i), 1);
  PaykanInt_destroy((PaykanObject *)i);
}

TEST(BoxedInt, EqualsSameValue) {
  PaykanInt *a = PaykanInt_new(10);
  PaykanInt *b = PaykanInt_new(10);
  EXPECT_EQ(PaykanInt_equals((PaykanObject *)a, (PaykanObject *)b), 1);
  PaykanInt_destroy((PaykanObject *)a);
  PaykanInt_destroy((PaykanObject *)b);
}

TEST(BoxedInt, NotEqualDifferentValue) {
  PaykanInt *a = PaykanInt_new(1);
  PaykanInt *b = PaykanInt_new(2);
  EXPECT_EQ(PaykanInt_equals((PaykanObject *)a, (PaykanObject *)b), 0);
  PaykanInt_destroy((PaykanObject *)a);
  PaykanInt_destroy((PaykanObject *)b);
}

TEST(BoxedInt, FromStrValid) {
  PaykanObject *strObj = (PaykanObject *)PaykanString_new("42", 2);
  PaykanShared *result = PaykanInt_from_str(strObj);
  ASSERT_NE(result, nullptr);
  PaykanObject *inner = PaykanShared_get(result);
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->vtable, &PaykanInt_vtable);
  EXPECT_EQ(((PaykanInt *)inner)->value, 42);
  Paykan_release(result);
  PaykanString_destroy(strObj);
}

TEST(BoxedInt, FromStrInvalid) {
  PaykanObject *strObj = (PaykanObject *)PaykanString_new("abc", 3);
  PaykanShared *result = PaykanInt_from_str(strObj);
  ASSERT_NE(result, nullptr);
  // On failure returns a PaykanError box.
  PaykanObject *inner = PaykanShared_get(result);
  EXPECT_EQ(inner->vtable, &PaykanError_vtable);
  Paykan_release(result);
  PaykanString_destroy(strObj);
}

TEST(BoxedInt, FromStrNegative) {
  PaykanObject *strObj = (PaykanObject *)PaykanString_new("-7", 2);
  PaykanShared *result = PaykanInt_from_str(strObj);
  PaykanObject *inner = PaykanShared_get(result);
  EXPECT_EQ(inner->vtable, &PaykanInt_vtable);
  EXPECT_EQ(((PaykanInt *)inner)->value, -7);
  Paykan_release(result);
  PaykanString_destroy(strObj);
}

// ============================================================================
// PaykanFloat
// ============================================================================

TEST(BoxedFloat, New) {
  PaykanFloat *f = PaykanFloat_new(3.14);
  ASSERT_NE(f, nullptr);
  EXPECT_DOUBLE_EQ(f->value, 3.14);
  PaykanFloat_destroy((PaykanObject *)f);
}

TEST(BoxedFloat, VtableIsFloatVtable) {
  PaykanFloat *f = PaykanFloat_new(0.0);
  EXPECT_EQ(f->vtable, &PaykanFloat_vtable);
  PaykanFloat_destroy((PaykanObject *)f);
}

TEST(BoxedFloat, ToString) {
  PaykanFloat *f = PaykanFloat_new(2.5);
  PaykanShared *s = PaykanFloat_toString((PaykanObject *)f);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(sharedStr(s), "2.5");
  Paykan_release(s);
  PaykanFloat_destroy((PaykanObject *)f);
}

TEST(BoxedFloat, EqualsSameValue) {
  PaykanFloat *a = PaykanFloat_new(1.0);
  PaykanFloat *b = PaykanFloat_new(1.0);
  EXPECT_EQ(PaykanFloat_equals((PaykanObject *)a, (PaykanObject *)b), 1);
  PaykanFloat_destroy((PaykanObject *)a);
  PaykanFloat_destroy((PaykanObject *)b);
}

TEST(BoxedFloat, NotEqualDifferentValue) {
  PaykanFloat *a = PaykanFloat_new(1.0);
  PaykanFloat *b = PaykanFloat_new(2.0);
  EXPECT_EQ(PaykanFloat_equals((PaykanObject *)a, (PaykanObject *)b), 0);
  PaykanFloat_destroy((PaykanObject *)a);
  PaykanFloat_destroy((PaykanObject *)b);
}

TEST(BoxedFloat, FromStrValid) {
  PaykanObject *strObj = (PaykanObject *)PaykanString_new("1.5", 3);
  PaykanShared *result = PaykanFloat_from_str(strObj);
  PaykanObject *inner = PaykanShared_get(result);
  EXPECT_EQ(inner->vtable, &PaykanFloat_vtable);
  EXPECT_DOUBLE_EQ(((PaykanFloat *)inner)->value, 1.5);
  Paykan_release(result);
  PaykanString_destroy(strObj);
}

TEST(BoxedFloat, FromStrInvalid) {
  PaykanObject *strObj = (PaykanObject *)PaykanString_new("xyz", 3);
  PaykanShared *result = PaykanFloat_from_str(strObj);
  PaykanObject *inner = PaykanShared_get(result);
  EXPECT_EQ(inner->vtable, &PaykanError_vtable);
  Paykan_release(result);
  PaykanString_destroy(strObj);
}

// ============================================================================
// PaykanBool
// ============================================================================

TEST(BoxedBool, NewTrue) {
  PaykanBool *b = PaykanBool_new(1);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(b->value, 0);
  PaykanBool_destroy((PaykanObject *)b);
}

TEST(BoxedBool, NewFalse) {
  PaykanBool *b = PaykanBool_new(0);
  EXPECT_EQ(b->value, 0);
  PaykanBool_destroy((PaykanObject *)b);
}

TEST(BoxedBool, VtableIsBoolVtable) {
  PaykanBool *b = PaykanBool_new(1);
  EXPECT_EQ(b->vtable, &PaykanBool_vtable);
  PaykanBool_destroy((PaykanObject *)b);
}

TEST(BoxedBool, ToStringTrue) {
  PaykanBool *b = PaykanBool_new(1);
  PaykanShared *s = PaykanBool_toString((PaykanObject *)b);
  EXPECT_STREQ(sharedStr(s), "True");
  Paykan_release(s);
  PaykanBool_destroy((PaykanObject *)b);
}

TEST(BoxedBool, ToStringFalse) {
  PaykanBool *b = PaykanBool_new(0);
  PaykanShared *s = PaykanBool_toString((PaykanObject *)b);
  EXPECT_STREQ(sharedStr(s), "False");
  Paykan_release(s);
  PaykanBool_destroy((PaykanObject *)b);
}

TEST(BoxedBool, EqualsSameValue) {
  PaykanBool *a = PaykanBool_new(1);
  PaykanBool *b = PaykanBool_new(1);
  EXPECT_EQ(PaykanBool_equals((PaykanObject *)a, (PaykanObject *)b), 1);
  PaykanBool_destroy((PaykanObject *)a);
  PaykanBool_destroy((PaykanObject *)b);
}

// ============================================================================
// PaykanError
// ============================================================================

TEST(BoxedError, New) {
  const char *msg = "something went wrong";
  PaykanError *e = PaykanError_new(msg, (int64_t)strlen(msg));
  ASSERT_NE(e, nullptr);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(BoxedError, VtableIsErrorVtable) {
  PaykanError *e = PaykanError_new("err", 3);
  EXPECT_EQ(e->vtable, &PaykanError_vtable);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(BoxedError, ToStringContainsMessage) {
  PaykanError *e = PaykanError_new("oops", 4);
  PaykanShared *s = PaykanError_toString((PaykanObject *)e);
  ASSERT_NE(s, nullptr);
  std::string str = sharedStr(s);
  EXPECT_NE(str.find("oops"), std::string::npos);
  Paykan_release(s);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(BoxedError, EqualsSelf) {
  PaykanError *e = PaykanError_new("err", 3);
  EXPECT_EQ(PaykanError_equals((PaykanObject *)e, (PaykanObject *)e), 1);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(BoxedError, NotEqualDifferentErrors) {
  PaykanError *a = PaykanError_new("a", 1);
  PaykanError *b = PaykanError_new("b", 1);
  EXPECT_EQ(PaykanError_equals((PaykanObject *)a, (PaykanObject *)b), 0);
  PaykanError_destroy((PaykanObject *)a);
  PaykanError_destroy((PaykanObject *)b);
}
