// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the concurrency prototype's `locked T` cell (PaykanLocked_*,
// src/Runtime/Locked.c): one heap object holding an object's box behind a
// mutex, which a `lock` block takes for its length.  Its count is atomic, as
// an atomic cell's is, and the last handle frees it and the box it holds.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "Runtime.h"
}

#include "VTableTestHelper.h"

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

/// The cell an operation takes: a handle is the cell's own pointer.
PaykanObject *cell(PaykanShared *handle) { return PaykanShared_get(handle); }

/// What a lock gives: the address of the cell's payload slot, a box.
PaykanShared **slot(void *p) { return static_cast<PaykanShared **>(p); }

/// An object with a number in it, standing in for the class a `locked`
/// holds (`Counts` in samples/concurrency/05_locked.pkn).  Counted as any
/// object, so a test sees the cell take its box and keep it.
struct Counter {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
  int64_t n;
};

PaykanShared *counter(int64_t n) {
  auto *c = static_cast<Counter *>(Paykan_malloc(sizeof(Counter)));
  std::memset(c, 0, sizeof *c);
  c->vtable = PaykanObject_vtable; // a destroy of nothing; the block is freed
  c->n = n;
  return PaykanShared_new(reinterpret_cast<PaykanObject *>(c));
}

int64_t &count(PaykanShared *box) {
  return reinterpret_cast<Counter *>(PaykanShared_get(box))->n;
}

std::string text(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

/// The cell's text through its vtable slot, as `Str(x)` reads it.
std::string toString(PaykanShared *handle) {
  PaykanShared *s = vtToString(handle)(cell(handle));
  std::string result = text(s);
  Paykan_release(s);
  return result;
}

/// The equals slot with `other` lent (bit 0 of the mask clear).  Not
/// RuntimeEqualsHelper.h's paykanTestEquals: that undoes by hand the retain
/// boxing takes, which an immortal cell's count never took.
int64_t equalsLent(PaykanShared *self, PaykanShared *other) {
  return vtEquals(self)(cell(self), other, 0);
}

using PaykanWalkFn = void (*)(PaykanObject *);
PaykanWalkFn vtWalk(const void *obj) {
  return vtSlot<PaykanWalkFn>(obj, PAYKAN_SLOT_WALK);
}

/// Releases a cell's payload and frees the cell at the end of a test.  The
/// runtime never does either (the prototype leaks every cell with what it
/// holds, samples/concurrency/README.md "What is not", and has no teardown
/// of the mutex): the payload is taken out under the lock, and the count
/// put at 1 is the thaw the design describes, after which the release
/// frees the block as it frees any object's.
void discard(PaykanShared *handle) { Paykan_release(handle); }

} // namespace

// -- The cell is counted atomically

TEST(LockedCell, IsCountedAtomicallyAndFreesItsContentsWithItsLastHandle) {
  TrackingHeap heap;
  PaykanShared *c = PaykanLocked_new(counter(1));
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(c->weak, 0);
  const int64_t blocks = Paykan_heap_live_blocks(); // the cell, its payload
  EXPECT_EQ(blocks, 2);
  // A box is another handle to the cell.
  EXPECT_EQ(PaykanShared_new(c), c);
  EXPECT_EQ(PaykanShared_get(c), c);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 2);
  Paykan_retain(c);
  Paykan_retain_inline(c);
  Paykan_retain_nonnull_inline(c);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 5);
  Paykan_release(c);
  Paykan_release_inline(c);
  Paykan_release_nonnull_inline(c);
  Paykan_release(c);
  EXPECT_EQ(Paykan_heap_live_blocks(), blocks);
  PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(count(*p), 1);
  PaykanLocked_unlock(cell(c));
  Paykan_release(c);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(LockedCell, HasOneVTableWhoseWalkIsNothing) {
  PaykanShared *a = PaykanLocked_new(counter(1));
  PaykanShared *b = PaykanLocked_new(nullptr);
  EXPECT_EQ(Paykan_vtable_of(a), Paykan_vtable_of(b));
  EXPECT_NE(Paykan_vtable_of(a), PaykanObject_vtable);
  // A freeze stops at a cell: its walk is Obj's, nothing, so what the cell
  // holds stays counted (FreezeTests.cpp shows it).
  EXPECT_TRUE(vtWalk(a) == &PaykanObject_walk);
  discard(a);
  discard(b);
}

// -- new, lock and unlock

