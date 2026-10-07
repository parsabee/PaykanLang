// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Null-hardening tests for the runtime `equals` implementations.
//
// The vtable `equals` slot receives its `other` argument as a consumed
// PaykanShared box (see src/Runtime/RuntimeInternal.h).  That box can be NULL
// — e.g. an absent optional, which is a null box — and every
// implementation must treat a NULL box as "equal to nothing" (identity: NULL
// equals nothing) and return 0 instead of dereferencing it.

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

#include "VTableTestHelper.h"

namespace {

// Call `fn` with a NULL `other` box.  Paykan_release(NULL) is a no-op, so the
// consumed-argument contract needs no offsetting retain here.
int64_t equalsWithNullOther(int64_t (*fn)(PaykanObject *, PaykanShared *),
                            PaykanObject *self) {
  return fn(self, nullptr);
}

} // namespace

TEST(EqualsNullOther, Object) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(equalsWithNullOther(PaykanObject_equals, obj), 0);
  PaykanObject_destroy(obj);
}

TEST(EqualsNullOther, NoneSingleton) {
  // None's equals goes through its own static vtable implementation.
  PaykanObject *none = &PaykanObject_None;
  EXPECT_EQ(equalsWithNullOther(vtEquals(none), none), 0);
}

TEST(EqualsNullOther, Int) {
  PaykanInt *v = PaykanInt_new(42);
  EXPECT_EQ(equalsWithNullOther(PaykanInt_equals, (PaykanObject *)v), 0);
  PaykanInt_destroy((PaykanObject *)v);
}

TEST(EqualsNullOther, Float) {
  PaykanFloat *v = PaykanFloat_new(1.5);
  EXPECT_EQ(equalsWithNullOther(PaykanFloat_equals, (PaykanObject *)v), 0);
  PaykanFloat_destroy((PaykanObject *)v);
}

TEST(EqualsNullOther, Bool) {
  PaykanBool *v = PaykanBool_new(1);
  EXPECT_EQ(equalsWithNullOther(PaykanBool_equals, (PaykanObject *)v), 0);
  PaykanBool_destroy((PaykanObject *)v);
}

TEST(EqualsNullOther, String) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(equalsWithNullOther(PaykanString_equals, (PaykanObject *)s), 0);
  PaykanString_destroy((PaykanObject *)s);
}

TEST(EqualsNullOther, File) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ(equalsWithNullOther(PaykanFile_equals, (PaykanObject *)f), 0);
  PaykanFile_destroy((PaykanObject *)f);
}

TEST(EqualsNullOther, Error) {
  PaykanError *e = PaykanError_new("boom", 4);
  EXPECT_EQ(equalsWithNullOther(PaykanError_equals, (PaykanObject *)e), 0);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(EqualsNullOther, Array) {
  PaykanArray *a = PaykanArray_new(3);
  EXPECT_EQ(equalsWithNullOther(PaykanArray_equals, (PaykanObject *)a), 0);
  PaykanArray_destroy((PaykanObject *)a);
}
