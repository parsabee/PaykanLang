// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanObject runtime functions.

#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"
#include "VTableTestHelper.h"

// -- PaykanObject_new / PaykanObject_destroy

TEST(ObjectNew, AllocatesObject) {
  PaykanObject *obj = PaykanObject_new();
  ASSERT_NE(obj, nullptr);
  PaykanObject_destroy(obj);
}

TEST(ObjectNew, VtableIsObjectVtable) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(obj->vtable, PaykanObject_vtable);
  PaykanObject_destroy(obj);
}

// -- PaykanObject_toString

TEST(ObjectToString, ContainsObjectPrefix) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *shared = PaykanObject_toString(obj);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  EXPECT_NE(std::string(s->data).find("Object@"), std::string::npos);
  Paykan_release(shared);
  PaykanObject_destroy(obj);
}

TEST(ObjectToString, ReturnsNewStringEachCall) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *sh1 = PaykanObject_toString(obj);
  PaykanShared *sh2 = PaykanObject_toString(obj);
  PaykanString *s1 = (PaykanString *)PaykanShared_get(sh1);
  PaykanString *s2 = (PaykanString *)PaykanShared_get(sh2);
  // Two distinct shared boxes.
  EXPECT_NE(sh1, sh2);
  // But same content.
  EXPECT_STREQ(s1->data, s2->data);
  Paykan_release(sh1);
  Paykan_release(sh2);
  PaykanObject_destroy(obj);
}

// -- PaykanObject_equals

TEST(ObjectEquals, SameObjectIsEqual) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(paykanTestEquals(PaykanObject_equals, obj, obj), 1);
  PaykanObject_destroy(obj);
}

TEST(ObjectEquals, DifferentObjectsAreNotEqual) {
  PaykanObject *a = PaykanObject_new();
  PaykanObject *b = PaykanObject_new();
  EXPECT_EQ(paykanTestEquals(PaykanObject_equals, a, b), 0);
  PaykanObject_destroy(a);
  PaykanObject_destroy(b);
}

// -- Vtable dispatch

// Paykan_vtable_of reads any object's vtable, whatever its struct type.
TEST(ObjectVtable, VTableOfReadsTheHeader) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(Paykan_vtable_of(obj), PaykanObject_vtable);
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ(Paykan_vtable_of(s), PaykanString_vtable);
  EXPECT_EQ(Paykan_vtable_of(&PaykanFile_Stdin), PaykanFile_Stdin.vtable);
  PaykanString_destroy((PaykanObject *)s);
  PaykanObject_destroy(obj);
}

TEST(ObjectVtable, ToStringViaVtable) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *shared = vtToString(obj)(obj);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  EXPECT_NE(std::string(s->data).find("Object@"), std::string::npos);
  Paykan_release(shared);
  PaykanObject_destroy(obj);
}

TEST(ObjectVtable, EqualsViaVtable) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(paykanTestEquals(vtEquals(obj), obj, obj), 1);
  PaykanObject_destroy(obj);
}

TEST(ObjectVtable, DestroyViaVtable) {
  // Should not crash.
  PaykanObject *obj = PaykanObject_new();
  vtDestroy(obj)(obj);
}

// -- PaykanObject_None singleton

TEST(ObjectNone, ToStringReturnsNone) {
  PaykanShared *shared = vtToString(&PaykanObject_None)(&PaykanObject_None);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s->data, "None");
  Paykan_release(shared);
}

TEST(ObjectNone, EqualsOnlyItself) {
  EXPECT_EQ(paykanTestEquals(vtEquals(&PaykanObject_None), &PaykanObject_None,
                             &PaykanObject_None),
            1);
  PaykanObject *other = PaykanObject_new();
  EXPECT_EQ(
      paykanTestEquals(vtEquals(&PaykanObject_None), &PaykanObject_None, other),
      0);
  PaykanObject_destroy(other);
}

TEST(ObjectNone, DestroyIsNoOp) {
  // Must not free the immortal singleton.
  PaykanDestroyFn destroy = vtDestroy(&PaykanObject_None);
  destroy(&PaykanObject_None);
  // Still accessible after "destroy".
  EXPECT_NE(PaykanObject_None.vtable, nullptr);
}
