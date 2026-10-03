// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for iterative (deferred) destruction in Paykan_release (#118):
// a release that drops a count to zero while a destroy is running queues the
// dead box, and the outermost release destroys the deferred objects before it
// returns, in depth-first pre-order (each object's references in the order
// its destroy released them).  Deep chains must not grow the C stack.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "Runtime.h"
}

namespace {

// -- A test object type -------------------------------------------------------
//
// TestNode owns up to kMaxKids boxed children; its destroy logs its id, then
// releases the children in index order (exactly what a compiler-generated
// destroy does with its reference-typed fields), then frees itself.

constexpr int kMaxKids = 4;

struct TestNode {
  PaykanMethod *vtable;
  PaykanShared *shared;
  int id;
  PaykanShared *kids[kMaxKids];
};

std::vector<int> gLog; // destroy order

// How many destroys had started when each destroy finished releasing its
// children: shows whether a child's destroy ran inside the parent's destroy.
std::vector<size_t> gLogAtEnd;

void TestNode_destroy(PaykanObject *self) {
  auto *n = reinterpret_cast<TestNode *>(self);
  gLog.push_back(n->id);
  for (PaykanShared *kid : n->kids)
    Paykan_release(kid); // null-safe
  gLogAtEnd.push_back(gLog.size());
  Paykan_free(n);
}

PaykanMethod TestNode_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&TestNode_destroy),
    reinterpret_cast<PaykanMethod>(&PaykanObject_toString),
    reinterpret_cast<PaykanMethod>(&PaykanObject_equals),
};

PaykanShared *makeNode(int id, std::initializer_list<PaykanShared *> kids) {
  auto *n = static_cast<TestNode *>(Paykan_malloc(sizeof(TestNode)));
  std::memset(n, 0, sizeof *n);
  n->vtable = TestNode_vtable;
  n->id = id;
  int i = 0;
  for (PaykanShared *k : kids)
    n->kids[i++] = k; // consumed
  return PaykanShared_new(reinterpret_cast<PaykanObject *>(n));
}

class DeferredDestroy : public ::testing::Test {
protected:
  void SetUp() override {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
    gLog.clear();
    gLogAtEnd.clear();
  }
  void TearDown() override {
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_heap_set_tracking(0);
  }
};

// -- Tests --------------------------------------------------------------------

TEST_F(DeferredDestroy, SingleObjectIsDestroyedImmediately) {
  PaykanShared *a = makeNode(1, {});
  Paykan_release(a);
  EXPECT_EQ(gLog, std::vector<int>({1}));
}

TEST_F(DeferredDestroy, NestedReleaseIsQueuedNotRecursive) {
  // 1 -> 2 -> 3: when 1's destroy finishes releasing 2, 2 has not been
  // destroyed yet (it was queued); the outermost release then drains it.
  PaykanShared *chain = makeNode(1, {makeNode(2, {makeNode(3, {})})});
  Paykan_release(chain);
  EXPECT_EQ(gLog, std::vector<int>({1, 2, 3}));
  ASSERT_EQ(gLogAtEnd.size(), 3u);
  EXPECT_EQ(gLogAtEnd, std::vector<size_t>({1, 2, 3}));
}

