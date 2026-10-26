// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the strong references (PaykanShared: the object itself,
// counted in its header).

#include <gtest/gtest.h>

#include <cstring>
#include <initializer_list>

extern "C" {
#include "Runtime.h"
}

// -- PaykanShared_new

TEST(SharedNew, InitialRefCountIsOne) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s);
}

TEST(SharedNew, IsTheObject) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  EXPECT_EQ(s, obj);
  EXPECT_EQ(PaykanShared_get(s), obj);
  Paykan_release(s);
}

TEST(SharedNew, NullIsNull) { EXPECT_EQ(PaykanShared_new(nullptr), nullptr); }

TEST(SharedNew, ANewObjectIsUnowned) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(obj->strong, 0);
  EXPECT_EQ(obj->weak, 0);
  Paykan_dispose(obj);
}

TEST(SharedNew, TheHeaderIsAVTableAndTwo32BitCounts) {
  EXPECT_EQ(sizeof(PaykanObject), 16u);
  EXPECT_EQ(sizeof(PaykanObject::strong), 4u);
  EXPECT_EQ(sizeof(PaykanObject::weak), 4u);
}

// -- Paykan_retain

TEST(SharedRetainDeathTest, ACountPastTheLimitPanics) {
  // A count never wraps: one reference past PAYKAN_REF_MAX panics.
  PaykanObject *obj = PaykanObject_new();
  obj->strong = PAYKAN_REF_MAX;
  EXPECT_DEATH(Paykan_retain(obj), "too many strong references");
  EXPECT_DEATH(PaykanShared_new(obj), "too many strong references");
  EXPECT_DEATH(Paykan_weak_load(obj), "too many strong references");
  obj->strong = 1;
  obj->weak = PAYKAN_REF_MAX;
  EXPECT_DEATH(Paykan_weak_new(obj), "too many weak references");
  obj->weak = 0;
  Paykan_release(obj);
}

TEST(SharedRetain, IncrementsRefCount) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  Paykan_retain(s);
  EXPECT_EQ(s->strong, 2);
  Paykan_release(s);
  Paykan_release(s);
}

TEST(SharedRetain, MultipleRetains) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  Paykan_retain(s);
  Paykan_retain(s);
  Paykan_retain(s);
  EXPECT_EQ(s->strong, 4);
  Paykan_release(s);
  Paykan_release(s);
  Paykan_release(s);
  Paykan_release(s);
}

TEST(SharedRetain, NullIsNoOp) {
  // Should not crash.
  Paykan_retain(nullptr);
}

// -- Paykan_release

TEST(SharedRelease, DecrementsRefCount) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  Paykan_retain(s); // refcount = 2
  EXPECT_EQ(s->strong, 2);
  Paykan_release(s); // refcount = 1
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s); // refcount = 0 -> freed
}

TEST(SharedRelease, FreesAtZero) {
  // Use a String so we can detect double-free via ASAN if enabled.
  PaykanObject *obj = (PaykanObject *)PaykanString_new("bye", 3);
  PaykanShared *s = PaykanShared_new(obj);
  // Single release should call destroy and free.  If this crashes, the
  // destructor or free is broken.
  Paykan_release(s);
}

TEST(SharedRelease, NullIsNoOp) { Paykan_release(nullptr); }

TEST(SharedRelease, RetainThenReleasePair) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  for (int i = 0; i < 10; ++i)
    Paykan_retain(s);
  EXPECT_EQ(s->strong, 11);
  for (int i = 0; i < 10; ++i)
    Paykan_release(s);
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s);
}

// -- PaykanShared_get

TEST(SharedGet, ReturnsWrappedObject) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *s = PaykanShared_new(obj);
  EXPECT_EQ(PaykanShared_get(s), obj);
  Paykan_release(s);
}

TEST(SharedGet, NullReturnsNull) {
  EXPECT_EQ(PaykanShared_get(nullptr), nullptr);
}

// -- Destroy via vtable is called on release-to-zero

TEST(SharedRelease, CallsVtableDestroy) {
  // Wrap a String — its destroy frees its data buffer.  Running under ASAN
  // will catch any use-after-free or leak.
  PaykanString *str = PaykanString_new("hello", 5);
  PaykanShared *s = PaykanShared_new((PaykanObject *)str);
  Paykan_retain(s);  // refcount = 2
  Paykan_release(s); // refcount = 1 — must NOT destroy yet
  // str->data should still be valid here.
  EXPECT_STREQ(str->data, "hello");
  Paykan_release(s); // refcount = 0 — destroys str and frees box
}

// -- The inline fast paths
//
// Paykan_retain_inline, Paykan_release_inline and PaykanShared_get_inline
// (what the generated code does) do exactly what their functions do, at
// every count: the common case inline, the rest through the function.

