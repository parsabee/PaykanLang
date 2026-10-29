// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Tests for how the runtime `equals` implementations take `other`.
//
// The vtable `equals` slot receives its `other` argument as a PaykanShared
// box, with an ownership mask whose bit 0 says whether the call releases it
// (see src/Runtime/RuntimeInternal.h).  That box can be NULL — e.g. an absent
// optional, which is a null box — and every implementation must treat a NULL
// box as "equal to nothing" (identity: NULL equals nothing) and return 0
// instead of dereferencing it.  A box handed over (bit 0 set) is released;
// one lent (bit 0 clear) is left as it was.

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

#include "VTableTestHelper.h"

namespace {

using EqualsFn = int64_t (*)(PaykanObject *, PaykanShared *, uint64_t);

// Call `fn` with a NULL `other` box, handed over.  Paykan_release(NULL) is a
// no-op, so nothing needs offsetting here.
int64_t equalsWithNullOther(EqualsFn fn, PaykanObject *self) {
  return fn(self, nullptr, 1);
}

// `other`, boxed and handed to `fn` with @p mask: the strong count it is
// left with (it starts at 2: the test's reference and the box's).
int32_t countAfterEquals(EqualsFn fn, PaykanObject *self, PaykanObject *other,
                         uint64_t mask) {
  PaykanShared *box = PaykanShared_new(other);
  Paykan_retain(box); // the test's own reference
  fn(self, box, mask);
  int32_t count = other->strong;
  // Back to what it was: no reference, the test disposes of it.
  other->strong = 0;
  return count;
}

} // namespace

TEST(EqualsNullOther, Object) {
  PaykanObject *obj = PaykanObject_new();
  EXPECT_EQ(equalsWithNullOther(PaykanObject_equals, obj), 0);
  Paykan_dispose(obj);
}

TEST(EqualsNullOther, NoneSingleton) {
  // None's equals goes through its own static vtable implementation.
  PaykanObject *none = &PaykanObject_None;
  EXPECT_EQ(equalsWithNullOther(vtEquals(none), none), 0);
}

TEST(EqualsNullOther, String) {
  PaykanString *s = PaykanString_new("hello", 5);
  EXPECT_EQ(equalsWithNullOther(PaykanString_equals, (PaykanObject *)s), 0);
  Paykan_dispose((PaykanObject *)s);
}

TEST(EqualsNullOther, File) {
  PaykanFile *f = PaykanFile_new();
  EXPECT_EQ(equalsWithNullOther(PaykanFile_equals, (PaykanObject *)f), 0);
  Paykan_dispose((PaykanObject *)f);
}

// Bit 0 of the ownership mask: set, `other` is handed over and released;
// clear, it is lent and survives the call untouched.
TEST(EqualsOwnership, ObjectReleasesOnlyWhatIsHandedOver) {
  PaykanObject *a = PaykanObject_new();
  PaykanObject *b = PaykanObject_new();
  EXPECT_EQ(countAfterEquals(PaykanObject_equals, a, b, 0), 2);
  EXPECT_EQ(countAfterEquals(PaykanObject_equals, a, b, 1), 1);
  Paykan_dispose(a);
  Paykan_dispose(b);
}

TEST(EqualsOwnership, StringReleasesOnlyWhatIsHandedOver) {
  PaykanString *a = PaykanString_new("hello", 5);
  PaykanString *b = PaykanString_new("hello", 5);
  EXPECT_EQ(countAfterEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b, 0),
            2);
  EXPECT_EQ(countAfterEquals(PaykanString_equals, (PaykanObject *)a,
                             (PaykanObject *)b, 1),
            1);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

TEST(EqualsOwnership, FileReleasesOnlyWhatIsHandedOver) {
  PaykanFile *a = PaykanFile_new();
  PaykanFile *b = PaykanFile_new();
  EXPECT_EQ(countAfterEquals(PaykanFile_equals, (PaykanObject *)a,
                             (PaykanObject *)b, 0),
            2);
  EXPECT_EQ(countAfterEquals(PaykanFile_equals, (PaykanObject *)a,
                             (PaykanObject *)b, 1),
            1);
  Paykan_dispose((PaykanObject *)a);
  Paykan_dispose((PaykanObject *)b);
}

// The other bits are about other box parameters: equals has one.
TEST(EqualsOwnership, OnlyBitZeroCounts) {
  PaykanObject *a = PaykanObject_new();
  PaykanObject *b = PaykanObject_new();
  EXPECT_EQ(countAfterEquals(PaykanObject_equals, a, b, ~uint64_t{1}), 2);
  EXPECT_EQ(countAfterEquals(PaykanObject_equals, a, b, UINT64_MAX), 1);
  Paykan_dispose(a);
  Paykan_dispose(b);
}
