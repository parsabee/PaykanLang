// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the PaykanBuffer_* functions, the storage of the standard
// library's memory::Buffer<T>: values of any PaykanType, new values all zero
// bytes, references retained by the buffer and released when they leave
// it, and misuse panics.  Each test runs on the tracking allocator and ends
// with no live blocks.

#include "Names.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>

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

// A type holding one reference: its word, then (when it may not be NULL)
// the reference again, as the word that tells storage never set.
struct RefType {
  PaykanType head;
  int64_t ref;
  int64_t unset;
};

// Not const: the PaykanBuffer_* functions take a PaykanType as a `void *`.
PaykanType kInt = {8, 8, PAYKAN_KIND_INT, 0, 0};
PaykanType kFloat = {8, 8, PAYKAN_KIND_FLOAT, 0, 0};
RefType kStr = {{8, 8, PAYKAN_KIND_OBJECT, 1, 1}, 0, 0};
RefType kOpt = {{8, 8, PAYKAN_KIND_OPTIONAL, 1, 0}, 1, 0}; // may be NULL

// A 24-byte value with a reference in the middle, as a struct would be.
struct Wide {
  int64_t n;
  PaykanShared *ref;
  bool flag;
};
RefType kWide = {{sizeof(Wide), alignof(Wide), PAYKAN_KIND_OBJECT, 1, 1},
                 (int64_t)offsetof(Wide, ref) << 1,
                 (int64_t)offsetof(Wide, ref)};

// `enum F { B(s: Str), A }` as the compiler lays it out: the tag, then B's
// Str, NULL in an A.  Zeros are B with a NULL Str, no value: the tag and the
// Str tell storage never set.
struct Union {
  int64_t tag;
  PaykanShared *s;
};
struct UnionType {
  PaykanType head;
  int64_t ref;
  int64_t unset[2];
};
UnionType kUnion = {
    {sizeof(Union), alignof(Union), PAYKAN_KIND_TUPLE, 1, 2},
    (int64_t)offsetof(Union, s) << 1 | 1,
    {(int64_t)offsetof(Union, tag), (int64_t)offsetof(Union, s)}};

// A function value, `() -> int`: its code word, never 0 in a value, then
// its closure, NULL when it captures nothing.
struct Function {
  int64_t code;
  PaykanShared *closure;
};
RefType kFunction = {
    {sizeof(Function), alignof(Function), PAYKAN_KIND_TUPLE, 1, 1},
    (int64_t)offsetof(Function, closure) << 1 | 1,
    (int64_t)offsetof(Function, code)};

// `enum G { A(n: int, s: Str?), B(s: Str) }`: zeros are A(0, None), a
// value, so no word tells.
struct Valued {
  int64_t tag;
  int64_t n;
  PaykanShared *a;
  PaykanShared *b;
};
struct ValuedType {
  PaykanType head;
  int64_t refs[2];
};
ValuedType kValued = {
    {sizeof(Valued), alignof(Valued), PAYKAN_KIND_TUPLE, 2, 0},
    {(int64_t)offsetof(Valued, a) << 1 | 1,
     (int64_t)offsetof(Valued, b) << 1 | 1}};

PaykanObject *obj(PaykanShared *buf) { return PaykanShared_get(buf); }

int64_t intAt(PaykanShared *buf, int64_t i) {
  int64_t v;
  memcpy(&v, PaykanBuffer_get(&kInt, obj(buf), i), sizeof v);
  return v;
}

void setInt(PaykanShared **buf, int64_t i, int64_t v) {
  PaykanBuffer_set(&kInt, buf, i, &v);
}

PaykanShared *boxAt(RefType &t, PaykanShared *buf, int64_t i) {
  PaykanShared *v;
  memcpy(&v, PaykanBuffer_get(&t.head, obj(buf), i), sizeof(PaykanShared *));
  return v;
}

PaykanShared *str(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

std::string text(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

int64_t cap(PaykanShared *buf) { return ((PaykanBuffer *)obj(buf))->cap; }

} // namespace

TEST(Buffer, NewHasZeroedValues) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kInt, 3);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 3);
  for (int64_t i = 0; i < 3; ++i)
    EXPECT_EQ(intAt(buf, i), 0) << i;
  EXPECT_EQ((void *)obj(buf)->vtable, (void *)&PaykanBuffer_vtable);
  Paykan_release(buf);
}

