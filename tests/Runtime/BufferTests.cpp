// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for the PaykanBuffer_* functions, the storage of the standard
// library's memory::Buffer<T>: every slot is live, new slots are 0 / None,
// references are retained by the buffer and released when they leave it,
// and misuse panics.  Each test runs on the tracking allocator and ends with
// no live blocks.

#include <cstring>
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

namespace {

// RAII: the tracking allocator with zeroed counters; no live blocks at the
// end.
struct TrackingHeap {
  TrackingHeap() {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~TrackingHeap() {
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_heap_set_tracking(0);
  }
};

int64_t bits(PaykanShared *box) {
  int64_t b;
  memcpy(&b, &box, sizeof b);
  return b;
}

PaykanShared *box(int64_t bits) {
  PaykanShared *b;
  memcpy(&b, &bits, sizeof bits);
  return b;
}

PaykanShared *str(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

std::string text(int64_t b) {
  return ((PaykanString *)PaykanShared_get(box(b)))->data;
}

PaykanArray *array(int64_t buf) {
  return (PaykanArray *)PaykanShared_get(box(buf));
}

} // namespace

TEST(Buffer, NewHasZeroedSlots) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_INT, 3);
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 3);
  for (int64_t i = 0; i < 3; ++i)
    EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_INT, buf, i), 0) << i;
  EXPECT_EQ((void *)array(buf)->vtable, (void *)&PaykanArray_vtable);
  Paykan_release(box(buf));
}

TEST(Buffer, ValuesRoundTrip) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_FLOAT, 2);
  double d = 2.5;
  int64_t b;
  memcpy(&b, &d, sizeof b);
  PaykanBuffer_set(PAYKAN_KIND_FLOAT, &buf, 1, b);
  EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_FLOAT, buf, 1), b);
  EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_FLOAT, buf, 0), 0);
  Paykan_release(box(buf));
}

// The buffer holds its own reference to what is stored; a read is owned.
TEST(Buffer, ReferencesAreRetainedAndReleased) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_OBJECT, 2);
  EXPECT_EQ((void *)array(buf)->vtable, (void *)&PaykanArray_obj_vtable);
  PaykanShared *a = str("a");
  PaykanBuffer_set(PAYKAN_KIND_OBJECT, &buf, 0, bits(a));
  EXPECT_EQ(a->refCount, 2);
  int64_t got = PaykanBuffer_get(PAYKAN_KIND_OBJECT, buf, 0);
  EXPECT_EQ(text(got), "a");
  EXPECT_EQ(a->refCount, 3);
  Paykan_release(box(got));
  // Overwriting releases the old element; storing it again is safe.
  PaykanBuffer_set(PAYKAN_KIND_OBJECT, &buf, 0, bits(a));
  EXPECT_EQ(a->refCount, 2);
  PaykanShared *b = str("b");
  PaykanBuffer_set(PAYKAN_KIND_OBJECT, &buf, 0, bits(b));
  EXPECT_EQ(a->refCount, 1);
  Paykan_release(a);
  PaykanBuffer_set(PAYKAN_KIND_OBJECT, &buf, 1, bits(b));
  Paykan_release(b);
  Paykan_release(box(buf)); // releases both of its references to b
}

TEST(Buffer, OptionalSlotsStartAsNone) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_OPTIONAL, 1);
  EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_OPTIONAL, buf, 0), 0);
  PaykanShared *a = str("a");
  PaykanBuffer_set(PAYKAN_KIND_OPTIONAL, &buf, 0, bits(a));
  PaykanBuffer_set(PAYKAN_KIND_OPTIONAL, &buf, 0, 0); // back to None
  EXPECT_EQ(a->refCount, 1);
  Paykan_release(a);
  Paykan_release(box(buf));
}

