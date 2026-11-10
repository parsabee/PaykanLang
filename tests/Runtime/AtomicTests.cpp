// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the concurrency prototype's `atomic T` cell (PaykanAtomic_*,
// src/Runtime/Atomic.c): one heap object holding 64 bits, an integer or a
// double's bits, that any thread reads and changes by atomic operations.
// Its count is atomic (PAYKAN_REF_ATOMIC + n), so the last handle frees it.

#include <gtest/gtest.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
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

/// Frees a cell at the end of a test.  The runtime never does (the
/// prototype leaks every cell, samples/concurrency/README.md "What is
/// not"): the count put at 1 is the thaw the design describes, and the
/// release then frees the block as it frees any object's.
void discard(PaykanShared *handle) { Paykan_release(handle); }

/// compareExchange, its `expected` by value.
int64_t cas(PaykanObject *c, int64_t expected, int64_t desired) {
  return PaykanAtomic_compare_exchange(c, &expected, desired);
}
int64_t fcas(PaykanObject *c, double expected, double desired) {
  return PaykanAtomic_fcompare_exchange(c, &expected, desired);
}

std::string text(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

/// The cell's text through its vtable slot, as `Str(x)` reads it.
std::string toString(PaykanShared *handle) {
  PaykanShared *s = vtToString(handle)(cell(handle));
  EXPECT_EQ(s->strong, 1); // a new, owned string each call
  std::string result = text(s);
  Paykan_release(s);
  return result;
}

/// The equals slot with `other` lent (bit 0 of the mask clear).  Not
/// RuntimeEqualsHelper.h's paykanTestEquals: that undoes by hand the retain
/// boxing takes, by writing a plain count.
int64_t equalsLent(PaykanShared *self, PaykanShared *other) {
  return vtEquals(self)(cell(self), other, 0);
}

using PaykanWalkFn = void (*)(PaykanObject *);
PaykanWalkFn vtWalk(const void *obj) {
  return vtSlot<PaykanWalkFn>(obj, PAYKAN_SLOT_WALK);
}

int64_t bitsOf(double d) {
  int64_t bits;
  std::memcpy(&bits, &d, sizeof bits);
  return bits;
}

constexpr double kInf = std::numeric_limits<double>::infinity();

} // namespace

// -- The cell is counted atomically

TEST(AtomicCell, IsCountedAtomicallyAndFreedByItsLastHandle) {
  TrackingHeap heap;
  PaykanShared *c = PaykanAtomic_new(1, PAYKAN_ATOMIC_INT);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(c->weak, 0);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1);
  // A box is another handle to the cell.
  EXPECT_EQ(PaykanShared_new(c), c);
  EXPECT_EQ(PaykanShared_get(c), c);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 2);
  // A retain, by the function or inline, counts one more.
  Paykan_retain(c);
  Paykan_retain_inline(c);
  Paykan_retain_nonnull_inline(c);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 5);
  // A release counts one less and only the last frees it.
  Paykan_release(c);
  Paykan_release_inline(c);
  Paykan_release_nonnull_inline(c);
  Paykan_release(c);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 1);
  Paykan_release(c);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// Handles retained and released on many threads lose no count: the last
// release frees the cell once.
TEST(AtomicCell, ACountChangedFromManyThreadsIsExact) {
  TrackingHeap heap;
  PaykanShared *c = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  std::vector<std::thread> threads;
  threads.reserve(8);
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([c] {
      for (int i = 0; i < 20000; ++i) {
        Paykan_retain_inline(c);
        Paykan_retain(c);
        Paykan_release_inline(c);
        Paykan_release(c);
      }
    });
  for (std::thread &t : threads)
    t.join();
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(c);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(AtomicCell, HasOneVTableWhoseWalkIsNothing) {
  PaykanShared *a = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  PaykanShared *f = PaykanAtomic_fnew(0, PAYKAN_ATOMIC_F64);
  // One vtable for every kind, not Obj's.
  EXPECT_EQ(Paykan_vtable_of(a), Paykan_vtable_of(f));
  EXPECT_NE(Paykan_vtable_of(a), PaykanObject_vtable);
  // A cell holds no reference, so its walk (what Paykan_freeze calls on
  // what a frozen object holds) is Obj's: nothing.
  EXPECT_TRUE(vtWalk(a) == &PaykanObject_walk);
  // Its destroy tears nothing down: the cell is readable after it.
  vtDestroy(a)(cell(a));
  EXPECT_EQ(PaykanAtomic_load(cell(a)), 0);
  discard(a);
  discard(f);
}

// -- Integer cells

TEST(AtomicInt, LoadStoreAddAndExchange) {
  PaykanShared *c = PaykanAtomic_new(5, PAYKAN_ATOMIC_INT);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 5);
  PaykanAtomic_store(cell(c), 7);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 7);
  // add and exchange give the value found.
  EXPECT_EQ(PaykanAtomic_add(cell(c), 3), 7);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 10);
  EXPECT_EQ(PaykanAtomic_add(cell(c), -20), 10);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), -10);
  EXPECT_EQ(PaykanAtomic_exchange(cell(c), 42), -10);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 42);
  discard(c);
}