TEST(Buffer, ValuesRoundTrip) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kFloat, 2);
  double d = 2.5;
  PaykanBuffer_set(&kFloat, &buf, 1, &d);
  double got = 0;
  PaykanBuffer_copy(&kFloat, &got, obj(buf), 1);
  EXPECT_EQ(got, 2.5);
  EXPECT_EQ(*(const double *)PaykanBuffer_get(&kFloat, obj(buf), 0), 0.0);
  Paykan_release(buf);
}

// The buffer holds its own reference to what is stored; get lends it, copy
// gives the caller one.
TEST(Buffer, ReferencesAreRetainedAndReleased) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 2);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  EXPECT_EQ(a->strong, 2);
  EXPECT_EQ(boxAt(kStr, buf, 0), a);
  EXPECT_EQ(a->strong, 2);
  PaykanShared *got = nullptr;
  PaykanBuffer_copy(&kStr.head, &got, obj(buf), 0);
  EXPECT_EQ(text(got), "a");
  EXPECT_EQ(a->strong, 3);
  Paykan_release(got);
  // Overwriting releases the old value; storing it again is safe.
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  EXPECT_EQ(a->strong, 2);
  PaykanShared *b = str("b");
  PaykanBuffer_set(&kStr.head, &buf, 0, &b);
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
  PaykanBuffer_set(&kStr.head, &buf, 1, &b);
  Paykan_release(b);
  Paykan_release(buf); // releases both of its references to b
}

TEST(Buffer, OptionalValuesStartAsNone) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kOpt.head, 1);
  EXPECT_EQ(boxAt(kOpt, buf, 0), nullptr);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kOpt.head, &buf, 0, &a);
  PaykanShared *none = nullptr;
  PaykanBuffer_set(&kOpt.head, &buf, 0, &none);
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
  Paykan_release(buf);
}

// A value is as wide as its type says, and the buffer finds its references
// where the type says they are.
TEST(Buffer, HoldsValuesOfAnySize) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kWide.head, 2);
  Wide w = {7, str("w"), true};
  PaykanBuffer_set(&kWide.head, &buf, 0, &w);
  PaykanBuffer_set(&kWide.head, &buf, 1, &w);
  EXPECT_EQ(w.ref->strong, 3);
  const char *first = (const char *)PaykanBuffer_get(&kWide.head, obj(buf), 0);
  const char *second = (const char *)PaykanBuffer_get(&kWide.head, obj(buf), 1);
  EXPECT_EQ(second - first, (ptrdiff_t)sizeof(Wide));
  Wide got = {};
  PaykanBuffer_copy(&kWide.head, &got, obj(buf), 1);
  EXPECT_EQ(got.n, 7);
  EXPECT_TRUE(got.flag);
  EXPECT_EQ(w.ref->strong, 4);
  PaykanType_drop(&kWide.head, &got);
  PaykanBuffer_shrink(&kWide.head, &buf, 1);
  EXPECT_EQ(w.ref->strong, 2);
  Paykan_release(w.ref);
  Paykan_release(buf);
}

TEST(Buffer, ReserveGrowAndShrinkChangeTheLength) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kInt, 3);
  setInt(&buf, 2, 7);
  PaykanBuffer_reserve(&kInt, &buf, 2); // already has 2
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 3);
  PaykanBuffer_reserve(&kInt, &buf, 10);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 10);
  PaykanBuffer_grow(&kInt, &buf, 5);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 15);
  EXPECT_EQ(intAt(buf, 2), 7);
  for (int64_t i = 3; i < 15; ++i)
    EXPECT_EQ(intAt(buf, i), 0) << i;
  setInt(&buf, 14, 9);
  PaykanBuffer_shrink(&kInt, &buf, 12);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 3);
  // A value that comes back is new: 0, not what it held before.
  PaykanBuffer_grow(&kInt, &buf, 12);
  EXPECT_EQ(intAt(buf, 14), 0);
  PaykanBuffer_shrink(&kInt, &buf, 15);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 0);
  Paykan_release(buf);
}

TEST(Buffer, ShrinkReleasesTheDroppedReferences) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 3);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kStr.head, &buf, 2, &a);
  PaykanBuffer_shrink(&kStr.head, &buf, 2);
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
  Paykan_release(buf);
}

