// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanType, the description of a type a generic native
// function is passed: copying and dropping a value through it retains and
// releases exactly the references it says the value holds.

#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

namespace {

// A 24-byte value: an int, then a reference that may be NULL, then one that
// may not.
struct Pair {
  int64_t n;
  PaykanShared *maybe;
  PaykanShared *ref;
};

struct PairType {
  PaykanType head;
  int64_t refs[2];
};

const int64_t kMaybe = (int64_t)offsetof(Pair, maybe) << 1 | 1;
const int64_t kRef = (int64_t)offsetof(Pair, ref) << 1;
const PairType kPair = {
    {sizeof(Pair), alignof(Pair), PAYKAN_KIND_OBJECT, 2},
    {kMaybe, kRef},
};

PaykanShared *boxStr(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

} // namespace

TEST(PaykanType, CopyRetainsAndDropReleasesItsReferences) {
  Pair a = {7, nullptr, boxStr("x")};
  Pair b;
  PaykanType_copy(&kPair.head, &b, &a);
  EXPECT_EQ(b.n, 7);
  EXPECT_EQ(b.maybe, nullptr);
  EXPECT_EQ(b.ref, a.ref);
  EXPECT_EQ(a.ref->refCount, 2);
  PaykanType_drop(&kPair.head, &b);
  EXPECT_EQ(a.ref->refCount, 1);
  PaykanType_drop(&kPair.head, &a);
}

TEST(PaykanType, UnsetIsANullReferenceThatMayNotBe) {
  Pair zero = {};
  EXPECT_TRUE(PaykanType_isUnset(&kPair.head, &zero));
  Pair set = {0, nullptr, boxStr("x")};
  EXPECT_FALSE(PaykanType_isUnset(&kPair.head, &set));
  PaykanType_drop(&kPair.head, &set);

  const PaykanType scalar = {8, 8, PAYKAN_KIND_INT, 0};
  int64_t n = 0;
  EXPECT_FALSE(PaykanType_isUnset(&scalar, &n));
}
