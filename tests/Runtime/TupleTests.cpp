// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for the PaykanTuple runtime object (prototype): construction with
// per-slot kinds, raw / reference slot access, destroy releasing exactly the
// reference slots, element-wise equals, and toString rendering.

#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"

// ============================================================================
// Helpers
// ============================================================================

static int64_t bitsOf(double d) {
  int64_t b;
  memcpy(&b, &d, sizeof(b));
  return b;
}

static double doubleOf(int64_t b) {
  double d;
  memcpy(&d, &b, sizeof(d));
  return d;
}

static PaykanShared *boxStr(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

static std::string render(PaykanTuple *t) {
  PaykanShared *s = PaykanTuple_toString((PaykanObject *)t);
  std::string out(((PaykanString *)PaykanShared_get(s))->data);
  Paykan_release(s);
  return out;
}

static int64_t tupleEq(PaykanTuple *a, PaykanTuple *b) {
  return paykanTestEquals(PaykanTuple_equals, (PaykanObject *)a,
                          (PaykanObject *)b);
}

// (int, Str) with the given values; the Str slot is retained by the tuple.
static PaykanTuple *makeIntStr(int64_t n, const char *s) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_REF};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  PaykanTuple_set(t, 0, n);
  PaykanShared *box = boxStr(s);
  PaykanTuple_set_obj(t, 1, box);
  Paykan_release(box); // the tuple holds the surviving reference
  return t;
}

// ============================================================================
// Construction
// ============================================================================

TEST(TupleNew, CountKindsAndVtable) {
  const uint8_t kinds[3] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_REF,
                            PAYKAN_TUPLE_FLOAT};
  PaykanTuple *t = PaykanTuple_new(3, kinds);
  EXPECT_EQ(t->count, 3);
  EXPECT_EQ(PaykanTuple_count(t), 3);
  EXPECT_EQ((void *)t->vtable, (void *)&PaykanTuple_vtable);
  EXPECT_EQ(t->shared, nullptr);
  EXPECT_EQ(PaykanTuple_kind(t, 0), PAYKAN_TUPLE_INT);
  EXPECT_EQ(PaykanTuple_kind(t, 1), PAYKAN_TUPLE_REF);
  EXPECT_EQ(PaykanTuple_kind(t, 2), PAYKAN_TUPLE_FLOAT);
  // The kinds are copied, not aliased.
  EXPECT_NE((const void *)t->kinds, (const void *)kinds);
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleNew, SlotsAreZeroInitialised) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_INT};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  EXPECT_EQ(PaykanTuple_get(t, 0), 0);
  EXPECT_EQ(PaykanTuple_get(t, 1), 0);
  // Destroying with a NULL reference slot is a no-op release.
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleNew, SingleHeapBlock) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  const uint8_t kinds[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_INT};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1);
  PaykanTuple_destroy((PaykanObject *)t);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

// ============================================================================
// Primitive slots
// ============================================================================

TEST(TupleSetGet, IntFloatBoolChar) {
  const uint8_t kinds[4] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_FLOAT,
                            PAYKAN_TUPLE_BOOL, PAYKAN_TUPLE_CHAR};
  PaykanTuple *t = PaykanTuple_new(4, kinds);
  PaykanTuple_set(t, 0, -42);
  PaykanTuple_set(t, 1, bitsOf(2.5));
  PaykanTuple_set(t, 2, 1);
  PaykanTuple_set(t, 3, (int64_t)'z');
  EXPECT_EQ(PaykanTuple_get(t, 0), -42);
  EXPECT_DOUBLE_EQ(doubleOf(PaykanTuple_get(t, 1)), 2.5);
  EXPECT_EQ(PaykanTuple_get(t, 2), 1);
  EXPECT_EQ((char)PaykanTuple_get(t, 3), 'z');
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleSetGet, OutOfBoundsDies) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_INT};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  EXPECT_DEATH(PaykanTuple_get(t, 2), "out of bounds");
  EXPECT_DEATH(PaykanTuple_set(t, -1, 0), "out of bounds");
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleSetGet, KindMismatchDies) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_REF};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  EXPECT_DEATH(PaykanTuple_set(t, 1, 0), "reference slot");
  EXPECT_DEATH(PaykanTuple_set_obj(t, 0, nullptr), "value slot");
  PaykanTuple_destroy((PaykanObject *)t);
}

// ============================================================================
// Reference slots
// ============================================================================