// Growing one value at a time reallocates O(log n) times; shrinking frees
// the allocation once a quarter of it is in use.
TEST(Buffer, AllocationGrowsGeometricallyAndShrinks) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kInt, 0);
  int64_t before = Paykan_heap_total_reallocs();
  for (int i = 0; i < 1000; ++i)
    PaykanBuffer_grow(&kInt, &buf, 1);
  EXPECT_LE(Paykan_heap_total_reallocs() - before, 12);
  EXPECT_GE(cap(buf), 1000);
  PaykanBuffer_shrink(&kInt, &buf, 999);
  EXPECT_EQ(cap(buf), 8);
  Paykan_release(buf);
}

// 2^61 - 1 ints are 2^64 - 8 bytes, which a size_t holds but not with the
// tracking allocator's header: ENOMEM, as without tracking, and the buffer
// is unchanged, whether it had no allocation (malloc) or one (realloc).
TEST(Buffer, ValuesWithNoRoomForTheTrackingHeaderAreOutOfMemory) {
  TrackingHeap heap;
  const int64_t most = (INT64_C(1) << 61) - 1;
  PaykanShared *buf = PaykanBuffer_new(&kInt, 0);
  EXPECT_EQ(PaykanBuffer_tryReserve(&kInt, &buf, most), ENOMEM);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 0);
  EXPECT_EQ(cap(buf), 0);
  PaykanBuffer_grow(&kInt, &buf, 1);
  setInt(&buf, 0, 7);
  EXPECT_EQ(PaykanBuffer_tryGrow(&kInt, &buf, most - 1), ENOMEM);
  EXPECT_EQ(PaykanBuffer_tryReserve(&kInt, &buf, most), ENOMEM);
  EXPECT_EQ(PaykanBuffer_len(&kInt, obj(buf)), 1);
  EXPECT_EQ(intAt(buf, 0), 7);
  Paykan_release(buf);
}

TEST(BufferDeath, OutOfBoundsPanics) {
  PaykanShared *buf = PaykanBuffer_new(&kInt, 3);
  EXPECT_DEATH(PaykanBuffer_get(&kInt, obj(buf), 3),
               "buffer index 3 out of bounds \\(len=3\\)");
  EXPECT_DEATH(PaykanBuffer_get(&kInt, obj(buf), -1), "out of bounds");
  EXPECT_DEATH(setInt(&buf, 5, 1), "out of bounds");
  Paykan_release(buf);
}

// An index is the standard library's `unsigned`, in an int64_t: one of 2^63
// or more is negative here, and a panic prints it as the program wrote it.
TEST(BufferDeath, AnIndexPastINT64MaxPanicsUnsigned) {
  PaykanShared *buf = PaykanBuffer_new(&kInt, 3);
  EXPECT_DEATH(PaykanBuffer_get(&kInt, obj(buf), -1),
               "buffer index 18446744073709551615 out of bounds \\(len=3\\)");
  EXPECT_DEATH(PaykanBuffer_get(&kInt, obj(buf), INT64_MIN),
               "buffer index 9223372036854775808 out of bounds \\(len=3\\)");
  int64_t out = 0;
  EXPECT_DEATH(PaykanBuffer_copy(&kInt, &out, obj(buf), INT64_MIN),
               "buffer index 9223372036854775808 out of bounds \\(len=3\\)");
  EXPECT_DEATH(setInt(&buf, -2, 1),
               "buffer index 18446744073709551614 out of bounds \\(len=3\\)");
  EXPECT_EQ(PaykanBuffer_isSet(&kInt, obj(buf), INT64_MIN), 0);
  Paykan_release(buf);
}

TEST(BufferDeath, NeverSetReferencePanics) {
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 1);
  EXPECT_DEATH(PaykanBuffer_get(&kStr.head, obj(buf), 0),
               "buffer value 0 was never set");
  PaykanShared *out = nullptr;
  EXPECT_DEATH(PaykanBuffer_copy(&kStr.head, &out, obj(buf), 0),
               "was never set");
  Paykan_release(buf);
}