TEST(AtomicInt, CompareExchange) {
  PaykanShared *c = PaykanAtomic_new(42, PAYKAN_ATOMIC_INT);
  // The cell holds another value than expected: nothing is stored, 0.
  EXPECT_EQ(cas(cell(c), 1, 9), 0);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 42);
  // It holds the expected value: desired is stored, 1.
  EXPECT_EQ(cas(cell(c), 42, 9), 1);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 9);
  // Expected and desired the same: a store that changes nothing, 1.
  EXPECT_EQ(cas(cell(c), 9, 9), 1);
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 9);
  discard(c);
}

// The kind is for the text and a float's rounding only: an integer
// operation is the same 64-bit operation on every integer kind, and wraps
// (the lowering wraps a narrow integer's reads back into its range; an
// `unsigned` cell's text is its bits unsigned, AtomicToString below).
TEST(AtomicInt, TheCellHoldsBitsWhateverTheKind) {
  for (int64_t kind : {PAYKAN_ATOMIC_INT, PAYKAN_ATOMIC_UINT,
                       PAYKAN_ATOMIC_BOOL, PAYKAN_ATOMIC_CHAR}) {
    PaykanShared *c = PaykanAtomic_new(INT64_MAX, kind);
    EXPECT_EQ(PaykanAtomic_add(cell(c), 1), INT64_MAX) << kind;
    EXPECT_EQ(PaykanAtomic_load(cell(c)), INT64_MIN) << kind;
    PaykanAtomic_store(cell(c), -1); // every bit set: the largest unsigned
    EXPECT_EQ(PaykanAtomic_add(cell(c), 1), -1) << kind;
    EXPECT_EQ(PaykanAtomic_load(cell(c)), 0) << kind;
    discard(c);
  }
}

// Adds from several threads are each one atomic operation: none is lost.
TEST(AtomicInt, AddsFromThreadsAreNotLost) {
  PaykanShared *c = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  constexpr int kThreads = 8;
  constexpr int kAdds = 20000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([c] {
      for (int i = 0; i < kAdds; ++i)
        PaykanAtomic_add(cell(c), 1);
    });
  for (std::thread &t : threads)
    t.join();
  EXPECT_EQ(PaykanAtomic_load(cell(c)), kThreads * kAdds);
  discard(c);
}

// A compare-and-exchange from every thread at once: exactly one finds the
// value expected.
TEST(AtomicInt, OneCompareExchangeOfManyWins) {
  PaykanShared *c = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  constexpr int kThreads = 8;
  std::atomic<int> won{0};
  std::vector<std::thread> threads;
  for (int t = 1; t <= kThreads; ++t)
    threads.emplace_back([c, t, &won] {
      if (cas(cell(c), 0, t))
        ++won;
    });
  for (std::thread &t : threads)
    t.join();
  EXPECT_EQ(won.load(), 1);
  const int64_t winner = PaykanAtomic_load(cell(c));
  EXPECT_GE(winner, 1);
  EXPECT_LE(winner, kThreads);
  discard(c);
}

// -- Float cells: the cell holds the double's bits

TEST(AtomicFloat, LoadStoreAddAndExchange) {
  PaykanShared *c = PaykanAtomic_fnew(1.5, PAYKAN_ATOMIC_F64);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 1.5);
  PaykanAtomic_fstore(cell(c), 2.25);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 2.25);
  // fadd and fexchange give the value found.
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 0.5), 2.25);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 2.75);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), -3.0), 2.75);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), -0.25);
  EXPECT_EQ(PaykanAtomic_fexchange(cell(c), 1e300), -0.25);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 1e300);
  // An f64 cell rounds nothing.
  PaykanAtomic_fstore(cell(c), 0.1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 0.1);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 1e-9), 0.1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 0.1 + 1e-9);
  // The integer view of the cell is the double's bits.
  EXPECT_EQ(PaykanAtomic_load(cell(c)), bitsOf(0.1 + 1e-9));
  PaykanAtomic_store(cell(c), bitsOf(-2.0));
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), -2.0);
  discard(c);
}