TEST(TupleSetObj, RetainsStoredBoxAndReleasesOld) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_REF};
  PaykanTuple *t = PaykanTuple_new(2, kinds);

  PaykanShared *a = boxStr("a");
  EXPECT_EQ(a->refCount, 1);
  PaykanTuple_set_obj(t, 0, a);
  EXPECT_EQ(a->refCount, 2); // tuple + test

  PaykanShared *got;
  int64_t bits = PaykanTuple_get(t, 0);
  memcpy(&got, &bits, sizeof(bits));
  EXPECT_EQ(got, a); // get returns the box bits without retaining
  EXPECT_EQ(a->refCount, 2);

  PaykanShared *b = boxStr("b");
  PaykanTuple_set_obj(t, 0, b); // replaces a: a released, b retained
  EXPECT_EQ(a->refCount, 1);
  EXPECT_EQ(b->refCount, 2);

  PaykanTuple_destroy((PaykanObject *)t);
  EXPECT_EQ(b->refCount, 1); // destroy released the tuple's reference
  Paykan_release(a);
  Paykan_release(b);
}

TEST(TupleDestroy, ReleasesExactlyTheReferenceSlots) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    const uint8_t kinds[3] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_REF,
                              PAYKAN_TUPLE_REF};
    PaykanTuple *t = PaykanTuple_new(3, kinds);
    PaykanTuple_set(t, 0, 7);
    PaykanShared *s = boxStr("owned by tuple");
    PaykanTuple_set_obj(t, 1, s);
    Paykan_release(s); // tuple now the sole owner
    // slot 2 left NULL
    PaykanTuple_destroy((PaykanObject *)t);
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0); // string, its box, and the tuple
  Paykan_heap_set_tracking(0);
}

TEST(TupleDestroy, NestedTupleIsReleased) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanTuple *inner = makeIntStr(1, "in");
    PaykanShared *innerBox = PaykanShared_new((PaykanObject *)inner);
    const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_INT};
    PaykanTuple *outer = PaykanTuple_new(2, kinds);
    PaykanTuple_set_obj(outer, 0, innerBox);
    Paykan_release(innerBox);
    PaykanTuple_set(outer, 1, 2);
    EXPECT_EQ(render(outer), "((1, in), 2)");
    PaykanTuple_destroy((PaykanObject *)outer);
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

// ============================================================================
// toString
// ============================================================================

TEST(TupleToString, RendersEachKind) {
  const uint8_t kinds[5] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_FLOAT,
                            PAYKAN_TUPLE_BOOL, PAYKAN_TUPLE_CHAR,
                            PAYKAN_TUPLE_REF};
  PaykanTuple *t = PaykanTuple_new(5, kinds);
  PaykanTuple_set(t, 0, 1);
  PaykanTuple_set(t, 1, bitsOf(2.5));
  PaykanTuple_set(t, 2, 1);
  PaykanTuple_set(t, 3, (int64_t)'c');
  PaykanShared *s = boxStr("a");
  PaykanTuple_set_obj(t, 4, s);
  Paykan_release(s);
  EXPECT_EQ(render(t), "(1, 2.5, True, c, a)");
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleToString, NullReferenceRendersNone) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_INT};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  PaykanTuple_set(t, 1, 0);
  EXPECT_EQ(render(t), "(None, 0)");
  PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleToString, LongRenderingGrowsBuffer) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_REF};
  PaykanTuple *t = PaykanTuple_new(2, kinds);
  std::string big(200, 'x');
  PaykanShared *s = boxStr(big.c_str());
  PaykanTuple_set_obj(t, 0, s);
  PaykanTuple_set_obj(t, 1, s);
  Paykan_release(s);
  EXPECT_EQ(render(t), "(" + big + ", " + big + ")");
  PaykanTuple_destroy((PaykanObject *)t);
}

// ============================================================================
// equals
// ============================================================================

TEST(TupleEquals, ElementWiseOnPrimitivesAndStrings) {
  PaykanTuple *a = makeIntStr(1, "x");
  PaykanTuple *b = makeIntStr(1, "x"); // distinct Str object, same content
  PaykanTuple *c = makeIntStr(2, "x");
  PaykanTuple *d = makeIntStr(1, "y");
  EXPECT_EQ(tupleEq(a, a), 1);
  EXPECT_EQ(tupleEq(a, b), 1);
  EXPECT_EQ(tupleEq(a, c), 0);
  EXPECT_EQ(tupleEq(a, d), 0);
  for (PaykanTuple *t : {a, b, c, d})
    PaykanTuple_destroy((PaykanObject *)t);
}