// A slot of a tagged union whose first variant holds a reference is unset
// until stored: zeros are that variant with a NULL reference.  A value of
// another variant, the reference NULL in it, is set.
TEST(Buffer, ANeverSetTaggedUnionIsUnset) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kUnion.head, 3);
  for (int64_t i = 0; i < 3; ++i)
    EXPECT_EQ(PaykanBuffer_isSet(&kUnion.head, obj(buf), i), 0) << i;
  Union named = {0, str("x")};
  PaykanBuffer_set(&kUnion.head, &buf, 0, &named);
  Union other = {1, nullptr};
  PaykanBuffer_set(&kUnion.head, &buf, 1, &other);
  EXPECT_EQ(PaykanBuffer_isSet(&kUnion.head, obj(buf), 0), 1);
  EXPECT_EQ(PaykanBuffer_isSet(&kUnion.head, obj(buf), 1), 1);
  EXPECT_EQ(PaykanBuffer_isSet(&kUnion.head, obj(buf), 2), 0);
  Union got = {};
  PaykanBuffer_copy(&kUnion.head, &got, obj(buf), 1);
  EXPECT_EQ(got.tag, 1);
  EXPECT_EQ(got.s, nullptr);
  PaykanBuffer_copy(&kUnion.head, &got, obj(buf), 0);
  EXPECT_EQ(got.s, named.s);
  EXPECT_EQ(named.s->strong, 3); // named, the slot and got
  PaykanType_drop(&kUnion.head, &got);
  PaykanType_drop(&kUnion.head, &named);
  Paykan_release(buf);
}

// A function value's slot is unset until stored: its code word is 0.  One
// that captures nothing (a NULL closure) is set.
TEST(Buffer, ANeverSetFunctionValueIsUnset) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kFunction.head, 2);
  EXPECT_EQ(PaykanBuffer_isSet(&kFunction.head, obj(buf), 0), 0);
  Function f = {0x1000, nullptr};
  PaykanBuffer_set(&kFunction.head, &buf, 0, &f);
  EXPECT_EQ(PaykanBuffer_isSet(&kFunction.head, obj(buf), 0), 1);
  EXPECT_EQ(PaykanBuffer_isSet(&kFunction.head, obj(buf), 1), 0);
  Function got = {};
  PaykanBuffer_copy(&kFunction.head, &got, obj(buf), 0);
  EXPECT_EQ(got.code, 0x1000);
  Paykan_release(buf);
}

// A type whose zeros are a value has no slot that is unset: a new one reads
// as that value, even with references that are NULL in it.
TEST(Buffer, ZerosAreAValueOfATypeThatNamesNoWord) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kValued.head, 1);
  EXPECT_EQ(PaykanBuffer_isSet(&kValued.head, obj(buf), 0), 1);
  Valued got = {1, 2, nullptr, nullptr};
  PaykanBuffer_copy(&kValued.head, &got, obj(buf), 0);
  EXPECT_EQ(got.tag, 0);
  EXPECT_EQ(got.n, 0);
  EXPECT_EQ(got.a, nullptr);
  EXPECT_EQ(got.b, nullptr);
  PaykanShared *opts = PaykanBuffer_new(&kOpt.head, 1);
  EXPECT_EQ(PaykanBuffer_isSet(&kOpt.head, obj(opts), 0), 1);
  Paykan_release(opts);
  Paykan_release(buf);
}

TEST(BufferDeath, NeverSetTaggedUnionOrFunctionValuePanics) {
  PaykanShared *buf = PaykanBuffer_new(&kUnion.head, 1);
  EXPECT_DEATH(PaykanBuffer_get(&kUnion.head, obj(buf), 0),
               "buffer value 0 was never set");
  Union out = {};
  EXPECT_DEATH(PaykanBuffer_copy(&kUnion.head, &out, obj(buf), 0),
               "buffer value 0 was never set");
  Paykan_release(buf);
  PaykanShared *fns = PaykanBuffer_new(&kFunction.head, 2);
  EXPECT_DEATH(PaykanBuffer_get(&kFunction.head, obj(fns), 1),
               "buffer value 1 was never set");
  Paykan_release(fns);
}

// A count is an `unsigned` too: one of 2^63 or more (negative here) is more
// values than there is memory for (PaykanBuffer_tryGrow's ENOMEM), and the
// panics print it unsigned.
TEST(BufferDeath, BadCountsPanic) {
  PaykanShared *buf = PaykanBuffer_new(&kInt, 3);
  EXPECT_DEATH(PaykanBuffer_new(&kInt, -1),
               "out of memory growing a buffer to 18446744073709551615 values");
  EXPECT_DEATH(PaykanBuffer_grow(&kInt, &buf, -2),
               "out of memory growing a buffer by 18446744073709551614 values");
  EXPECT_DEATH(PaykanBuffer_reserve(&kInt, &buf, INT64_MIN),
               "out of memory growing a buffer to 9223372036854775808 values");
  EXPECT_DEATH(PaykanBuffer_shrink(&kInt, &buf, 4),
               "buffer shrink by 4: it has only 3 values");
  EXPECT_DEATH(PaykanBuffer_shrink(&kInt, &buf, -1),
               "buffer shrink by 18446744073709551615: it has only 3 values");
  Paykan_release(buf);
}

