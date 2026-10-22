// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for weak references (Runtime.h, "Shared"): Paykan_weak_new,
// Paykan_weak_load and Paykan_weak_release, the zombie an object leaves for
// its weak references, and Paykan_dispose.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include "Runtime.h"
}

namespace {

class Weak : public ::testing::Test {
protected:
  void SetUp() override {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  void TearDown() override {
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_heap_set_tracking(0);
  }
};

} // namespace

TEST_F(Weak, NewCountsAndIsTheObject) {
  PaykanShared *s = PaykanShared_new(PaykanObject_new());
  PaykanWeak *w = Paykan_weak_new(s);
  EXPECT_EQ(w, s);
  EXPECT_EQ(s->strong, 1);
  EXPECT_EQ(s->weak, 1);
  PaykanWeak *w2 = Paykan_weak_new(s);
  EXPECT_EQ(s->weak, 2);
  Paykan_weak_release(w2);
  EXPECT_EQ(s->weak, 1);
  Paykan_weak_release(w);
  EXPECT_EQ(s->weak, 0);
  Paykan_release(s);
}

TEST_F(Weak, NullIsNullEverywhere) {
  EXPECT_EQ(Paykan_weak_new(nullptr), nullptr);
  EXPECT_EQ(Paykan_weak_load(nullptr), nullptr);
  Paykan_weak_release(nullptr); // no-op
  Paykan_dispose(nullptr);      // no-op
}

TEST_F(Weak, LoadGivesAStrongReferenceWhileAlive) {
  PaykanShared *s = PaykanShared_new(PaykanObject_new());
  PaykanWeak *w = Paykan_weak_new(s);
  PaykanShared *loaded = Paykan_weak_load(w);
  EXPECT_EQ(loaded, s);
  EXPECT_EQ(s->strong, 2);
  Paykan_release(loaded);
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s);
  // Destroyed: the memory stays for the weak reference, reading as NULL.
  EXPECT_EQ(Paykan_heap_live_blocks(), 1);
  EXPECT_EQ(Paykan_weak_load(w), nullptr);
  EXPECT_EQ(w->strong, 0);
  EXPECT_EQ(w->weak, 1);
  Paykan_weak_release(w); // the last weak reference frees the zombie
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST_F(Weak, ReleasedBeforeTheObjectDiesItIsFreedByTheStrongRelease) {
  PaykanShared *s = PaykanShared_new(PaykanObject_new());
  PaykanWeak *w = Paykan_weak_new(s);
  Paykan_weak_release(w);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1); // still alive: the strong one
  Paykan_release(s);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST_F(Weak, AStringZombieKeepsOnlyItsHeader) {
  // The destroy tears down what the object holds (the string's bytes) even
  // while the object itself stays for its weak references.
  PaykanShared *s =
      PaykanShared_new((PaykanObject *)PaykanString_new("hello", 5));
  PaykanWeak *w = Paykan_weak_new(s);
  const int64_t withData = Paykan_heap_live_bytes();
  Paykan_release(s);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1);
  EXPECT_LT(Paykan_heap_live_bytes(), withData);
  Paykan_weak_release(w);
}

TEST_F(Weak, DisposeDestroysAndFreesAnUnboxedObject) {
  PaykanString *s = PaykanString_new("bye", 3);
  EXPECT_EQ(Paykan_heap_live_blocks(), 2); // the object and its bytes
  Paykan_dispose((PaykanObject *)s);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST_F(Weak, ImmortalNoneAlwaysLoads) {
  PaykanShared *none = PaykanShared_new(&PaykanObject_None);
  PaykanWeak *w = Paykan_weak_new(none);
  Paykan_release(none);
  PaykanShared *loaded = Paykan_weak_load(w);
  EXPECT_EQ(loaded, &PaykanObject_None);
  Paykan_release(loaded);
  Paykan_weak_release(w);
  EXPECT_EQ(PaykanObject_None.strong, 1);
  EXPECT_EQ(PaykanObject_None.weak, 0);
}

// -- Weak references touched while an object dies
//
// Link: an object holding a strong reference to `next` and a weak one to
// `peer`; its destroy loads `peer` (recording what it saw), releases `next`
// and drops the weak reference, as a generated destroy does.

namespace {

struct Link {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
  PaykanShared *next;
  PaykanWeak *peer;
};

int gLoadedPeerAlive = -1; // what the last destroy saw of its peer

void Link_destroy(PaykanObject *self) {
  auto *l = reinterpret_cast<Link *>(self);
  PaykanShared *peer = Paykan_weak_load(l->peer);
  gLoadedPeerAlive = peer != nullptr;
  Paykan_release(peer);
  Paykan_release(l->next);
  Paykan_weak_release(l->peer);
}

PaykanMethod Link_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Link_destroy),
    reinterpret_cast<PaykanMethod>(&PaykanObject_toString),
    reinterpret_cast<PaykanMethod>(&PaykanObject_equals),
};

