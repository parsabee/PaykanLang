// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanArray runtime functions.

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"
#include "VTableTestHelper.h"

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
  auto *lenFn = vtSlot<PaykanLengthFn>(arr, PAYKAN_SLOT_ARRAY_LENGTH);
  EXPECT_EQ(lenFn((PaykanObject *)arr), 9);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayVtable, DestroyViaVtable) {
  // Just confirm calling through the vtable doesn't crash.
  PaykanArray *arr = PaykanArray_new(2);
  vtDestroy(arr)((PaykanObject *)arr);
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
  // Start with len=8, cap=8. First push fills slot 8 and doubles cap to 16.
  // Second push still fits inside the new cap=16 without another doubling.
  PaykanArray *arr = PaykanArray_new(8);
  PaykanArray_push(arr, i64vp(10));
  EXPECT_EQ(arr->len, 9UL);
  EXPECT_EQ(arr->cap, 16UL); // doubled once

  PaykanArray_push(arr, i64vp(20));
  EXPECT_EQ(arr->len, 10UL);
  EXPECT_EQ(arr->cap, 16UL); // no second doubling — still room
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
  PaykanArray *arr = PaykanArray_new(8);
  // cap==8, len==8 -> push must double cap to 16.
  PaykanArray_push(arr, i64vp(99));
  EXPECT_EQ(arr->len, 9UL);
  EXPECT_EQ(arr->cap, 16UL);
  for (int64_t i = 0; i < 7; ++i)
    PaykanArray_push(arr, i64vp(i));
  EXPECT_EQ(arr->cap, 16UL);
  // cap==16, len==16 -> push must double cap to 32.
  PaykanArray_push(arr, i64vp(88));
  EXPECT_EQ(arr->len, 17UL);
  EXPECT_EQ(arr->cap, 32UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPush, GrowthFromSmallCapUsesMinimumCapacity) {
  // A short exact-capacity array (e.g. a literal) grows straight to the
  // minimum capacity of 8 rather than 2, 4, 8.
  PaykanArray *arr = PaykanArray_new(1);
  PaykanArray_push(arr, i64vp(99));
  EXPECT_EQ(arr->len, 2UL);
  EXPECT_EQ(arr->cap, 8UL);
  PaykanArray_destroy((PaykanObject *)arr);

  PaykanArray *empty = PaykanArray_new(0);
  PaykanArray_push(empty, i64vp(1));
  EXPECT_EQ(empty->cap, 8UL);
  PaykanArray_destroy((PaykanObject *)empty);
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

// --- Pop: shrink with hysteresis (len <= cap/4 -> cap/2, min 8) ------------

// Push n values 0..n-1 onto a fresh empty primitive array.
static PaykanArray *filled(int64_t n) {
  PaykanArray *arr = PaykanArray_new(0);
  for (int64_t i = 0; i < n; ++i)
    PaykanArray_push(arr, i64vp(i * 10));
  return arr;
}

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

TEST(ArrayPop, NoShrinkAtHalfOfCap) {
  // cap=32, len=32.  Popping to len=16 (cap/2) used to shrink to exactly 16;
  // with hysteresis nothing happens until len <= cap/4.
  PaykanArray *arr = filled(32);
  EXPECT_EQ(arr->cap, 32UL);
  while (arr->len > 9)
    PaykanArray_pop(arr);
  EXPECT_EQ(arr->cap, 32UL); // len=9 > 32/4
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, ShrinksToHalfAtQuarterOfCap) {
  PaykanArray *arr = filled(32);
  while (arr->len > 8)
    PaykanArray_pop(arr);
  EXPECT_EQ(arr->len, 8UL); // 8 <= 32/4 -> shrink to 16, not to 8
  EXPECT_EQ(arr->cap, 16UL);
  // The next push must not grow again.
  PaykanArray_push(arr, i64vp(1));
  EXPECT_EQ(arr->cap, 16UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, NeverShrinksBelowMinimumCapacity) {
  PaykanArray *arr = filled(64);
  while (arr->len > 0)
    PaykanArray_pop(arr);
  EXPECT_EQ(arr->len, 0UL);
  EXPECT_EQ(arr->cap, 8UL);
  EXPECT_NE(arr->data, nullptr);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, SmallExactCapacityIsKept) {
  // A literal-sized array below the minimum capacity never shrinks.
  PaykanArray *arr = PaykanArray_new(4);
  for (int i = 0; i < 4; ++i)
    PaykanArray_pop(arr);
  EXPECT_EQ(arr->len, 0UL);
  EXPECT_EQ(arr->cap, 4UL);
  PaykanArray_destroy((PaykanObject *)arr);
}

TEST(ArrayPop, RemainingElementsIntact) {
  PaykanArray *arr = filled(64);
  while (arr->len > 3)
    PaykanArray_pop(arr); // shrinks 64 -> 32 -> 16 -> 8 on the way down
  EXPECT_EQ(arr->cap, 8UL);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 0)), 0);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 1)), 10);
  EXPECT_EQ(vpi64(PaykanArray_get(arr, 2)), 20);
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
  PaykanShared *elems[9];
  for (int i = 0; i < 9; ++i) {
    elems[i] = wrap(PaykanObject_new());
    PaykanArray_push_obj(arr, elems[i]); // cap 0 -> 8 -> 16
    EXPECT_EQ(arr->cap, i < 8 ? 8UL : 16UL);
  }
  EXPECT_EQ(arr->len, 9UL);
  for (int i = 0; i < 9; ++i)
    Paykan_release(elems[i]);
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