// What the lowering knows of the Buffer functions (Names.h,
// kPaykanBufferFunctions, read in src/Lowering/LoweringHolds.cpp) is what
// Buffer.c does: the table has every function it defines, none of them
// replaces the buffer an `inout` handle names, and only those the table
// says drop values let go of one.  A read the lowering keeps in place
// across a call of one is only safe while this holds.
TEST(Buffer, TheFunctionsDoWhatTheLoweringKnowsOfThem) {
  std::ifstream source(PAYKAN_RUNTIME_BUFFER_SOURCE);
  ASSERT_TRUE(source) << PAYKAN_RUNTIME_BUFFER_SOURCE;
  const std::string text((std::istreambuf_iterator<char>(source)),
                         std::istreambuf_iterator<char>());
  std::set<std::string> defined;
  const std::regex definition(R"((^|\n)[A-Za-z][^\n]*\b(PaykanBuffer_\w+)\()");
  for (auto it = std::sregex_iterator(text.begin(), text.end(), definition);
       it != std::sregex_iterator(); ++it)
    if ((*it)[0].str().find("static") == std::string::npos)
      defined.insert((*it)[2].str());
  std::set<std::string> known;
  for (const paykan::names::BufferFunction &f :
       paykan::names::kPaykanBufferFunctions)
    known.insert(f.Symbol);
  EXPECT_EQ(defined, known);

  TrackingHeap heap;
  PaykanShared *other = str("b");
  const std::map<std::string, std::function<void(PaykanShared **)>> calls = {
      {paykan::names::kPaykanBufferNew,
       [](PaykanShared **) {
         Paykan_release(PaykanBuffer_new(&kStr.head, 2));
       }},
      {paykan::names::kPaykanBufferLen,
       [](PaykanShared **b) { PaykanBuffer_len(&kStr.head, obj(*b)); }},
      {paykan::names::kPaykanBufferGet,
       [](PaykanShared **b) { PaykanBuffer_get(&kStr.head, obj(*b), 0); }},
      {paykan::names::kPaykanBufferCopy,
       [](PaykanShared **b) {
         PaykanShared *got = nullptr;
         PaykanBuffer_copy(&kStr.head, &got, obj(*b), 0);
         Paykan_release(got);
       }},
      {paykan::names::kPaykanBufferIsSet,
       [](PaykanShared **b) { PaykanBuffer_isSet(&kStr.head, obj(*b), 0); }},
      {paykan::names::kPaykanBufferSet,
       [&](PaykanShared **b) { PaykanBuffer_set(&kStr.head, b, 0, &other); }},
      {paykan::names::kPaykanBufferReserve,
       [](PaykanShared **b) { PaykanBuffer_reserve(&kStr.head, b, 100); }},
      {paykan::names::kPaykanBufferGrow,
       [](PaykanShared **b) { PaykanBuffer_grow(&kStr.head, b, 100); }},
      {paykan::names::kPaykanBufferTryReserve,
       [](PaykanShared **b) { PaykanBuffer_tryReserve(&kStr.head, b, 100); }},
      {paykan::names::kPaykanBufferTryGrow,
       [](PaykanShared **b) { PaykanBuffer_tryGrow(&kStr.head, b, 100); }},
      {paykan::names::kPaykanBufferShrink,
       [](PaykanShared **b) { PaykanBuffer_shrink(&kStr.head, b, 1); }},
  };
  for (const paykan::names::BufferFunction &f :
       paykan::names::kPaykanBufferFunctions) {
    auto call = calls.find(f.Symbol);
    ASSERT_NE(call, calls.end()) << f.Symbol;
    PaykanShared *buf = PaykanBuffer_new(&kStr.head, 1);
    PaykanShared *a = str("a");
    PaykanBuffer_set(&kStr.head, &buf, 0, &a);
    PaykanShared *const handle = buf;
    call->second(&buf);
    EXPECT_EQ(buf, handle) << f.Symbol;
    EXPECT_EQ(a->strong, f.Drops ? 1 : 2) << f.Symbol;
    Paykan_release(buf);
    Paykan_release(a);
  }
  Paykan_release(other);
}