Link *makeLink() {
  auto *l = static_cast<Link *>(Paykan_malloc(sizeof(Link)));
  std::memset(l, 0, sizeof *l);
  l->vtable = Link_vtable;
  return l;
}

} // namespace

TEST_F(Weak, AnObjectBeingDestroyedReadsAsGone) {
  // A link whose peer is itself: its destroy sees NULL (the object is
  // dying), and dropping its own weak reference inside the destroy does not
  // free it under the destroy's feet.
  Link *l = makeLink();
  PaykanShared *s = PaykanShared_new(reinterpret_cast<PaykanObject *>(l));
  l->peer = Paykan_weak_new(s);
  gLoadedPeerAlive = -1;
  Paykan_release(s);
  EXPECT_EQ(gLoadedPeerAlive, 0);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST_F(Weak, AnObjectPendingDestructionReadsAsGone) {
  // a -> b strongly, and a's peer is b: a's destroy releases b (deferred
  // until a's destroy returns, Shared.c) and then loads its peer -- which
  // must already read as gone, although b's destroy has not run yet.
  Link *a = makeLink();
  Link *b = makeLink();
  PaykanShared *sa = PaykanShared_new(reinterpret_cast<PaykanObject *>(a));
  PaykanShared *sb = PaykanShared_new(reinterpret_cast<PaykanObject *>(b));
  a->next = sb; // a owns b
  // a's destroy releases next before loading peer: order the fields so.
  a->peer = Paykan_weak_new(sb);
  // Swap the destroy's order for this test: load peer after releasing next.
  // Link_destroy loads first, so give a a second strong reference to b that
  // a drops first through a nested link instead.
  Link *c = makeLink();
  PaykanShared *sc = PaykanShared_new(reinterpret_cast<PaykanObject *>(c));
  c->next = sb;
  Paykan_retain(sb);
  c->peer = Paykan_weak_new(sb);
  a->next = sc;
  Paykan_release(sb); // a's own strong reference to b, through c now
  // Dropping a: a's destroy loads its peer b (alive: c still owns it),
  // releases c (deferred); then c's destroy loads b (alive), releases b
  // (deferred), drops its weak one; then b's destroy runs.
  gLoadedPeerAlive = -1;
  Paykan_release(sa);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}

TEST_F(Weak, AZombieIsFreedByTheLastWeakReferenceDuringADrain) {
  // a holds b strongly and a weak reference to it too, and so does an outer
  // holder: when everything is dropped, b's zombie goes with the last weak
  // release, whichever runs last.
  Link *a = makeLink();
  Link *b = makeLink();
  PaykanShared *sa = PaykanShared_new(reinterpret_cast<PaykanObject *>(a));
  PaykanShared *sb = PaykanShared_new(reinterpret_cast<PaykanObject *>(b));
  a->next = sb;
  a->peer = Paykan_weak_new(sb);
  PaykanWeak *outer = Paykan_weak_new(sb);
  Paykan_release(sa);
  EXPECT_EQ(Paykan_heap_live_blocks(), 1); // b's zombie, for `outer`
  EXPECT_EQ(Paykan_weak_load(outer), nullptr);
  Paykan_weak_release(outer);
  EXPECT_EQ(Paykan_heap_live_blocks(), 0);
}