TEST(AtomicFloat, CompareExchangeComparesTheBits) {
  PaykanShared *c = PaykanAtomic_fnew(0.0, PAYKAN_ATOMIC_F64);
  // -0.0 == 0.0, but their bits differ: not the value expected.
  EXPECT_EQ(fcas(cell(c), -0.0, 1.0), 0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 0.0);
  EXPECT_FALSE(std::signbit(PaykanAtomic_fload(cell(c))));
  EXPECT_EQ(fcas(cell(c), 0.0, -0.0), 1);
  EXPECT_TRUE(std::signbit(PaykanAtomic_fload(cell(c))));
  // A NaN is never == itself, but its bits are: the compare finds it.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  PaykanAtomic_fstore(cell(c), nan);
  EXPECT_TRUE(std::isnan(PaykanAtomic_fload(cell(c))));
  EXPECT_EQ(fcas(cell(c), nan, 2.0), 1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 2.0);
  EXPECT_EQ(fcas(cell(c), 2.5, 3.0), 0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 2.0);
  discard(c);
}

// The CAS loop of fadd retries on a value another thread changed: adds from
// several threads are all kept (each sum here is exact in a double).
TEST(AtomicFloat, AddsFromThreadsAreNotLost) {
  PaykanShared *c = PaykanAtomic_fnew(0.0, PAYKAN_ATOMIC_F64);
  constexpr int kThreads = 8;
  constexpr int kAdds = 20000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([c] {
      for (int i = 0; i < kAdds; ++i)
        PaykanAtomic_fadd(cell(c), 1.0);
    });
  for (std::thread &t : threads)
    t.join();
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(kThreads * kAdds));
  discard(c);
}

// An f32 cell holds every value rounded to f32, as the lowering rounds
// every f32 arithmetic result: a store, an add's sum, an exchange and a
// compare-exchange's desired value.
TEST(AtomicFloat, AnF32CellRoundsEveryStore) {
  const double third = 1.0 / 3.0;
  const double thirdF32 = double(float(third));
  ASSERT_NE(thirdF32, third);
  PaykanShared *c = PaykanAtomic_fnew(third, PAYKAN_ATOMIC_F32);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), thirdF32);
  PaykanAtomic_fstore(cell(c), 0.1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(float(0.1)));
  // The sum is rounded: 1e-9 is under half an f32 ulp of 1 (2^-23), so
  // adding it leaves 1; 1e-7 is over, so adding it gives 1 + 2^-23.
  PaykanAtomic_fstore(cell(c), 1.0);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 1e-9), 1.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 1.0);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 1e-7), 1.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 1.0 + 0x1p-23);
  // An exchange stores the rounded value and gives the value found.
  EXPECT_EQ(PaykanAtomic_fexchange(cell(c), 0.2), 1.0 + 0x1p-23);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(float(0.2)));
  // The expected value is compared as given, against the rounded value the
  // cell holds: 0.2 as a double is not in the cell, its f32 rounding is.
  // The desired value is rounded like any store.
  EXPECT_EQ(fcas(cell(c), 0.2, 0.3), 0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(float(0.2)));
  EXPECT_EQ(fcas(cell(c), double(float(0.2)), 0.3), 1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(float(0.3)));
  discard(c);
}

TEST(AtomicFloat, AnF32CellOverflowsToInfinity) {
  PaykanShared *c = PaykanAtomic_fnew(FLT_MAX, PAYKAN_ATOMIC_F32);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), double(FLT_MAX));
  // From half an ulp above the f32 maximum a value rounds to infinity (the
  // C conversion of such a value is undefined, so the runtime tests it).
  PaykanAtomic_fstore(cell(c), 3.4028235677973366e+38);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), kInf);
  PaykanAtomic_fstore(cell(c), -1e39);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), -kInf);
  // A sum past the maximum too.
  PaykanAtomic_fstore(cell(c), FLT_MAX);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), FLT_MAX), double(FLT_MAX));
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), kInf);
  // Infinity and NaN are f32 values: stored as they are.
  PaykanAtomic_fstore(cell(c), -kInf);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), -kInf);
  PaykanAtomic_fstore(cell(c), std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::isnan(PaykanAtomic_fload(cell(c))));
  discard(c);
}