TEST(Buffer, ReserveGrowAndShrinkChangeTheLength) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_INT, 3);
  PaykanBuffer_set(PAYKAN_KIND_INT, &buf, 2, 7);
  PaykanBuffer_reserve(PAYKAN_KIND_INT, &buf, 2); // already has 2
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 3);
  PaykanBuffer_reserve(PAYKAN_KIND_INT, &buf, 10);
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 10);
  PaykanBuffer_grow(PAYKAN_KIND_INT, &buf, 5);
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 15);
  EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_INT, buf, 2), 7);
  for (int64_t i = 3; i < 15; ++i)
    EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_INT, buf, i), 0) << i;
  PaykanBuffer_set(PAYKAN_KIND_INT, &buf, 14, 9);
  PaykanBuffer_shrink(PAYKAN_KIND_INT, &buf, 12);
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 3);
  // A slot that comes back is new: 0, not what it held before.
  PaykanBuffer_grow(PAYKAN_KIND_INT, &buf, 12);
  EXPECT_EQ(PaykanBuffer_get(PAYKAN_KIND_INT, buf, 14), 0);
  PaykanBuffer_shrink(PAYKAN_KIND_INT, &buf, 15);
  EXPECT_EQ(PaykanBuffer_len(PAYKAN_KIND_INT, buf), 0);
  Paykan_release(box(buf));
}

TEST(Buffer, ShrinkReleasesTheDroppedReferences) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_OBJECT, 3);
  PaykanShared *a = str("a");
  PaykanBuffer_set(PAYKAN_KIND_OBJECT, &buf, 2, bits(a));
  PaykanBuffer_shrink(PAYKAN_KIND_OBJECT, &buf, 2);
  EXPECT_EQ(a->refCount, 1);
  Paykan_release(a);
  Paykan_release(box(buf));
}

// Growing one slot at a time reallocates O(log n) times; shrinking frees
// the allocation once a quarter of it is in use.
TEST(Buffer, AllocationGrowsGeometricallyAndShrinks) {
  TrackingHeap heap;
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_INT, 0);
  int64_t before = Paykan_heap_total_reallocs();
  for (int i = 0; i < 1000; ++i)
    PaykanBuffer_grow(PAYKAN_KIND_INT, &buf, 1);
  EXPECT_LE(Paykan_heap_total_reallocs() - before, 12);
  EXPECT_GE(array(buf)->cap, 1000UL);
  PaykanBuffer_shrink(PAYKAN_KIND_INT, &buf, 999);
  EXPECT_EQ(array(buf)->cap, 8UL);
  Paykan_release(box(buf));
}

TEST(BufferDeath, OutOfBoundsPanics) {
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_INT, 3);
  EXPECT_DEATH(PaykanBuffer_get(PAYKAN_KIND_INT, buf, 3),
               "buffer index 3 out of bounds \\(len=3\\)");
  EXPECT_DEATH(PaykanBuffer_get(PAYKAN_KIND_INT, buf, -1), "out of bounds");
  EXPECT_DEATH(PaykanBuffer_set(PAYKAN_KIND_INT, &buf, 5, 1), "out of bounds");
  Paykan_release(box(buf));
}

TEST(BufferDeath, NeverSetObjectSlotPanics) {
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_OBJECT, 1);
  EXPECT_DEATH(PaykanBuffer_get(PAYKAN_KIND_OBJECT, buf, 0),
               "buffer slot 0 was never set");
  Paykan_release(box(buf));
}

TEST(BufferDeath, BadCountsPanic) {
  int64_t buf = PaykanBuffer_new(PAYKAN_KIND_INT, 3);
  EXPECT_DEATH(PaykanBuffer_new(PAYKAN_KIND_INT, -1), "cannot be negative");
  EXPECT_DEATH(PaykanBuffer_grow(PAYKAN_KIND_INT, &buf, -2),
               "buffer grow by -2: the count cannot be negative");
  EXPECT_DEATH(PaykanBuffer_reserve(PAYKAN_KIND_INT, &buf, -1),
               "cannot be negative");
  EXPECT_DEATH(PaykanBuffer_shrink(PAYKAN_KIND_INT, &buf, 4),
               "buffer shrink by 4: it has only 3 slots");
  Paykan_release(box(buf));
}
