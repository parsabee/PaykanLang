// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the concurrency prototype's freeze (Paykan_freeze and
// Paykan_freeze_mem, src/Runtime/Object.c), what a unique value goes
// through on its way into an `atomic shared` slot.  A frozen object is
// immortal, its strong count PAYKAN_REF_MAX, which retains and releases
// leave alone; and the freeze walks, through the object's walk slot
// (`__walk`, which the lowering generates per class: Paykan_freeze of each
// reference field but a `weak` one, Paykan_freeze_mem of each field held
// in memory; PaykanBuffer_walk for a buffer), everything the object holds,
// so a whole graph is read by any thread with no count.  The prototype
// never thaws or frees a frozen object, so a test puts each count back to
// what it was before the freeze (thaw, below: the thaw the design
// describes) to end with no live blocks.

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
  Paykan_freeze(PaykanShared_get(h->first));
  Paykan_freeze(PaykanShared_get(h->second));
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

/// The thaw the prototype lacks: @p box, frozen, gets back the count
/// @p strong it had before, so that its last release frees it.
void thaw(PaykanShared *box, int32_t strong) {
  ASSERT_EQ(box->strong, PAYKAN_REF_MAX);
  box->strong = strong;
}

using PaykanWalkFn = void (*)(PaykanObject *);
PaykanWalkFn vtWalk(const void *o) {
  return vtSlot<PaykanWalkFn>(o, PAYKAN_SLOT_WALK);
}

} // namespace

// -- Paykan_freeze

