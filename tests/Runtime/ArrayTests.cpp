// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanArray runtime functions.

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"

// ============================================================================
// Helpers
// ============================================================================

// Wrap a raw object in a fresh PaykanShared* (refcount=1).
static PaykanShared *wrap(PaykanObject *obj) { return PaykanShared_new(obj); }

// ============================================================================
// PaykanArray_new — primitive arrays
// ============================================================================

TEST(ArrayNew, AllocatesCorrectLength) {
  PaykanArray *arr = PaykanArray_new(5);
  EXPECT_EQ(arr->len, 5UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayNew, ZeroLength) {
  PaykanArray *arr = PaykanArray_new(0);
  EXPECT_EQ(arr->len, 0UL);
  EXPECT_EQ(arr->data, nullptr);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayNew, DataIsZeroInitialised) {
  PaykanArray *arr = PaykanArray_new(4);
  for (unsigned long i = 0; i < arr->len; ++i) {
    void *v = PaykanArray_get(arr, i);
    EXPECT_EQ(v, nullptr) << "slot " << i << " should be zero";
  }
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayNew, VtableIsPrimitive) {
  PaykanArray *arr = PaykanArray_new(1);
  EXPECT_EQ((void *)arr->vtable, (void *)&PaykanArray_vtable);
  PaykanArray_destroy((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_new_obj — object arrays
// ============================================================================

TEST(ArrayNewObj, AllocatesCorrectLength) {
  PaykanArray *arr = PaykanArray_new_obj(3);
  EXPECT_EQ(arr->len, 3UL);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayNewObj, ZeroLength) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  EXPECT_EQ(arr->len, 0UL);
  EXPECT_EQ(arr->data, nullptr);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayNewObj, VtableIsObject) {
  PaykanArray *arr = PaykanArray_new_obj(1);
  EXPECT_EQ((void *)arr->vtable, (void *)&PaykanArray_obj_vtable);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayNewObj, DataIsZeroInitialised) {
  PaykanArray *arr = PaykanArray_new_obj(3);
  for (unsigned long i = 0; i < arr->len; ++i)
    EXPECT_EQ(PaykanArray_get(arr, i), nullptr) << "slot " << i;
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_set / PaykanArray_get — primitive
// ============================================================================

TEST(ArraySetGet, StoresAndReadsInt) {
  PaykanArray *arr = PaykanArray_new(3);
  int64_t val = 42;
  void *vp;
  memcpy(&vp, &val, 8);
  PaykanArray_set(arr, 0, vp);

  void *out = PaykanArray_get(arr, 0);
  int64_t result;
  memcpy(&result, &out, 8);
  EXPECT_EQ(result, 42);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArraySetGet, IndependentSlots) {
  PaykanArray *arr = PaykanArray_new(3);
  for (int64_t i = 0; i < 3; ++i) {
    void *vp;
    memcpy(&vp, &i, 8);
    PaykanArray_set(arr, (unsigned long)i, vp);
  }
  for (int64_t i = 0; i < 3; ++i) {
    void *out = PaykanArray_get(arr, (unsigned long)i);
    int64_t result;
    memcpy(&result, &out, 8);
    EXPECT_EQ(result, i);
  }
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArraySetGet, OutOfBoundsDies) {
  PaykanArray *arr = PaykanArray_new(2);
  EXPECT_DEATH(PaykanArray_get(arr, 2), "out of bounds");
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArraySetGet, SetOutOfBoundsDies) {
  PaykanArray *arr = PaykanArray_new(2);
  EXPECT_DEATH(PaykanArray_set(arr, 5, nullptr), "out of bounds");
  PaykanArray_destroy((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_set_obj — reference counting
// ============================================================================

TEST(ArraySetObj, RetainsIncomingElement) {
  PaykanArray *arr = PaykanArray_new_obj(1);
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *shared = wrap(obj);
  EXPECT_EQ(shared->refCount, 1);

  PaykanArray_set_obj(arr, 0, shared);
  // Array retains -> refcount should be 2.
  EXPECT_EQ(shared->refCount, 2);

  // Release our own reference; the array still holds one.
  Paykan_release(shared);
  EXPECT_EQ(((PaykanShared *)PaykanArray_get(arr, 0))->refCount, 1);

  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArraySetObj, ReleasesOldElement) {
  PaykanArray *arr = PaykanArray_new_obj(1);

  PaykanObject *obj1 = PaykanObject_new();
  PaykanShared *s1 = wrap(obj1);
  PaykanArray_set_obj(arr, 0, s1);
  // arr retains s1 -> refcount 2.
  EXPECT_EQ(s1->refCount, 2);

  PaykanObject *obj2 = PaykanObject_new();
  PaykanShared *s2 = wrap(obj2);
  // Overwrite slot — should release s1 and retain s2.
  PaykanArray_set_obj(arr, 0, s2);
  EXPECT_EQ(s1->refCount, 1); // array released its hold on s1
  EXPECT_EQ(s2->refCount, 2); // array retained s2

  Paykan_release(s1);
  Paykan_release(s2);
  // arr still holds s2; destroy_obj will release it.
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArraySetObj, NullValueIsStoredSafely) {
  PaykanArray *arr = PaykanArray_new_obj(1);
  // Should not crash when storing nullptr.
  PaykanArray_set_obj(arr, 0, nullptr);
  EXPECT_EQ(PaykanArray_get(arr, 0), nullptr);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_destroy_obj — releases all elements
// ============================================================================

TEST(ArrayDestroyObj, ReleasesAllElements) {
  PaykanArray *arr = PaykanArray_new_obj(3);
  PaykanShared *shared[3];
  for (int i = 0; i < 3; ++i) {
    shared[i] = wrap(PaykanObject_new());
    PaykanArray_set_obj(arr, (unsigned long)i, shared[i]);
    // arr retains -> refcount 2.
    EXPECT_EQ(shared[i]->refCount, 2);
  }
  // Release our references; arr still holds one each.
  for (int i = 0; i < 3; ++i)
    Paykan_release(shared[i]);

  // destroy_obj must not crash; it releases the remaining ref for each slot.
  // (We can't check refcounts after destroy since the memory is freed.)
  PaykanArray_destroy_obj((PaykanObject *)arr);
  // If we reach here without crashing / ASAN error, the test passes.
}

// ============================================================================
// PaykanArray_length
// ============================================================================

TEST(ArrayLength, ReturnsCorrectLen) {
  PaykanArray *arr = PaykanArray_new(7);
  EXPECT_EQ(PaykanArray_length((PaykanObject *)arr), 7);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayLength, ZeroLen) {
  PaykanArray *arr = PaykanArray_new(0);
  EXPECT_EQ(PaykanArray_length((PaykanObject *)arr), 0);
  PaykanArray_destroy((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_toString
// ============================================================================

TEST(ArrayToString, ContainsLenAndAddress) {
  PaykanArray *arr = PaykanArray_new(4);
  PaykanShared *shared = PaykanArray_toString((PaykanObject *)arr);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  ASSERT_NE(s, nullptr);
  // Output should contain "Array@" and "len=4".
  EXPECT_NE(std::string(s->data).find("Array@"), std::string::npos);
  EXPECT_NE(std::string(s->data).find("len=4"), std::string::npos);
  Paykan_release(shared);
  PaykanArray_destroy((PaykanObject *)arr);
}

// ============================================================================
// PaykanArray_equals
// ============================================================================

TEST(ArrayEquals, SameObjectIsEqual) {
  PaykanArray *arr = PaykanArray_new(2);
  EXPECT_EQ(paykanTestEquals(PaykanArray_equals, (PaykanObject *)arr,
                             (PaykanObject *)arr),
            1);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayEquals, DifferentObjectsAreNotEqual) {
  PaykanArray *a = PaykanArray_new(2);
  PaykanArray *b = PaykanArray_new(2);
  EXPECT_EQ(paykanTestEquals(PaykanArray_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            0);
  PaykanArray_destroy((PaykanObject *)a);
  PaykanArray_destroy((PaykanObject *)b);
}

// ============================================================================
// Vtable virtual dispatch
// ============================================================================

TEST(ArrayVtable, LengthViaVtable) {
  PaykanArray *arr = PaykanArray_new(9);
  auto *lenFn = (int64_t(*)(PaykanObject *))((void **)arr->vtable)[3];
  EXPECT_EQ(lenFn((PaykanObject *)arr), 9);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayVtable, DestroyViaVtable) {
  // Just confirm calling through the vtable doesn't crash.
  PaykanArray *arr = PaykanArray_new(2);
  arr->vtable->destroy((PaykanObject *)arr);
  // Reached here without crash — pass.
}

// ============================================================================
// PaykanArray_push / PaykanArray_pop — capacity management
// ============================================================================

// Helper: pack an int64_t into a void* for push.
static void *i64vp(int64_t v) {
  void *vp;
  memcpy(&vp, &v, 8);
  return vp;
}
static int64_t vpi64(void *vp) {
  int64_t v;
  memcpy(&v, &vp, 8);
  return v;
}

// --- Initial capacity matches len on construction ---------------------------

TEST(ArrayCap, NewSetsCapToLen) {
  PaykanArray *arr = PaykanArray_new(4);
  EXPECT_EQ(arr->cap, 4UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayCap, NewZeroSetsCapZero) {
  PaykanArray *arr = PaykanArray_new(0);
  EXPECT_EQ(arr->cap, 0UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayCap, NewFromDataSetsCapToLen) {
  int64_t data[] = {1, 2, 3};
  PaykanArray *arr = PaykanArray_new_from_data(3, data);
  EXPECT_EQ(arr->cap, 3UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayCap, NewObjSetsCapToLen) {
  PaykanArray *arr = PaykanArray_new_obj(2);
  EXPECT_EQ(arr->cap, 2UL);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

// --- Push: grow only when full ----------------------------------------------

TEST(ArrayPush, PushIntoRoomDoesNotReallocate) {
  // Start with len=2, cap=2. First push fills slot 2 and doubles cap to 4.
  // Second push still fits inside the new cap=4 without another doubling.
  PaykanArray *arr = PaykanArray_new(2);
  PaykanArray_push(arr, i64vp(10));
  EXPECT_EQ(arr->len, 3UL);
  EXPECT_EQ(arr->cap, 4UL); // doubled once

  PaykanArray_push(arr, i64vp(20));
  EXPECT_EQ(arr->len, 4UL);
  EXPECT_EQ(arr->cap, 4UL); // no second doubling — still room
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPush, PushOntoEmptyArrayAllocates) {
  PaykanArray *arr = PaykanArray_new(0);
  EXPECT_EQ(arr->cap, 0UL);
  PaykanArray_push(arr, i64vp(7));
  EXPECT_EQ(arr->len, 1UL);
  EXPECT_GE(arr->cap, 1UL);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 0)), 7);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPush, DoublesCapWhenFull) {
  PaykanArray *arr = PaykanArray_new(1);
  // cap==1, len==1 -> push must double cap to 2.
  PaykanArray_push(arr, i64vp(99));
  EXPECT_EQ(arr->len, 2UL);
  EXPECT_EQ(arr->cap, 2UL);
  // cap==2, len==2 -> push must double cap to 4.
  PaykanArray_push(arr, i64vp(88));
  EXPECT_EQ(arr->len, 3UL);
  EXPECT_EQ(arr->cap, 4UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPush, PreservesExistingElements) {
  PaykanArray *arr = PaykanArray_new(2);
  void *vp0 = i64vp(11), *vp1 = i64vp(22);
  PaykanArray_set(arr, 0, vp0);
  PaykanArray_set(arr, 1, vp1);
  PaykanArray_push(arr, i64vp(33));
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 0)), 11);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 1)), 22);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 2)), 33);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPush, LenIncrementsByOne) {
  PaykanArray *arr = PaykanArray_new(0);
  for (int64_t i = 0; i < 8; ++i) {
    PaykanArray_push(arr, i64vp(i));
    EXPECT_EQ(arr->len, (unsigned long)(i + 1));
  }
  PaykanArray_destroy((PaykanObject *)arr);
}

// --- Pop: shrink when len drops to half of cap ------------------------------

TEST(ArrayPop, ReturnsLastElement) {
  PaykanArray *arr = PaykanArray_new(3);
  PaykanArray_set(arr, 0, i64vp(1));
  PaykanArray_set(arr, 1, i64vp(2));
  PaykanArray_set(arr, 2, i64vp(3));
  void *v = PaykanArray_pop(arr);
  EXPECT_EQ(vpi64(v), 3);
  EXPECT_EQ(arr->len, 2UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, DecreasesLen) {
  PaykanArray *arr = PaykanArray_new(4);
  PaykanArray_pop(arr);
  EXPECT_EQ(arr->len, 3UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, ShrinksWhenLenHalfOfCap) {
  // Push until cap=4, len=4. Then pop twice -> len=2 == cap/2 -> shrink to 2.
  PaykanArray *arr = PaykanArray_new(0);
  for (int64_t i = 0; i < 4; ++i)
    PaykanArray_push(arr, i64vp(i));
  EXPECT_EQ(arr->cap, 4UL);
  EXPECT_EQ(arr->len, 4UL);

  PaykanArray_pop(arr); // len=3, cap=4 -> no shrink (3 > 4/2)
  EXPECT_EQ(arr->cap, 4UL);

  PaykanArray_pop(arr); // len=2, cap=4 -> 2 <= 4/2 -> shrink to 2
  EXPECT_EQ(arr->len, 2UL);
  EXPECT_EQ(arr->cap, 2UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, ShrinkToZeroFreesBuffer) {
  PaykanArray *arr = PaykanArray_new(0);
  PaykanArray_push(arr, i64vp(5)); // len=1, cap=1
  PaykanArray_pop(arr);            // len=0 -> shrink: free buffer, cap=0
  EXPECT_EQ(arr->len, 0UL);
  EXPECT_EQ(arr->cap, 0UL);
  EXPECT_EQ(arr->data, nullptr);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, RemainingElementsIntact) {
  PaykanArray *arr = PaykanArray_new(0);
  for (int64_t i = 0; i < 4; ++i)
    PaykanArray_push(arr, i64vp(i * 10));
  PaykanArray_pop(arr); // remove 30
  PaykanArray_pop(arr); // remove 20 -> shrink
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 0)), 0);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 1)), 10);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, EmptyArrayDies) {
  PaykanArray *arr = PaykanArray_new(0);
  EXPECT_DEATH(PaykanArray_pop(arr), "pop on empty array");
  PaykanArray_destroy((PaykanObject *)arr);
}

// --- push_obj / pop_obj -----------------------------------------------------

TEST(ArrayPushObj, RetainsElement) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  PaykanShared *s = wrap(PaykanObject_new());
  EXPECT_EQ(s->refCount, 1);
  PaykanArray_push_obj(arr, s);
  EXPECT_EQ(s->refCount, 2); // array retained
  EXPECT_EQ(arr->len, 1UL);
  Paykan_release(s);
  PaykanArray_destroy_obj((PaykanObject *)arr); // releases final ref
}

TEST(ArrayPushObj, DoublesCapWhenFull) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  PaykanShared *s1 = wrap(PaykanObject_new());
  PaykanShared *s2 = wrap(PaykanObject_new());
  PaykanArray_push_obj(arr, s1); // cap->1
  PaykanArray_push_obj(arr, s2); // cap->2
  EXPECT_EQ(arr->len, 2UL);
  EXPECT_EQ(arr->cap, 2UL);
  Paykan_release(s1);
  Paykan_release(s2);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayPopObj, ReturnsElementAndReleasesSlot) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  PaykanShared *s = wrap(PaykanObject_new());
  PaykanArray_push_obj(arr, s); // arr retains -> refcount 2
  Paykan_release(s);            // drop our ref; arr holds the only one

  PaykanShared *got = PaykanArray_pop_obj(arr); // returned, not released
  EXPECT_EQ(got, s);
  EXPECT_EQ(got->refCount, 1); // ownership transferred to caller
  Paykan_release(got);         // caller drops it

  EXPECT_EQ(arr->len, 0UL);
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayPopObj, ShrinksWhenLenHalfOfCap) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  PaykanShared *elems[4];
  for (int i = 0; i < 4; ++i) {
    elems[i] = wrap(PaykanObject_new());
    PaykanArray_push_obj(arr, elems[i]);
  }
  EXPECT_EQ(arr->cap, 4UL);

  // Drop our refs; arr holds the only ones now.
  for (int i = 0; i < 4; ++i)
    Paykan_release(elems[i]);

  PaykanShared *v3 = PaykanArray_pop_obj(arr);
  Paykan_release(v3);       // len=3
  EXPECT_EQ(arr->cap, 4UL); // not yet half

  PaykanShared *v2 = PaykanArray_pop_obj(arr);
  Paykan_release(v2); // len=2 == cap/2 -> shrink
  EXPECT_EQ(arr->len, 2UL);
  EXPECT_EQ(arr->cap, 2UL);

  PaykanArray_destroy_obj((PaykanObject *)arr);
}

TEST(ArrayPopObj, EmptyArrayDies) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  EXPECT_DEATH(PaykanArray_pop_obj(arr), "pop on empty array");
  PaykanArray_destroy_obj((PaykanObject *)arr);
}

// --- round-trip push/pop correctness ----------------------------------------

TEST(ArrayPushPop, RoundTripPreservesValues) {
  PaykanArray *arr = PaykanArray_new(0);
  int64_t vals[] = {100, 200, 300, 400};
  for (int64_t v : vals)
    PaykanArray_push(arr, i64vp(v));

  // Pop them back in LIFO order.
  for (int i = 3; i >= 0; --i)
    EXPECT_EQ(vpi64(PaykanArray_pop(arr)), vals[i]);

  EXPECT_EQ(arr->len, 0UL);
  PaykanArray_destroy((PaykanObject *)arr);
}
