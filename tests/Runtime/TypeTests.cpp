// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PaykanType, the description of a type a generic native
// function is passed: copying and dropping a value through it retains and
// releases exactly the references it says the value holds, and storage
// never set is told from a value by the words it names.

#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

namespace {

// A 24-byte value: an int, then a reference that may be NULL, then one that
// may not, which tells storage never set.
struct Pair {
  int64_t n;
  PaykanShared *maybe;
  PaykanShared *ref;
};

struct PairType {
  PaykanType head;
  int64_t refs[2];
  int64_t unset[1];
};

const int64_t kMaybe = (int64_t)offsetof(Pair, maybe) << 1 | 1;
const int64_t kRef = (int64_t)offsetof(Pair, ref) << 1;
const PairType kPair = {
    {sizeof(Pair), alignof(Pair), PAYKAN_KIND_OBJECT, 2, 1},
    {kMaybe, kRef},
    {(int64_t)offsetof(Pair, ref)},
};

// `enum F { B(s: Str, f: () -> int), A }` as the compiler lays it out: the
// tag, then B's fields.  Zeros are B with a NULL Str and a zero code word, no
// value; this descriptor names the tag, the Str and the code word as the
// words that tell (the tag and either field would do).  A (tag 1) has them 0
// but its tag.
struct Union {
  int64_t tag;
  PaykanShared *s;
  int64_t code;
  PaykanShared *closure;
};

struct UnionType {
  PaykanType head;
  int64_t refs[2];
  int64_t unset[3];
};

const UnionType kUnion = {
    {sizeof(Union), alignof(Union), PAYKAN_KIND_TUPLE, 2, 3},
    {(int64_t)offsetof(Union, s) << 1 | 1,
     (int64_t)offsetof(Union, closure) << 1 | 1},
    {(int64_t)offsetof(Union, tag), (int64_t)offsetof(Union, s),
     (int64_t)offsetof(Union, code)},
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
  EXPECT_EQ(a.ref->strong, 2);
  PaykanType_drop(&kPair.head, &b);
  EXPECT_EQ(a.ref->strong, 1);
  PaykanType_drop(&kPair.head, &a);
}

TEST(PaykanType, UnsetIsANullReferenceThatMayNotBe) {
  Pair zero = {};
  EXPECT_TRUE(PaykanType_isUnset(&kPair.head, &zero));
  Pair set = {0, nullptr, boxStr("x")};
  EXPECT_FALSE(PaykanType_isUnset(&kPair.head, &set));
  PaykanType_drop(&kPair.head, &set);

  const PaykanType scalar = {8, 8, PAYKAN_KIND_INT, 0, 0};
  int64_t n = 0;
  EXPECT_FALSE(PaykanType_isUnset(&scalar, &n));
}

// The words that tell storage never set follow the references.
TEST(PaykanType, UnsetWordsFollowTheReferences) {
  EXPECT_EQ(PaykanType_ref(&kUnion.head, 1),
            (int64_t)offsetof(Union, closure) << 1 | 1);
  EXPECT_EQ(PaykanType_unset(&kUnion.head, 0), 0);
  EXPECT_EQ(PaykanType_unset(&kUnion.head, 1), (int64_t)offsetof(Union, s));
  EXPECT_EQ(PaykanType_unset(&kUnion.head, 2), (int64_t)offsetof(Union, code));
}

// Storage never set is all zeros in every word that tells: one nonzero word
// is a value, whichever it is.
TEST(PaykanType, UnsetIsEveryWordThatTellsZero) {
  Union zero = {};
  EXPECT_TRUE(PaykanType_isUnset(&kUnion.head, &zero));
  Union other = {1, nullptr, 0, nullptr}; // A
  EXPECT_FALSE(PaykanType_isUnset(&kUnion.head, &other));
  Union named = {0, boxStr("x"), 0, nullptr}; // B with a NULL code word
  EXPECT_FALSE(PaykanType_isUnset(&kUnion.head, &named));
  PaykanType_drop(&kUnion.head, &named);
  Union code = {0, nullptr, 0x1000, nullptr}; // B with a NULL Str
  EXPECT_FALSE(PaykanType_isUnset(&kUnion.head, &code));
}

// A type that names no such word has zeros as one of its values, whatever
// its references: `enum E { A(n: int), B(s: Str) }`'s A(0), an optional's
// None.
TEST(PaykanType, ZerosAreAValueOfATypeThatNamesNoWord) {
  struct Enum {
    int64_t tag;
    int64_t n;
    PaykanShared *s;
  };
  struct EnumType {
    PaykanType head;
    int64_t refs[1];
  };
  const EnumType kEnum = {
      {sizeof(Enum), alignof(Enum), PAYKAN_KIND_TUPLE, 1, 0},
      {(int64_t)offsetof(Enum, s) << 1 | 1}};
  Enum zero = {};
  EXPECT_FALSE(PaykanType_isUnset(&kEnum.head, &zero));
}