TEST_F(DeferredDestroy, DestroyReleasingSeveralObjectsIsDepthFirst) {
  // 1 holds [2, 3, 4]; 2 holds [5, 6]; 4 holds [7]; 6 holds [8].
  // Depth-first pre-order, children in release (field) order: the order in
  // which a recursive destroy would have started them.
  PaykanShared *root = makeNode(
      1, {makeNode(2, {makeNode(5, {}), makeNode(6, {makeNode(8, {})})}),
          makeNode(3, {}), makeNode(4, {makeNode(7, {})})});
  Paykan_release(root);
  EXPECT_EQ(gLog, std::vector<int>({1, 2, 5, 6, 8, 3, 4, 7}));
  // Each destroy finished before any object it released was destroyed.
  EXPECT_EQ(gLogAtEnd, std::vector<size_t>({1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST_F(DeferredDestroy, NestedChainsAreDestroyedOneAfterAnother) {
  // A root holding three chains of 1000 links each: each chain is destroyed
  // in full, in field order, before the next one starts.
  constexpr int kLen = 1000;
  PaykanShared *chains[3] = {nullptr, nullptr, nullptr};
  for (int c = 0; c < 3; ++c)
    for (int i = kLen - 1; i >= 0; --i)
      chains[c] = makeNode((c + 1) * 10000 + i, {chains[c]});
  Paykan_release(makeNode(0, {chains[0], chains[1], chains[2]}));
  std::vector<int> expected = {0};
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < kLen; ++i)
      expected.push_back((c + 1) * 10000 + i);
  EXPECT_EQ(gLog, expected);
}

TEST_F(DeferredDestroy, SharedChildIsDestroyedOnceAtLastRelease) {
  // 1 and 2 both hold 3; 3 is destroyed when the second reference goes.
  PaykanShared *shared = makeNode(3, {});
  Paykan_retain(shared);
  PaykanShared *root = makeNode(1, {makeNode(2, {shared}), shared});
  Paykan_release(root);
  EXPECT_EQ(gLog, std::vector<int>({1, 2, 3}));
}

TEST_F(DeferredDestroy, SurvivingChildIsNotQueued) {
  PaykanShared *kid = makeNode(2, {});
  Paykan_retain(kid);
  Paykan_release(makeNode(1, {kid}));
  EXPECT_EQ(gLog, std::vector<int>({1}));
  // The outermost release finished and reset the queue: a later release
  // destroys at once again.
  Paykan_release(kid);
  EXPECT_EQ(gLog, std::vector<int>({1, 2}));
}

TEST_F(DeferredDestroy, NoneInsideDestroyIsHandled) {
  // A boxed None released inside a destroy is queued like any dead box; its
  // destroy is a no-op and it is left unboxed so it can be boxed again.
  PaykanShared *none = PaykanShared_new(&PaykanObject_None);
  Paykan_release(makeNode(1, {none, makeNode(2, {})}));
  EXPECT_EQ(PaykanObject_None.shared, nullptr);
  EXPECT_EQ(gLog, std::vector<int>({1, 2}));
}

TEST_F(DeferredDestroy, LongChainDoesNotOverflowTheStack) {
  // One million links, each a C stack frame (or more) if destruction were
  // recursive.
  constexpr int kLen = 1000000;
  PaykanShared *head = nullptr;
  for (int i = 0; i < kLen; ++i)
    head = makeNode(i, {head});
  Paykan_release(head);
  ASSERT_EQ(gLog.size(), static_cast<size_t>(kLen));
  EXPECT_EQ(gLog.front(), kLen - 1);
  EXPECT_EQ(gLog.back(), 0);
  // Never more than one destroy in progress.
  for (size_t i = 0; i < gLogAtEnd.size(); ++i)
    ASSERT_EQ(gLogAtEnd[i], i + 1);
}

TEST_F(DeferredDestroy, DeepChainsThroughRuntimeContainers) {
  // Arrays (element release) and tuples (slot release) are destroyed through
  // the same queue: a chain alternating array -> tuple -> array ... with a
  // Str or an Error and a boxed int hanging off every tuple.
  constexpr int kLen = 300000;
  PaykanShared *prev = nullptr;
  const uint8_t kinds[3] = {PAYKAN_TUPLE_REF, PAYKAN_TUPLE_REF,
                            PAYKAN_TUPLE_REF};
  for (int i = 0; i < kLen; ++i) {
    PaykanTuple *t = PaykanTuple_new(3, kinds);
    PaykanTuple_set_obj(t, 0, prev); // retains
    Paykan_release(prev);
    PaykanObject *leaf =
        i % 2 ? reinterpret_cast<PaykanObject *>(PaykanString_new("x", 1))
              : reinterpret_cast<PaykanObject *>(PaykanError_new("e", 1));
    PaykanShared *s = PaykanShared_new(leaf);
    PaykanTuple_set_obj(t, 1, s);
    Paykan_release(s);
    PaykanShared *b =
        PaykanShared_new(reinterpret_cast<PaykanObject *>(PaykanInt_new(i)));
    PaykanTuple_set_obj(t, 2, b);
    Paykan_release(b);
    PaykanShared *tb = PaykanShared_new(reinterpret_cast<PaykanObject *>(t));
    PaykanArray *arr = PaykanArray_new_obj(0);
    PaykanArray_push_obj(arr, tb); // retains
    Paykan_release(tb);
    prev = PaykanShared_new(reinterpret_cast<PaykanObject *>(arr));
  }
  Paykan_release(prev);
}

} // namespace