namespace {

int gDestroys = 0;

void Counted_destroy(PaykanObject *) { ++gDestroys; }

PaykanMethod Counted_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Counted_destroy),
    reinterpret_cast<PaykanMethod>(&PaykanObject_toString),
    reinterpret_cast<PaykanMethod>(&PaykanObject_equals),
};

/// An object whose destroy counts in gDestroys, boxed (strong 1).
PaykanShared *counted() {
  auto *o = static_cast<PaykanObject *>(Paykan_malloc(sizeof(PaykanObject)));
  std::memset(o, 0, sizeof *o);
  o->vtable = Counted_vtable;
  return PaykanShared_new(o);
}

} // namespace

TEST(SharedInline, RetainIncrementsAsPaykanRetain) {
  for (int32_t n : {1, 2, 1000, PAYKAN_REF_MAX - 1}) {
    PaykanShared *a = counted();
    PaykanShared *b = counted();
    a->strong = n;
    b->strong = n;
    Paykan_retain_inline(a);
    Paykan_retain(b);
    EXPECT_EQ(a->strong, n + 1);
    EXPECT_EQ(a->strong, b->strong);
    a->strong = 1;
    b->strong = 1;
    Paykan_release(a);
    Paykan_release(b);
  }
}

TEST(SharedInlineDeathTest, ARetainPastTheLimitPanics) {
  // The limit is the function's: the inline path calls it there.
  PaykanShared *s = counted();
  s->strong = PAYKAN_REF_MAX;
  EXPECT_DEATH(Paykan_retain_inline(s), "too many strong references");
  s->strong = 1;
  Paykan_release(s);
}

TEST(SharedInline, AReleaseAboveOneOnlyDecrements) {
  gDestroys = 0;
  PaykanShared *s = counted();
  Paykan_retain_inline(s);
  Paykan_retain_inline(s);
  EXPECT_EQ(s->strong, 3);
  Paykan_release_inline(s);
  Paykan_release_inline(s);
  EXPECT_EQ(s->strong, 1);
  EXPECT_EQ(gDestroys, 0);
  Paykan_release_inline(s);
  EXPECT_EQ(gDestroys, 1);
}

TEST(SharedInline, TheLastReleaseDestroysAndFrees) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  gDestroys = 0;
  PaykanShared *s = counted();
  Paykan_release_inline(s);
  EXPECT_EQ(gDestroys, 1);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  // A string's destroy frees its data too.
  Paykan_release_inline(
      PaykanShared_new((PaykanObject *)PaykanString_new("bye", 3)));
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

TEST(SharedInline, AWeakReferenceOutlivesTheLastRelease) {
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  gDestroys = 0;
  PaykanShared *s = counted();
  PaykanWeak *w = Paykan_weak_new(s);
  Paykan_release_inline(s);
  EXPECT_EQ(gDestroys, 1);
  EXPECT_EQ(Paykan_weak_load(w), nullptr);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1); // the zombie, until its weak goes
  Paykan_weak_release(w);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

// The variants for a box that is never NULL do the same at every count.
TEST(SharedInline, TheNonNullVariantsCountAsTheOthers) {
  gDestroys = 0;
  for (int32_t n : {1, 2, 1000, PAYKAN_REF_MAX - 1}) {
    PaykanShared *s = counted();
    s->strong = n;
    Paykan_retain_nonnull_inline(s);
    EXPECT_EQ(s->strong, n + 1);
    Paykan_release_nonnull_inline(s);
    EXPECT_EQ(s->strong, n);
    s->strong = 1;
    Paykan_release(s);
  }
  EXPECT_EQ(gDestroys, 4);
  Paykan_heap_set_tracking(1);
  Paykan_heap_reset();
  PaykanShared *s = counted();
  Paykan_release_nonnull_inline(s);
  EXPECT_EQ(gDestroys, 5);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
  Paykan_heap_set_tracking(0);
}

TEST(SharedInlineDeathTest, ANonNullRetainPastTheLimitPanics) {
  PaykanShared *s = counted();
  s->strong = PAYKAN_REF_MAX;
  EXPECT_DEATH(Paykan_retain_nonnull_inline(s), "too many strong references");
  s->strong = 1;
  Paykan_release(s);
}

TEST(SharedInline, NullIsANoOp) {
  Paykan_retain_inline(nullptr);
  Paykan_release_inline(nullptr);
  EXPECT_EQ(PaykanShared_get_inline(nullptr), nullptr);
}

TEST(SharedInline, GetIsTheIdentity) {
  PaykanShared *s = counted();
  EXPECT_EQ(PaykanShared_get_inline(s), PaykanShared_get(s));
  EXPECT_EQ(PaykanShared_get_inline(s), s);
  Paykan_release(s);
}