// An f16 cell rounds the same way to f16 (Paykan_round_f16): 10 mantissa
// bits, a maximum of 65504.
TEST(AtomicFloat, AnF16CellRoundsEveryStore) {
  PaykanShared *c = PaykanAtomic_fnew(0.1, PAYKAN_ATOMIC_F16);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 0.0999755859375);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), Paykan_round_f16(0.1));
  // The quantum of [512, 1024) is 0.5: a sum of 1000.3 rounds to 1000.5.
  PaykanAtomic_fstore(cell(c), 1000.0);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 0.3), 1000.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 1000.5);
  // That of [2048, 4096) is 2: adding 0.5 to 2048 is lost.
  PaykanAtomic_fstore(cell(c), 2048.0);
  EXPECT_EQ(PaykanAtomic_fadd(cell(c), 0.5), 2048.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 2048.0);
  EXPECT_EQ(PaykanAtomic_fexchange(cell(c), 65504.0), 2048.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 65504.0);
  // From half an ulp above the maximum a value rounds to infinity.
  PaykanAtomic_fstore(cell(c), 65520.0);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), kInf);
  // A compare-exchange's desired value is rounded like any store.
  EXPECT_EQ(fcas(cell(c), kInf, -0.1), 1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), -0.0999755859375);
  discard(c);
}

// -- toString: by the cell's kind

TEST(AtomicToString, IntegerKinds) {
  struct Case {
    int64_t kind;
    int64_t value;
    const char *text;
  };
  const Case cases[] = {
      {PAYKAN_ATOMIC_INT, -5, "-5"},
      {PAYKAN_ATOMIC_INT, INT64_MIN, "-9223372036854775808"},
      {PAYKAN_ATOMIC_UINT, -1, "18446744073709551615"}, // the bits, unsigned
      {PAYKAN_ATOMIC_UINT, 7, "7"},
      {PAYKAN_ATOMIC_BOOL, 1, "True"},
      {PAYKAN_ATOMIC_BOOL, 0, "False"},
      {PAYKAN_ATOMIC_CHAR, 'x', "x"},
  };
  for (const Case &k : cases) {
    PaykanShared *c = PaykanAtomic_new(k.value, k.kind);
    EXPECT_EQ(toString(c), k.text) << k.kind << " " << k.value;
    discard(c);
  }
}

TEST(AtomicToString, FloatKinds) {
  struct Case {
    int64_t kind;
    double value;
    const char *text;
  };
  // As Str<float> prints: "%g", with "nan", "inf" and "-inf" spelled
  // canonically; an f32 or f16 cell prints the rounded value it holds.
  const Case cases[] = {
      {PAYKAN_ATOMIC_F64, 2.5, "2.5"},
      {PAYKAN_ATOMIC_F64, -1.0, "-1"},
      {PAYKAN_ATOMIC_F64, 1e20, "1e+20"},
      {PAYKAN_ATOMIC_F64, std::numeric_limits<double>::quiet_NaN(), "nan"},
      {PAYKAN_ATOMIC_F64, -kInf, "-inf"},
      {PAYKAN_ATOMIC_F32, 0.1, "0.1"},
      {PAYKAN_ATOMIC_F32, 1e39, "inf"},
      {PAYKAN_ATOMIC_F16, 0.1, "0.0999756"},
      {PAYKAN_ATOMIC_F16, 65504.0, "65504"},
  };
  for (const Case &k : cases) {
    PaykanShared *c = PaykanAtomic_fnew(k.value, k.kind);
    EXPECT_EQ(toString(c), k.text) << k.kind << " " << k.value;
    discard(c);
  }
}

// -- equals: identity