TEST(LockedCell, NewTakesTheBoxAndLockGivesItsSlot) {
  TrackingHeap heap;
  PaykanShared *payload = counter(7);
  EXPECT_EQ(payload->strong, 1);
  PaykanShared *c = PaykanLocked_new(payload);
  // The cell takes the box it is given: no retain.
  EXPECT_EQ(payload->strong, 1);
  PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(*p, payload);
  EXPECT_EQ(count(*p), 7);
  // What a `lock ... inout` block writes stays, and the next lock gives
  // the same slot.
  count(*p) = 8;
  PaykanLocked_unlock(cell(c));
  PaykanShared **again = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(again, p);
  EXPECT_EQ(*again, payload);
  EXPECT_EQ(count(*again), 8);
  PaykanLocked_unlock(cell(c));
  discard(c);
}

// The slot holds whatever box it is given, NULL included (a `locked T?`
// holding None): replacing the contents is a store into the slot, the old
// box released and the new one the cell's.
TEST(LockedCell, TheSlotHoldsTheBoxItIsGiven) {
  TrackingHeap heap;
  PaykanShared *c = PaykanLocked_new(nullptr);
  PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(*p, nullptr);
  *p = counter(1);
  PaykanLocked_unlock(cell(c));
  EXPECT_EQ(Paykan_heap_live_blocks(), 2);
  p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(count(*p), 1);
  PaykanShared *old = *p;
  *p = counter(2);
  Paykan_release(old);
  PaykanLocked_unlock(cell(c));
  EXPECT_EQ(Paykan_heap_live_blocks(), 2);
  p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(count(*p), 2);
  PaykanLocked_unlock(cell(c));
  discard(c);
}

// -- toString and equals

TEST(LockedCell, ToStringIsLocked) {
  PaykanShared *c = PaykanLocked_new(counter(3));
  EXPECT_EQ(toString(c), "locked");
  discard(c);
}

TEST(LockedCell, EqualsIsIdentity) {
  TrackingHeap heap;
  PaykanShared *a = PaykanLocked_new(counter(1));
  PaykanShared *b = PaykanLocked_new(counter(1));
  const int64_t blocks = Paykan_heap_live_blocks();
  EXPECT_EQ(equalsLent(a, a), 1);
  // The same contents in another cell is another cell.
  EXPECT_EQ(equalsLent(a, b), 0);
  EXPECT_EQ(equalsLent(b, a), 0);
  // A NULL box (an absent optional) is equal to nothing.
  EXPECT_EQ(equalsLent(a, nullptr), 0);
  // Any other object is not equal; one handed over (bit 0 of the mask) is
  // released, as every `*_equals` releases it.
  EXPECT_EQ(vtEquals(a)(cell(a), counter(1), 1), 0);
  EXPECT_EQ(Paykan_heap_live_blocks(), blocks);
  // A handle handed over is released.
  Paykan_retain(b);
  EXPECT_EQ(vtEquals(a)(cell(a), b, 1), 0);
  EXPECT_EQ(b->strong, PAYKAN_REF_ATOMIC + 1);
  discard(a);
  discard(b);
}

// -- The mutex

// A thread that locks while another holds the lock waits for the unlock:
// what it reads under the lock is what the holder wrote before unlocking.
TEST(LockedCell, ALockWaitsForTheUnlock) {
  PaykanShared *c = PaykanLocked_new(counter(0));
  PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
  std::atomic<bool> locking{false};
  int64_t seen = -1;
  std::thread other([&] {
    locking = true;
    PaykanShared **q = slot(PaykanLocked_lock(cell(c)));
    seen = count(*q);
    PaykanLocked_unlock(cell(c));
  });
  while (!locking)
    std::this_thread::yield();
  // The other thread is at its lock, or in it: give it time to block
  // there, then write and unlock.  The result does not depend on the
  // timing (the lock is held throughout); the wait only makes the other
  // thread wait for real.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(seen, -1);
  count(*p) = 1;
  PaykanLocked_unlock(cell(c));
  other.join();
  EXPECT_EQ(seen, 1);
  discard(c);
}

// Increments under the lock from several threads are all kept: each
// lock/increment/unlock excludes the others.
TEST(LockedCell, IncrementsUnderTheLockFromThreadsAreAllKept) {
  PaykanShared *c = PaykanLocked_new(counter(0));
  constexpr int kThreads = 4;
  constexpr int kIncrements = 20000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([c] {
      for (int i = 0; i < kIncrements; ++i) {
        PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
        count(*p) += 1;
        PaykanLocked_unlock(cell(c));
      }
    });
  for (std::thread &t : threads)
    t.join();
  PaykanShared **p = slot(PaykanLocked_lock(cell(c)));
  EXPECT_EQ(count(*p), kThreads * kIncrements);
  PaykanLocked_unlock(cell(c));
  discard(c);
}