TEST(ArrayPopObj, ShrinksWithHysteresisAndKeepsRefcounts) {
  PaykanArray *arr = PaykanArray_new_obj(0);
  PaykanShared *elems[32];
  for (int i = 0; i < 32; ++i) {
    elems[i] = wrap(PaykanObject_new());
    PaykanArray_push_obj(arr, elems[i]);
  }
  EXPECT_EQ(arr->cap, 32UL);

  // Keep our refs on the first 8 (they stay in the array); drop the rest so
  // the array holds the only ones.
  for (int i = 8; i < 32; ++i)
    Paykan_release(elems[i]);

  while (arr->len > 9)
    Paykan_release(PaykanArray_pop_obj(arr));
  EXPECT_EQ(arr->cap, 32UL); // len=9 > 32/4: no shrink yet

  Paykan_release(PaykanArray_pop_obj(arr)); // len=8 <= 32/4 -> cap 16
  EXPECT_EQ(arr->len, 8UL);
  EXPECT_EQ(arr->cap, 16UL);

  // The surviving elements moved with the buffer and are still retained by
  // the array (our ref + the array's).
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(PaykanArray_get(arr, (unsigned long)i), (void *)elems[i]);
    EXPECT_EQ(elems[i]->refCount, 2);
    Paykan_release(elems[i]);
  }
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

// --- Issue #95: push/pop at the capacity boundary must not thrash ----------

// RAII: run a block with the tracking allocator and a zeroed counter set.
struct TrackingHeap {
  TrackingHeap() {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~TrackingHeap() { Paykan_heap_set_tracking(0); }
};

// Grow to `peak`, pop down to `boundary`, then do `pairs` push/pop pairs.
// Returns the number of Paykan_realloc calls made by the push/pop pairs.
static int64_t boundaryReallocs(bool obj, int64_t peak, int64_t boundary,
                                int64_t pairs) {
  TrackingHeap heap;
  PaykanArray *arr = obj ? PaykanArray_new_obj(0) : PaykanArray_new(0);
  for (int64_t i = 0; i < peak; ++i) {
    if (obj)
      PaykanArray_push_obj(arr, nullptr);
    else
      PaykanArray_push(arr, i64vp(i));
  }
  while ((int64_t)arr->len > boundary) {
    if (obj)
      PaykanArray_pop_obj(arr);
    else
      PaykanArray_pop(arr);
  }
  int64_t before = Paykan_heap_total_reallocs();
  for (int64_t k = 0; k < pairs; ++k) {
    if (obj) {
      PaykanArray_push_obj(arr, nullptr);
      PaykanArray_pop_obj(arr);
    } else {
      PaykanArray_push(arr, i64vp(k));
      EXPECT_EQ(vpi64(PaykanArray_pop(arr)), k);
    }
  }
  int64_t reallocs = Paykan_heap_total_reallocs() - before;
  EXPECT_EQ((int64_t)arr->len, boundary);
  if (obj)
    PaykanArray_destroy_obj((PaykanObject *)arr);
  else
    PaykanArray_destroy((PaykanObject *)arr);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  return reallocs;
}

TEST(ArrayResizePolicy, BoundaryPushPopDoesNotRealloc) {
  // The issue's shape, scaled down: peak 1,000,000 -> cap 1,048,576 becomes
  // peak 1000 -> cap 1024; boundary 524,288 becomes 512 (= cap/2).  Before the
  // fix every pair cost two reallocs (40,000 here).
  EXPECT_EQ(boundaryReallocs(false, 1000, 512, 20000), 0);
  EXPECT_EQ(boundaryReallocs(true, 1000, 512, 20000), 0);
}

TEST(ArrayResizePolicy, BoundaryAtEveryPowerOfTwoIsBounded) {
  // Whatever length the array hovers at, a long run of push/pop pairs costs
  // at most one resize, not one per operation.
  for (int64_t boundary = 1; boundary <= 4096; boundary *= 2) {
    for (int64_t delta = -1; delta <= 1; ++delta) {
      int64_t b = boundary + delta;
      if (b < 0)
        continue;
      EXPECT_LE(boundaryReallocs(false, 3 * boundary, b, 2000), 1)
          << "boundary " << b;
    }
  }
}

TEST(ArrayResizePolicy, GrowthAndShrinkReallocsAreLogarithmic) {
  TrackingHeap heap;
  PaykanArray *arr = PaykanArray_new(0);
  for (int64_t i = 0; i < 100000; ++i)
    PaykanArray_push(arr, i64vp(i));
  // 0 -> 8 -> 16 -> ... -> 131072: 15 resizes.
  EXPECT_EQ(Paykan_heap_total_reallocs(), 15);
  while (arr->len > 0)
    PaykanArray_pop(arr);
  // 131072 -> ... -> 8: 14 more halvings.
  EXPECT_EQ(Paykan_heap_total_reallocs(), 29);
  EXPECT_EQ(arr->cap, 8UL);
  PaykanArray_destroy((PaykanObject *)arr);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}
