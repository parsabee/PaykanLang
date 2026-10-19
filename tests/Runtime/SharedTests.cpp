// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for the strong references (PaykanShared: the object itself,
// counted in its header).

#include <gtest/gtest.h>

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

// -- Paykan_retain

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