TEST(Freeze, MakesAnObjectImmortal) {
  TrackingHeap heap;
  PaykanShared *s = str("a");
  Paykan_retain(s);
  EXPECT_EQ(s->strong, 2);
  Paykan_freeze(obj(s));
  EXPECT_EQ(s->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(s->weak, 0);
  // A retain or a release, by the function or inline, leaves the count
  // alone, and no release frees the object.
  Paykan_retain(s);
  Paykan_retain_inline(s);
  Paykan_retain_nonnull_inline(s);
  EXPECT_EQ(s->strong, PAYKAN_REF_MAX);
  Paykan_release(s);
  Paykan_release_inline(s);
  Paykan_release_nonnull_inline(s);
  Paykan_dispose(obj(s));
  EXPECT_EQ(PaykanShared_new(obj(s)), s);
  EXPECT_EQ(s->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(Paykan_heap_live_blocks(), 2); // the string and its data
  EXPECT_STREQ(text(s), "a");
  // A weak reference loads it without a count, and its release leaves the
  // block: the object is never destroyed.
  PaykanWeak *w = Paykan_weak_new(s);
  EXPECT_EQ(Paykan_weak_load(w), s);
  EXPECT_EQ(s->strong, PAYKAN_REF_MAX);
  Paykan_weak_release(w);
  EXPECT_EQ(s->weak, 0);
  EXPECT_EQ(Paykan_heap_live_blocks(), 2);
  thaw(s, 1);
  Paykan_release(s);
}

TEST(Freeze, NullIsNothingToFreeze) {
  // An absent optional is a NULL box.
  Paykan_freeze(nullptr);
}

TEST(Freeze, WalksWhatTheObjectHoldsThroughTheVTableSlot) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *a = str("a");
  PaykanShared *b = str("b");
  PaykanShared *c = str("c");
  Paykan_retain(a); // held by the holder and by the test
  PaykanShared *h = holder(a, nullptr, {b, c});
  EXPECT_EQ(a->strong, 2);
  EXPECT_EQ(b->strong, 1);
  EXPECT_EQ(c->strong, 1);
  Paykan_freeze(obj(h));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(h->strong, PAYKAN_REF_MAX);
  // Its walk froze the reference fields (the NULL one is nothing) and the
  // references in the pair.
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(b->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(c->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(h)->second, nullptr);
  // The walk is the slot's, this class's own.
  EXPECT_TRUE(vtWalk(obj(h)) == &Holder_walk);
  // Freezing a frozen object is a no-op: no second walk, nothing counted.
  Paykan_freeze(obj(h));
  Paykan_freeze(obj(a));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(h->strong, PAYKAN_REF_MAX);
  thaw(h, 1);
  thaw(a, 2);
  thaw(b, 1);
  thaw(c, 1);
  Paykan_release(h); // its destroy releases a, b and c
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
}

// An object reached twice is walked once: the second visit finds it frozen
// (the count is set before the walk).  So a cycle ends too: the send check
// keeps `shared` parts out of a frozen graph and the lowering's walk skips
// `weak` fields, but the runtime relies on neither.
TEST(Freeze, WalksEachObjectOnce) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *inner = holder(str("i"), nullptr, {str("p"), nullptr});
  Paykan_retain(inner); // the outer holder holds it twice
  Paykan_retain(inner); // and the test once
  PaykanShared *outer = holder(inner, inner, {str("q"), nullptr});
  EXPECT_EQ(inner->strong, 3);
  Paykan_freeze(obj(outer));
  EXPECT_EQ(gWalks, 2);
  EXPECT_EQ(inner->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(inner)->first->strong, PAYKAN_REF_MAX);
  // A cycle: each of two holders holds the other.
  gWalks = 0;
  PaykanShared *x = holder(str("x"), nullptr, {str("xp"), nullptr});
  PaykanShared *y = holder(x, nullptr, {str("yp"), nullptr});
  Paykan_retain(x);
  Paykan_retain(y);
  asHolder(x)->second = y;
  Paykan_freeze(obj(x));
  EXPECT_EQ(gWalks, 2);
  EXPECT_EQ(x->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(y->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(x)->pair.a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(y)->pair.a->strong, PAYKAN_REF_MAX);
  // Teardown: the counts as they were, the cycle broken by hand.
  thaw(outer, 1);
  thaw(inner, 3);
  thaw(asHolder(inner)->first, 1);
  thaw(asHolder(inner)->pair.a, 1);
  thaw(asHolder(outer)->pair.a, 1);
  Paykan_release(outer); // releases inner twice
  EXPECT_EQ(inner->strong, 1);
  Paykan_release(inner);
  thaw(x, 2);
  thaw(y, 2);
  thaw(asHolder(x)->first, 1);
  thaw(asHolder(x)->pair.a, 1);
  thaw(asHolder(y)->pair.a, 1);
  asHolder(x)->second = nullptr;
  Paykan_release(y); // y's reference from x
  Paykan_release(y); // the test's; y's destroy releases x
  Paykan_release(x);
}

// -- A buffer: PaykanBuffer_walk

TEST(Freeze, ABufferFreezesTheReferencesItsValuesHold) {
  TrackingHeap heap;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 4);
  PaykanShared *a = str("a");
  PaykanShared *b = str("b");
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  PaykanBuffer_set(&kStr.head, &buf, 1, &b);
  PaykanBuffer_set(&kStr.head, &buf, 2, &b);
  // Value 3 was never set: it holds no reference, and the walk skips it.
  EXPECT_EQ(PaykanBuffer_isSet(&kStr.head, obj(buf), 3), 0);
  EXPECT_EQ(a->strong, 2);
  EXPECT_EQ(b->strong, 3);
  EXPECT_TRUE(vtWalk(obj(buf)) != &PaykanObject_walk);
  Paykan_freeze(obj(buf));
  EXPECT_EQ(buf->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(b->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(boxAt(kStr, buf, 0), a);
  EXPECT_EQ(boxAt(kStr, buf, 2), b);
  EXPECT_EQ(PaykanBuffer_isSet(&kStr.head, obj(buf), 3), 0);
  // Reading a frozen buffer's values copies them with no count.
  PaykanShared *got = nullptr;
  PaykanBuffer_copy(&kStr.head, &got, obj(buf), 0);
  EXPECT_EQ(got, a);
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  Paykan_release(got);
  thaw(buf, 1);
  thaw(a, 2);
  thaw(b, 3);
  Paykan_release(buf); // its destroy releases a once and b twice
  EXPECT_EQ(a->strong, 1);
  EXPECT_EQ(b->strong, 1);
  Paykan_release(a);
  Paykan_release(b);
}

TEST(Freeze, ABufferOfNumbersOrOfNoneHasNothingToFreeze) {
  TrackingHeap heap;
  PaykanShared *ints = PaykanBuffer_new(&kInt, 3);
  int64_t v = 7;
  PaykanBuffer_set(&kInt, &ints, 1, &v);
  Paykan_freeze(obj(ints));
  EXPECT_EQ(ints->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(*(const int64_t *)PaykanBuffer_get(&kInt, obj(ints), 1), 7);
  // An optional that is None is a NULL reference: nothing.
  PaykanShared *opts = PaykanBuffer_new(&kOpt.head, 2);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kOpt.head, &opts, 1, &a);
  Paykan_freeze(obj(opts));
  EXPECT_EQ(boxAt(kOpt, opts, 0), nullptr);
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  thaw(ints, 1);
  thaw(opts, 1);
  thaw(a, 2);
  Paykan_release(ints);
  Paykan_release(opts);
  Paykan_release(a);
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
  Paykan_freeze(obj(buf));
  EXPECT_EQ(w.ref->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(x.ref->strong, PAYKAN_REF_MAX);
  Wide got = {};
  PaykanBuffer_copy(&kWide.head, &got, obj(buf), 1);
  EXPECT_EQ(got.n, 8);
  EXPECT_EQ(got.ref, x.ref);
  EXPECT_FALSE(got.flag);
  thaw(buf, 1);
  thaw(w.ref, 2);
  thaw(x.ref, 2);
  Paykan_release(buf);
  Paykan_release(w.ref);
  Paykan_release(x.ref);
}

// An object holding a buffer (a `mem::Box<mem::Buffer<Str>>`, what
// samples/concurrency/README.md freezes for a parallel body) is walked in
// turn: the freeze recurses through each object's own slot.
TEST(Freeze, AnObjectHoldingABufferFreezesItsValuesToo) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *buf = PaykanBuffer_new(&kStr.head, 2);
  PaykanShared *a = str("a");
  PaykanBuffer_set(&kStr.head, &buf, 0, &a);
  PaykanBuffer_set(&kStr.head, &buf, 1, &a);
  PaykanShared *box = holder(buf, nullptr, {str("p"), nullptr});
  EXPECT_EQ(a->strong, 3);
  Paykan_freeze(obj(box));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(box->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(buf->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(box)->pair.a->strong, PAYKAN_REF_MAX);
  thaw(box, 1);
  thaw(buf, 1);
  thaw(a, 3);
  thaw(asHolder(box)->pair.a, 1);
  Paykan_release(box); // releases the buffer, whose destroy releases a twice
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
}

// -- Paykan_freeze_mem: the references in a value, by its type

TEST(FreezeMem, FollowsTheTypeDescriptor) {
  TrackingHeap heap;
  // A reference in the middle of a struct, found by its offset.
  Wide w = {7, str("w"), true};
  Paykan_freeze_mem(&kWide.head, &w);
  EXPECT_EQ(w.ref->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(w.n, 7);
  EXPECT_TRUE(w.flag);
  // Every reference of a value; a NULL one is nothing.
  Pair p = {str("a"), nullptr};
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(p.a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(p.b, nullptr);
  Pair q = {str("b"), str("c")};
  Paykan_freeze_mem(&kPair.head, &q);
  EXPECT_EQ(q.a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(q.b->strong, PAYKAN_REF_MAX);
  // A reference alone, and one that is None.
  PaykanShared *s = str("s");
  Paykan_freeze_mem(&kStr.head, &s);
  EXPECT_EQ(s->strong, PAYKAN_REF_MAX);
  PaykanShared *none = nullptr;
  Paykan_freeze_mem(&kOpt.head, &none);
  EXPECT_EQ(none, nullptr);
  // A number holds no reference: nothing to freeze.
  int64_t n = 5;
  Paykan_freeze_mem(&kInt, &n);
  EXPECT_EQ(n, 5);
  thaw(w.ref, 1);
  thaw(p.a, 1);
  thaw(q.a, 1);
  thaw(q.b, 1);
  thaw(s, 1);
  Paykan_release(w.ref);
  PaykanType_drop(&kPair.head, &p);
  PaykanType_drop(&kPair.head, &q);
  Paykan_release(s);
}

// What a reference in the value names is frozen with what it holds: the
// freeze of each goes through its walk slot, as Paykan_freeze's does.
TEST(FreezeMem, FreezesWhatTheReferencesHold) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *a = str("a");
  Pair p = {holder(a, nullptr, {str("p"), nullptr}), nullptr};
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(p.a->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(a->strong, PAYKAN_REF_MAX);
  // Again: the holder is frozen already, so nothing is walked.
  Paykan_freeze_mem(&kPair.head, &p);
  EXPECT_EQ(gWalks, 1);
  thaw(p.a, 1);
  thaw(a, 1);
  thaw(asHolder(p.a)->pair.a, 1);
  PaykanType_drop(&kPair.head, &p);
}

// -- The cells

// A walk reaches a handle like any reference, and the cell is immortal
// already, so the freeze stops there: an `atomic` field of a frozen object
// still changes, and a `locked` one keeps counting its contents.
TEST(Freeze, StopsAtAnAtomicOrLockedCell) {
  TrackingHeap heap;
  gWalks = 0;
  PaykanShared *hits = PaykanAtomic_new(0, PAYKAN_ATOMIC_INT);
  PaykanShared *payload = str("payload");
  PaykanShared *counts = PaykanLocked_new(payload);
  PaykanShared *cfg = holder(hits, counts, {str("name"), nullptr});
  Paykan_freeze(obj(cfg));
  EXPECT_EQ(gWalks, 1);
  EXPECT_EQ(cfg->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(hits->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(counts->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(asHolder(cfg)->pair.a->strong, PAYKAN_REF_MAX);
  // The atomic field still changes.
  EXPECT_EQ(PaykanAtomic_add(PaykanShared_get(hits), 1), 0);
  EXPECT_EQ(PaykanAtomic_load(PaykanShared_get(hits)), 1);
  // The locked cell's contents were not reached: still counted.
  EXPECT_EQ(payload->strong, 1);
  // Freezing a cell directly is a no-op too.
  Paykan_freeze(obj(hits));
  Paykan_freeze(obj(counts));
  EXPECT_EQ(payload->strong, 1);
  // Teardown: the holder's destroy releases the handles, which leaves the
  // cells alone; the test frees them as the runtime never does (the
  // prototype leaks every cell), the count put at 1 being the design's
  // thaw.
  thaw(cfg, 1);
  thaw(asHolder(cfg)->pair.a, 1);
  Paykan_release(cfg);
  EXPECT_EQ(hits->strong, PAYKAN_REF_MAX);
  EXPECT_EQ(counts->strong, PAYKAN_REF_MAX);
  thaw(hits, 1);
  Paykan_release(hits);
  PaykanShared **slot =
      static_cast<PaykanShared **>(PaykanLocked_lock(obj(counts)));
  EXPECT_EQ(*slot, payload);
  *slot = nullptr;
  PaykanLocked_unlock(obj(counts));
  Paykan_release(payload);
  thaw(counts, 1);
  Paykan_release(counts);
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
  thaw(frozen, 1);
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
  EXPECT_EQ(inner->strong, PAYKAN_REF_MAX);
  thaw(inner, 1);
  thaw(v, 1);
  Paykan_release(v);
}
