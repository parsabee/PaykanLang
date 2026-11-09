// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the freeze (Paykan_freeze and Paykan_freeze_mem,
// src/Runtime/Object.c), what a unique value goes through on its way into an
// Unit tests for the freeze (Paykan_freeze and Paykan_freeze_mem,
// src/Runtime/Object.c), what a unique value goes through on its way into an
// `atomic shared` slot: it and each object it uniquely holds become counted
// atomically (PAYKAN_REF_ATOMIC + 1), by the object's walk slot (`__walk`,
// which the lowering generates per class; PaykanBuffer_walk for a buffer).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

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

// Types as BufferTests.cpp lays them out: a reference's word, then (when it
// may not be NULL) the reference again, as the word that tells storage
// never set.  Not const: the PaykanBuffer_* functions take a `void *`.
struct RefType {
  PaykanType head;
  int64_t ref;
  int64_t unset;
};
PaykanType kInt = {8, 8, PAYKAN_KIND_INT, 0, 0};
RefType kStr = {{8, 8, PAYKAN_KIND_OBJECT, 1, 1}, 0, 0};
RefType kOpt = {{8, 8, PAYKAN_KIND_OPTIONAL, 1, 0}, 1, 0}; // may be NULL

// A 24-byte value with a reference in the middle, as a struct would be.
struct Wide {
  int64_t n;
  PaykanShared *ref;
  bool flag;
};
RefType kWide = {{sizeof(Wide), alignof(Wide), PAYKAN_KIND_OBJECT, 1, 1},
                 (int64_t)offsetof(Wide, ref) << 2,
                 (int64_t)offsetof(Wide, ref)};

// `(Str, Str?)`: two references, the second may be NULL; the first tells
// storage never set.
struct Pair {
  PaykanShared *a;
  PaykanShared *b;
};
struct PairType {
  PaykanType head;
  int64_t refs[2];
  int64_t unset;
};
PairType kPair = {
    {sizeof(Pair), alignof(Pair), PAYKAN_KIND_TUPLE, 2, 1},
    {(int64_t)offsetof(Pair, a) << 2, (int64_t)offsetof(Pair, b) << 2 | 1},
    (int64_t)offsetof(Pair, a)};

// A class holding two references (the second optional) and a pair held in
// memory, with the walk the lowering generates for one (emitWalk,
// src/Lowering/LoweringClass.cpp) and the destroy.  Its walks are counted
// in gWalks.
struct Holder {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
  PaykanShared *first;
  PaykanShared *second; // may be NULL
  Pair pair;
};

int gWalks = 0;

void Holder_destroy(PaykanObject *self) {
  auto *h = reinterpret_cast<Holder *>(self);
  Paykan_release(h->first);
  Paykan_release(h->second);
  PaykanType_drop(&kPair.head, &h->pair);
}

void Holder_walk(PaykanObject *self) {
  ++gWalks;
  auto *h = reinterpret_cast<Holder *>(self);
  Paykan_walk_child(PaykanShared_get(h->first), 1);
  Paykan_walk_child(PaykanShared_get(h->second), 1);
  Paykan_freeze_mem(&kPair.head, &h->pair);
}

static_assert(PAYKAN_SLOT_DESTROY == 0 && PAYKAN_SLOT_TO_STRING == 1 &&
                  PAYKAN_SLOT_EQUALS == 2 && PAYKAN_SLOT_WALK == 3 &&
                  PAYKAN_OBJECT_SLOTS == 4,
              "the vtable below lists the slots in order");
PaykanMethod Holder_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Holder_destroy),
    reinterpret_cast<PaykanMethod>(&PaykanObject_toString),
    reinterpret_cast<PaykanMethod>(&PaykanObject_equals),
    reinterpret_cast<PaykanMethod>(&Holder_walk),
};

/// A holder of the boxes given (which become its), boxed (strong 1).
PaykanShared *holder(PaykanShared *first, PaykanShared *second, Pair pair) {
  auto *h = static_cast<Holder *>(Paykan_malloc(sizeof(Holder)));
  std::memset(h, 0, sizeof *h);
  h->vtable = Holder_vtable;
  h->first = first;
  h->second = second;
  h->pair = pair;
  return PaykanShared_new(reinterpret_cast<PaykanObject *>(h));
}