TEST(AtomicEquals, IsIdentity) {
  TrackingHeap heap;
  PaykanShared *a = PaykanAtomic_new(1, PAYKAN_ATOMIC_INT);
  PaykanShared *b = PaykanAtomic_new(1, PAYKAN_ATOMIC_INT);
  const int64_t blocks = Paykan_heap_live_blocks(); // the two cells
  EXPECT_EQ(equalsLent(a, a), 1);
  // The same value in another cell is another cell.
  EXPECT_EQ(equalsLent(a, b), 0);
  EXPECT_EQ(equalsLent(b, a), 0);
  // A NULL box (an absent optional) is equal to nothing.
  EXPECT_EQ(equalsLent(a, nullptr), 0);
  // Any other object is not equal; one handed over (bit 0 of the mask) is
  // released, as every `*_equals` releases it.
  PaykanShared *s = PaykanShared_new((PaykanObject *)PaykanString_new("1", 1));
  EXPECT_EQ(vtEquals(a)(cell(a), s, 1), 0);
  EXPECT_EQ(Paykan_heap_live_blocks(), blocks);
  // A handle handed over is released.
  Paykan_retain(b);
  EXPECT_EQ(vtEquals(a)(cell(a), b, 1), 0);
  EXPECT_EQ(b->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_retain(a);
  EXPECT_EQ(vtEquals(a)(cell(a), a, 1), 1);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  discard(a);
  discard(b);
}

// -- compareExchange writes the value it found into `expected`

TEST(AtomicInt, ACompareExchangeThatFailsWritesTheValueFound) {
  PaykanShared *c = PaykanAtomic_new(5, PAYKAN_ATOMIC_INT);
  int64_t e = 4;
  EXPECT_EQ(PaykanAtomic_compare_exchange(cell(c), &e, 9), 0);
  EXPECT_EQ(e, 5);
  EXPECT_EQ(PaykanAtomic_compare_exchange(cell(c), &e, 9), 1);
  EXPECT_EQ(e, 5); // left alone
  EXPECT_EQ(PaykanAtomic_load(cell(c)), 9);
  discard(c);
}

TEST(AtomicFloat, ACompareExchangeThatFailsWritesTheValueFound) {
  PaykanShared *c = PaykanAtomic_fnew(1.5, PAYKAN_ATOMIC_F64);
  double e = 2.5;
  EXPECT_EQ(PaykanAtomic_fcompare_exchange(cell(c), &e, 9.0), 0);
  EXPECT_EQ(e, 1.5);
  EXPECT_EQ(PaykanAtomic_fcompare_exchange(cell(c), &e, 9.0), 1);
  EXPECT_EQ(PaykanAtomic_fload(cell(c)), 9.0);
  discard(c);
}

// -- `atomic T[n]`

TEST(AtomicArray, IsUsedByElementAndFreedByItsLastHandle) {
  TrackingHeap heap;
  PaykanShared *a = PaykanAtomicArray_new(4, PAYKAN_ATOMIC_INT);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(PaykanAtomicArray_load(cell(a), 2), 0);
  PaykanAtomicArray_store(cell(a), 2, 7);
  EXPECT_EQ(PaykanAtomicArray_add(cell(a), 2, 3), 7);
  EXPECT_EQ(PaykanAtomicArray_exchange(cell(a), 2, 20), 10);
  int64_t e = 1;
  EXPECT_EQ(PaykanAtomicArray_compare_exchange(cell(a), 2, &e, 21), 0);
  EXPECT_EQ(e, 20);
  EXPECT_EQ(PaykanAtomicArray_compare_exchange(cell(a), 2, &e, 21), 1);
  EXPECT_EQ(PaykanAtomicArray_load(cell(a), 2), 21);
  EXPECT_EQ(PaykanAtomicArray_load(cell(a), 3), 0);
  PaykanShared *copy = PaykanAtomicArray_clone(cell(a));
  PaykanAtomicArray_store(cell(a), 2, 1);
  EXPECT_EQ(PaykanAtomicArray_load(cell(copy), 2), 21);
  discard(copy);
  discard(a);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(AtomicArray, FloatsRoundToTheirKind) {
  PaykanShared *a = PaykanAtomicArray_new(2, PAYKAN_ATOMIC_F32);
  PaykanAtomicArray_fstore(cell(a), 1, 0.1);
  EXPECT_EQ(PaykanAtomicArray_fload(cell(a), 1), (double)0.1f);
  EXPECT_EQ(PaykanAtomicArray_fadd(cell(a), 1, 1.0), (double)0.1f);
  EXPECT_EQ(PaykanAtomicArray_fexchange(cell(a), 1, 2.0), (double)1.1f);
  double e = 3.0;
  EXPECT_EQ(PaykanAtomicArray_fcompare_exchange(cell(a), 1, &e, 4.0), 0);
  EXPECT_EQ(e, 2.0);
  discard(a);
}

TEST(AtomicArrayDeathTest, AnIndexOutOfRangeStopsTheProgram) {
  PaykanShared *a = PaykanAtomicArray_new(2, PAYKAN_ATOMIC_INT);
  EXPECT_DEATH(PaykanAtomicArray_load(cell(a), 2), "out of range");
  EXPECT_DEATH(PaykanAtomicArray_store(cell(a), -1, 0), "out of range");
  discard(a);
}