TEST(TupleEquals, FloatsCompareByValue) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_FLOAT, PAYKAN_TUPLE_BOOL};
  PaykanTuple *a = PaykanTuple_new(2, kinds);
  PaykanTuple *b = PaykanTuple_new(2, kinds);
  PaykanTuple_set(a, 0, bitsOf(0.5));
  PaykanTuple_set(b, 0, bitsOf(0.5));
  PaykanTuple_set(a, 1, 1);
  PaykanTuple_set(b, 1, 1);
  EXPECT_EQ(tupleEq(a, b), 1);
  PaykanTuple_set(b, 0, bitsOf(0.25));
  EXPECT_EQ(tupleEq(a, b), 0);
  PaykanTuple_destroy((PaykanObject *)a);
  PaykanTuple_destroy((PaykanObject *)b);
}

TEST(TupleEquals, ArityAndKindMismatchAreUnequal) {
  const uint8_t k2[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_INT};
  const uint8_t k3[3] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_INT, PAYKAN_TUPLE_INT};
  const uint8_t kf[2] = {PAYKAN_TUPLE_INT, PAYKAN_TUPLE_FLOAT};
  PaykanTuple *a = PaykanTuple_new(2, k2);
  PaykanTuple *b = PaykanTuple_new(3, k3);
  PaykanTuple *c = PaykanTuple_new(2, kf); // same bits (all zero), other kind
  EXPECT_EQ(tupleEq(a, b), 0);
  EXPECT_EQ(tupleEq(a, c), 0);
  PaykanTuple_destroy((PaykanObject *)a);
  PaykanTuple_destroy((PaykanObject *)b);
  PaykanTuple_destroy((PaykanObject *)c);
}

TEST(TupleEquals, NullReferenceSlots) {
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_INT};
  PaykanTuple *a = PaykanTuple_new(2, kinds);
  PaykanTuple *b = PaykanTuple_new(2, kinds);
  EXPECT_EQ(tupleEq(a, b), 1); // both NULL
  PaykanShared *s = boxStr("s");
  PaykanTuple_set_obj(b, 0, s);
  Paykan_release(s);
  EXPECT_EQ(tupleEq(a, b), 0); // NULL vs non-NULL
  EXPECT_EQ(tupleEq(b, a), 0);
  PaykanTuple_destroy((PaykanObject *)a);
  PaykanTuple_destroy((PaykanObject *)b);
}

TEST(TupleEquals, NestedTuplesAndNonTupleOther) {
  PaykanTuple *ia = makeIntStr(1, "x");
  PaykanTuple *ib = makeIntStr(1, "x");
  const uint8_t kinds[2] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_INT};
  PaykanTuple *a = PaykanTuple_new(2, kinds);
  PaykanTuple *b = PaykanTuple_new(2, kinds);
  PaykanShared *ba = PaykanShared_new((PaykanObject *)ia);
  PaykanShared *bb = PaykanShared_new((PaykanObject *)ib);
  PaykanTuple_set_obj(a, 0, ba);
  PaykanTuple_set_obj(b, 0, bb);
  Paykan_release(ba);
  Paykan_release(bb);
  EXPECT_EQ(tupleEq(a, b), 1);
  // A non-tuple `other` is never equal.
  PaykanString *str = PaykanString_new("t", 1);
  EXPECT_EQ(paykanTestEquals(PaykanTuple_equals, (PaykanObject *)a,
                             (PaykanObject *)str),
            0);
  PaykanString_destroy((PaykanObject *)str);
  PaykanTuple_destroy((PaykanObject *)a);
  PaykanTuple_destroy((PaykanObject *)b);
}

TEST(TupleEquals, DoesNotLeakOrDoubleFree) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  {
    PaykanTuple *a = makeIntStr(1, "x");
    PaykanTuple *b = makeIntStr(1, "x");
    // Exercise the consumed-box ABI the way generated code does: `other`
    // arrives as a +1 box that equals releases.
    PaykanShared *bb = PaykanShared_new((PaykanObject *)b);
    Paykan_retain(bb);
    EXPECT_EQ(PaykanTuple_equals((PaykanObject *)a, bb), 1);
    Paykan_release(bb); // destroys b
    PaykanTuple_destroy((PaykanObject *)a);
  }
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}