Holder *asHolder(PaykanShared *h) {
  return reinterpret_cast<Holder *>(PaykanShared_get(h));
}

PaykanShared *str(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

const char *text(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

PaykanObject *obj(PaykanShared *box) { return PaykanShared_get(box); }

PaykanShared *boxAt(RefType &t, PaykanShared *buf, int64_t i) {
  PaykanShared *v;
  memcpy(&v, PaykanBuffer_get(&t.head, obj(buf), i), sizeof(PaykanShared *));
  return v;
}

using PaykanWalkFn = void (*)(PaykanObject *);
PaykanWalkFn vtWalk(const void *o) {
  return vtSlot<PaykanWalkFn>(o, PAYKAN_SLOT_WALK);
}

} // namespace

// -- Paykan_freeze

TEST(Freeze, MakesARootCountedAtomicallyAndFreesItAtTheLastRelease) {
  TrackingHeap heap;
  PaykanShared *s = str("a");
  Paykan_freeze(obj(s));
  EXPECT_EQ(s->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(s->weak, 0);
  // A retain, by the function or inline, counts one more.
  Paykan_retain(s);
  Paykan_retain_inline(s);
  Paykan_retain_nonnull_inline(s);
  EXPECT_EQ(s->strong, PAYKAN_REF_ATOMIC + 4);
  EXPECT_EQ(PaykanShared_new(obj(s)), s);
  EXPECT_EQ(s->strong, PAYKAN_REF_ATOMIC + 5);
  // A release counts one less, and only the last frees it.
  Paykan_release(s);
  Paykan_release_inline(s);
  Paykan_release_nonnull_inline(s);
  Paykan_release(s);
  EXPECT_EQ(s->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(Paykan_heap_live_blocks(), 2); // the string and its data
  EXPECT_STREQ(text(s), "a");
  Paykan_release(s);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(Freeze, NullIsNothingToFreeze) {
  // An absent optional is a NULL box.
  Paykan_freeze(nullptr);
}

TEST(FreezeDeathTest, AValueSomethingElseHoldsStopsTheProgram) {
  PaykanShared *s = str("held");
  Paykan_retain(s);
  EXPECT_DEATH(Paykan_freeze(obj(s)),
               "a value made 'atomic shared' is still borrowed");
  Paykan_release(s);
  PaykanShared *a = str("a");
  Paykan_retain(a);
  PaykanShared *h = holder(a, nullptr, {str("p"), nullptr});
  EXPECT_DEATH(Paykan_freeze(obj(h)),
               "a value made 'atomic shared' is still borrowed");
  Paykan_release(h);
  Paykan_release(a);
  PaykanShared *w = str("w");
  PaykanWeak *weak = Paykan_weak_new(w);
  EXPECT_DEATH(Paykan_freeze(obj(w)),
               "a value made 'atomic shared' is still borrowed");
  Paykan_weak_release(weak);
  Paykan_release(w);
}

TEST(Freeze, WalksWhatTheObjectHoldsThroughTheVTableSlot) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *a = str("a");
  PaykanShared *b = str("b");
  PaykanShared *c = str("c");
  PaykanShared *h = holder(a, nullptr, {b, c});
  Paykan_freeze(obj(h));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(h->strong, PAYKAN_REF_ATOMIC + 1);
  // Its walk froze the reference fields (the NULL one is nothing) and the
  // references in the pair.
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(b->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(c->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(asHolder(h)->second, nullptr);
  // The walk is the slot's, this class's own.
  EXPECT_TRUE(vtWalk(obj(h)) == &Holder_walk);
  // What it holds is counted too, so a hold of it outlives the root.
  Paykan_retain(a);
  Paykan_retain_inline(a);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 3);
  Paykan_release(a);
  Paykan_release_inline(a);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  // Freezing a frozen value is a no-op: no second walk, nothing counted.
  Paykan_freeze(obj(h));
  Paykan_freeze(obj(a));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(h->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(h); // its destroy releases a, b and c
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(Freeze, AHoldOfAnObjectInsideOutlivesTheRoot) {
  TrackingHeap heap;
  PaykanShared *inner = str("inner");
  PaykanShared *root = holder(inner, nullptr, {nullptr, nullptr});
  Paykan_freeze(obj(root));
  Paykan_retain(inner);
  Paykan_release(root);
  EXPECT_STREQ(text(inner), "inner");
  EXPECT_EQ(inner->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(inner);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// Past the walker's inline stack, and deep enough to overflow a recursive
// walk's C stack.
TEST(Freeze, AChainOfObjectsIsWalkedAndFreedWithoutRecursion) {
  TrackingHeap heap;
  PaykanShared *head = holder(str("leaf"), nullptr, {nullptr, nullptr});
  for (int i = 0; i < 100000; ++i)
    head = holder(head, nullptr, {nullptr, nullptr});
  Paykan_freeze(obj(head));
  EXPECT_EQ(asHolder(head)->first->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(head);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// -- A buffer: PaykanBuffer_walk

TEST(Freeze, ABufferFreezesTheReferencesItsValuesHold) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 4);
  PaykanShared *a = str("a");
  PaykanShared *b = str("b");
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  PaykanBuffer_set(&kStr.head, &buf, 1, &b);
  Paykan_release(a); // the buffer's references are the only ones
  Paykan_release(b);
  // Value 2 was never set: it holds no reference, and the walk skips it.
  EXPECT_EQ(PaykanBuffer_isSet(&kStr.head, obj(buf), 2), 0);
  EXPECT_TRUE(vtWalk(obj(buf)) != &PaykanObject_walk);
  Paykan_freeze(obj(buf));
  EXPECT_EQ(buf->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(b->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(boxAt(kStr, buf, 0), a);
  EXPECT_EQ(PaykanBuffer_isSet(&kStr.head, obj(buf), 2), 0);
  // A copy of a frozen buffer's value counts it, atomically.
  PaykanShared *got = nullptr;
  PaykanBuffer_copy(&kStr.head, &got, obj(buf), 0);
  EXPECT_EQ(got, a);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 2);
  Paykan_release(got);
  Paykan_release(buf); // its destroy releases a and b
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST(Freeze, ABufferOfNumbersOrOfNoneHasNothingToFreeze) {
  TrackingHeap heap;
  PaykanShared *ints = PaykanBuffer_new(&kInt, 3);
  int64_t v = 7;
  PaykanBuffer_set(&kInt, &ints, 1, &v);
  Paykan_freeze(obj(ints));
  EXPECT_EQ(ints->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(*(const int64_t *)PaykanBuffer_get(&kInt, obj(ints), 1), 7);
  // An optional that is None is a NULL reference: nothing.
  PaykanShared *opts = PaykanBuffer_new(&kOpt.head, 2);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kOpt.head, &opts, 1, &a);
  Paykan_release(a);
  Paykan_freeze(obj(opts));
  EXPECT_EQ(boxAt(kOpt, opts, 0), nullptr);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(ints);
  Paykan_release(opts);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// A value is as wide as its type says, and the walk finds the reference in
// each where the type says it is.
TEST(Freeze, ABufferOfWideValuesFreezesEachReference) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kWide.head, 2);
  Wide w = {7, str("w"), true};
  Wide x = {8, str("x"), false};
  PaykanBuffer_set(&kWide.head, &buf, 0, &w);
  PaykanBuffer_set(&kWide.head, &buf, 1, &x);
  Paykan_release(w.ref);
  Paykan_release(x.ref);
  Paykan_freeze(obj(buf));
  EXPECT_EQ(w.ref->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(x.ref->strong, PAYKAN_REF_ATOMIC + 1);
  Wide got = {};
  PaykanBuffer_copy(&kWide.head, &got, obj(buf), 1);
  EXPECT_EQ(got.n, 8);
  EXPECT_EQ(got.ref, x.ref);
  EXPECT_FALSE(got.flag);
  Paykan_release(got.ref);
  Paykan_release(buf);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// An object holding a buffer (a `mem::Box<mem::Buffer<Str>>`, what
// samples/concurrency/README.md freezes for a parallel body) is walked in
// turn, through each object's own slot.
TEST(Freeze, AnObjectHoldingABufferFreezesItsValuesToo) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 2);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  Paykan_release(a);
  PaykanShared *box = holder(buf, nullptr, {str("p"), nullptr});
  Paykan_freeze(obj(box));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(box->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(buf->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(asHolder(box)->pair.a->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(box);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// -- Paykan_freeze_mem: the references in a value, by its type

// Outside a walk, each reference in the value is a root.
TEST(FreezeMem, FollowsTheTypeDescriptor) {
  TrackingHeap heap;
  // A reference in the middle of a struct, found by its offset.
  Wide w = {7, str("w"), true};
  Paykan_freeze_mem(&kWide.head, &w);
  EXPECT_EQ(w.ref->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(w.n, 7);
  EXPECT_TRUE(w.flag);
  // Every reference of a value; a NULL one is nothing.
  Pair p = {str("a"), nullptr};
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(p.a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(p.b, nullptr);
  // A reference alone, and one that is None.
  PaykanShared *s = str("s");
  Paykan_freeze_mem(&kStr.head, &s);
  EXPECT_EQ(s->strong, PAYKAN_REF_ATOMIC + 1);
  PaykanShared *none = nullptr;
  Paykan_freeze_mem(&kOpt.head, &none);
  EXPECT_EQ(none, nullptr);
  // A number holds no reference: nothing to freeze.
  int64_t n = 5;
  Paykan_freeze_mem(&kInt, &n);
  EXPECT_EQ(n, 5);
  Paykan_release(w.ref);
  PaykanType_drop(&kPair.head, &p);
  Paykan_release(s);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// What a reference in the value names is frozen with what it holds.
TEST(FreezeMem, FreezesWhatTheReferencesHold) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *a = str("a");
  Pair p = {holder(a, nullptr, {str("p"), nullptr}), nullptr};
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(p.a->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(a->strong, PAYKAN_REF_ATOMIC + 1);
  // Again: the holder is frozen already, so nothing is walked.
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(gWalks, 1);
  PaykanType_drop(&kPair.head, &p);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// -- The cells

// A walk reaches a handle like any reference, and a cell is counted
// atomically, so the freeze stops there: an `atomic` field of a frozen
// object still changes, and a `locked` one keeps counting its contents.
TEST(Freeze, StopsAtAnAtomicOrLockedCell) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *hits = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  PaykanShared *payload = str("payload");
  PaykanShared *counts = PaykanLocked_new(payload);
  PaykanShared *cfg = holder(hits, counts, {str("name"), nullptr});
  Paykan_freeze(obj(cfg));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(cfg->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(hits->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(counts->strong, PAYKAN_REF_ATOMIC + 1);
  EXPECT_EQ(asHolder(cfg)->pair.a->strong, PAYKAN_REF_ATOMIC + 1);
  // The atomic field still changes.
  EXPECT_EQ(PaykanAtomic_add(PaykanShared_get(hits), 1), 0);
  EXPECT_EQ(PaykanAtomic_load(PaykanShared_get(hits)), 1);
  // The locked cell's contents were not reached: still counted.
  EXPECT_EQ(payload->strong, 1);
  // Freezing a cell directly is a no-op too.
  Paykan_freeze(obj(hits));
  Paykan_freeze(obj(counts));
  EXPECT_EQ(payload->strong, 1);
  Paykan_release(cfg); // its destroy releases the handles
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

// -- The check of a moved value (#368)

namespace {

// As the lowering's `__walk`: a unique field, then a `shared` one.
struct Link {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
  PaykanShared *unique;
  PaykanShared *shared;
};

void Link_destroy(PaykanObject *self) {
  auto *l = reinterpret_cast<Link *>(self);
  Paykan_release(l->unique);
  Paykan_release(l->shared);
}

void Link_walk(PaykanObject *self) {
  auto *l = reinterpret_cast<Link *>(self);
  Paykan_walk_child(PaykanShared_get(l->unique), 1);
  Paykan_walk_child(PaykanShared_get(l->shared), 0);
}

PaykanMethod Link_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Link_destroy),
    reinterpret_cast<PaykanMethod>(&PaykanObject_toString),
    reinterpret_cast<PaykanMethod>(&PaykanObject_equals),
    reinterpret_cast<PaykanMethod>(&Link_walk),
};

PaykanShared *link(PaykanShared *unique, PaykanShared *shared) {
  auto *l = static_cast<Link *>(Paykan_malloc(sizeof(Link)));
  std::memset(l, 0, sizeof *l);
  l->vtable = Link_vtable;
  l->unique = unique;
  l->shared = shared;
  return PaykanShared_new(reinterpret_cast<PaykanObject *>(l));
}

// `(Node, shared Node)`: the second word has the bit for a reference that
// is no unique one.
struct TwoRefs {
  PaykanType head;
  int64_t unique;
  int64_t shared;
};
TwoRefs kTwo = {{sizeof(Pair), alignof(Pair), PAYKAN_KIND_OBJECT, 2, 0},
                (int64_t)offsetof(Pair, a) << 2,
                (int64_t)offsetof(Pair, b) << 2 | 2};

} // namespace

TEST(MovedUnique, NothingElseHoldsAChainOfUniqueObjects) {
  TrackingHeap heap;
  PaykanShared *v = link(link(str("leaf"), nullptr), nullptr);
  EXPECT_EQ(Paykan_moved_unique(v), 1);
  Paykan_release(v);
  EXPECT_EQ(Paykan_moved_unique(nullptr), 1);
}

TEST(MovedUnique, AHoldOnTheValueOrAUniquePartIsFound) {
  TrackingHeap heap;
  PaykanShared *leaf = str("leaf");
  PaykanShared *v = link(link(leaf, nullptr), nullptr);
  Paykan_retain(v);
  EXPECT_EQ(Paykan_moved_unique(v), 0);
  Paykan_release(v);
  Paykan_retain(leaf); // two levels down
  EXPECT_EQ(Paykan_moved_unique(v), 0);
  Paykan_release(leaf);
  EXPECT_EQ(Paykan_moved_unique(v), 1);
  Paykan_release(v);
}

TEST(MovedUnique, StopsAtASharedLinkAndSkipsAnImmortal) {
  TrackingHeap heap;
  PaykanShared *other = str("other");
  Paykan_retain(other); // a `shared` link's second reference
  PaykanShared *frozen = str("frozen");
  Paykan_freeze(obj(frozen));
  PaykanShared *v = link(frozen, other);
  EXPECT_EQ(Paykan_moved_unique(v), 1);
  Paykan_release(v);
  Paykan_release(other);
}

TEST(MovedUnique, ReadsTheDescriptorOfAValueHeldInMemory) {
  TrackingHeap heap;
  Pair p = {str("unique"), str("shared")};
  Paykan_retain(p.b); // the bit stops it: no matter
  EXPECT_EQ(Paykan_moved_unique_mem(&kTwo.head, &p), 1);
  Paykan_retain(p.a);
  EXPECT_EQ(Paykan_moved_unique_mem(&kTwo.head, &p), 0);
  Paykan_release(p.a);
  Paykan_release(p.b);
  Paykan_release(p.a);
  Paykan_release(p.b);
}

TEST(MovedUnique, AFreezeAfterACheckStillFreezes) {
  TrackingHeap heap;
  PaykanShared *inner = str("inner");
  PaykanShared *v = link(inner, nullptr);
  Paykan_retain(v);
  EXPECT_EQ(Paykan_moved_unique(v), 0); // stopped early, mode reset
  Paykan_release(v);
  Paykan_freeze(obj(v));
  EXPECT_EQ(inner->strong, PAYKAN_REF_ATOMIC + 1);
  Paykan_release(v);
}
