// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// NULL-box tolerance optional types (issue #5) rely on: a
// `T?` is a possibly-NULL PaykanShared*, so every runtime entry point that
// generated code may hand such a box to must accept NULL — reference
// counting, unboxing, printing, and every object-array operation (NULL
// slots are how `Node?[]` stores None).

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

#include "VTableTestHelper.h"

// -- Reference counting and unboxing

TEST(OptionalNull, RetainReleaseGetTolerateNull) {
  Paykan_retain(nullptr);
  Paykan_release(nullptr);
  EXPECT_EQ(PaykanShared_get(nullptr), nullptr);
}

TEST(OptionalNull, PrintTolerateNull) {
  // print_object returns early on a NULL object; must not crash.
  Paykan_print(nullptr);
  Paykan_println(nullptr);
  Paykan_printerr(nullptr);
  Paykan_printerrln(nullptr);
}

// -- Object arrays with NULL slots (T?[])

TEST(OptionalNull, ArrayNewObjSlotsStartNull) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanArray *arr = PaykanArray_new_obj(2);
    EXPECT_EQ(PaykanArray_get(arr, 0), nullptr);
    EXPECT_EQ(PaykanArray_get(arr, 1), nullptr);
    // destroy_obj skips NULL slots.
    Paykan_release(PaykanShared_new((PaykanObject *)arr));
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

TEST(OptionalNull, SetObjNullReleasesOldAndStoresNull) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanArray *arr = PaykanArray_new_obj(1);
    PaykanShared *s = PaykanShared_new((PaykanObject *)PaykanObject_new());
    PaykanArray_set_obj(arr, 0, s); // retains
    EXPECT_EQ(s->refCount, 2);
    PaykanArray_set_obj(arr, 0, nullptr); // releases old, stores NULL
    EXPECT_EQ(s->refCount, 1);
    EXPECT_EQ(PaykanArray_get(arr, 0), nullptr);
    PaykanArray_set_obj(arr, 0, nullptr); // NULL over NULL: no-op
    EXPECT_EQ(PaykanArray_get(arr, 0), nullptr);
    Paykan_release(s);
    Paykan_release(PaykanShared_new((PaykanObject *)arr));
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

TEST(OptionalNull, PushPopNullElements) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanArray *arr = PaykanArray_new_obj(0);
    PaykanShared *s = PaykanShared_new((PaykanObject *)PaykanObject_new());
    PaykanArray_push_obj(arr, nullptr);
    PaykanArray_push_obj(arr, s);
    PaykanArray_push_obj(arr, nullptr);
    EXPECT_EQ(arr->len, 3UL);
    EXPECT_EQ(s->refCount, 2);
    EXPECT_EQ(PaykanArray_get(arr, 0), nullptr);
    EXPECT_EQ(PaykanArray_get(arr, 1), (void *)s);
    EXPECT_EQ(PaykanArray_get(arr, 2), nullptr);
    // pop hands back the slot as-is: NULL for a None element, the owned box
    // otherwise (the caller releases it).
    EXPECT_EQ(PaykanArray_pop_obj(arr), nullptr);
    PaykanShared *popped = PaykanArray_pop_obj(arr);
    EXPECT_EQ(popped, s);
    Paykan_release(popped);
    EXPECT_EQ(s->refCount, 1);
    EXPECT_EQ(PaykanArray_pop_obj(arr), nullptr);
    EXPECT_EQ(arr->len, 0UL);
    Paykan_release(s);
    Paykan_release(PaykanShared_new((PaykanObject *)arr));
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

TEST(OptionalNull, DestroyObjArrayWithMixedNullSlots) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanArray *arr = PaykanArray_new_obj(3);
    PaykanShared *s = PaykanShared_new((PaykanObject *)PaykanObject_new());
    PaykanArray_set_obj(arr, 1, s);
    Paykan_release(s); // array now sole owner
    // toString / equals never touch the (NULL) elements.
    PaykanShared *str = vtToString(arr)((PaykanObject *)arr);
    EXPECT_NE(PaykanShared_get(str), nullptr);
    Paykan_release(str);
    PaykanShared *box = PaykanShared_new((PaykanObject *)arr);
    Paykan_retain(box);
    EXPECT_EQ(vtEquals(arr)((PaykanObject *)arr, box), 1);
    Paykan_release(box); // destroys array: releases slot 1, skips NULLs
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

// `equals` with a NULL `other` box (the two-optional protocol never calls
// equals with NULL, but the runtime convention is documented as tolerant)

TEST(OptionalNull, EqualsWithNullOtherIsFalse) {
  PaykanObject *o = PaykanObject_new();
  EXPECT_EQ(PaykanObject_equals(o, nullptr), 0);
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)s, nullptr), 0);
  PaykanString_destroy((PaykanObject *)s);
  PaykanObject_destroy(o);
}
