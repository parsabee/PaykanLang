// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the runtime's heap allocator (Runtime.h, "Pluggable heap
// allocator"), on both back-ends: Paykan_malloc and Paykan_realloc panic when
// the memory is not there, and Paykan_tryMalloc and Paykan_tryRealloc
// (RuntimeInternal.h) return NULL with errno ENOMEM instead, a failed
// realloc leaving the old block as it was.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
#include "RuntimeInternal.h"
}

namespace {

/// The back-end @p tracking selects for one test's scope, with zeroed
/// counters; the tracking one ends with no live blocks.
struct Heap {
  explicit Heap(int tracking) : tracking(tracking) {
    Paykan_heap_set_tracking(tracking);
    Paykan_heap_reset();
  }
  ~Heap() {
    if (tracking) {
      EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    }
    Paykan_heap_set_tracking(0);
  }
  Heap(const Heap &) = delete;
  Heap &operator=(const Heap &) = delete;
  int tracking;
};

/// A size no allocator serves (more than half the address space), with room
/// left for the tracking back-end's header.
constexpr size_t kTooBig = SIZE_MAX / 2;

/// Whether the @p n bytes at @p p are all @p c.
bool allBytes(const void *p, size_t n, char c) {
  const char *bytes = static_cast<const char *>(p);
  for (size_t i = 0; i < n; ++i)
    if (bytes[i] != c)
      return false;
  return true;
}

} // namespace

TEST(Heap, TryMallocReportsOutOfMemory) {
  for (int tracking : {0, 1}) {
    Heap heap(tracking);
    errno = 0;
    EXPECT_EQ(Paykan_tryMalloc(kTooBig), nullptr) << tracking;
    EXPECT_EQ(errno, ENOMEM) << tracking;
    errno = 0;
    EXPECT_EQ(Paykan_tryRealloc(nullptr, kTooBig), nullptr) << tracking;
    EXPECT_EQ(errno, ENOMEM) << tracking;
    EXPECT_EQ(Paykan_heap_live_bytes(), 0) << tracking;
  }
}

TEST(Heap, AFailedTryReallocKeepsTheBlock) {
  for (int tracking : {0, 1}) {
    Heap heap(tracking);
    void *p = Paykan_malloc(32);
    std::memset(p, 'x', 32);
    errno = 0;
    EXPECT_EQ(Paykan_tryRealloc(p, kTooBig), nullptr) << tracking;
    EXPECT_EQ(errno, ENOMEM) << tracking;
    EXPECT_TRUE(allBytes(p, 32, 'x')) << tracking;
    if (tracking) {
      EXPECT_EQ(Paykan_heap_live_blocks(), 1);
      EXPECT_EQ(Paykan_heap_live_bytes(), 32);
    }
    Paykan_free(p);
  }
}

// The try variants allocate like the others when the memory is there, and
// the tracking back-end counts what they allocate.
TEST(Heap, TheTryVariantsAllocate) {
  for (int tracking : {0, 1}) {
    Heap heap(tracking);
    void *p = Paykan_tryMalloc(24);
    ASSERT_NE(p, nullptr) << tracking;
    std::memset(p, 'y', 24);
    void *grown = Paykan_tryRealloc(p, 4096);
    ASSERT_NE(grown, nullptr) << tracking;
    EXPECT_TRUE(allBytes(grown, 24, 'y')) << tracking;
    if (tracking) {
      EXPECT_EQ(Paykan_heap_live_blocks(), 1);
      EXPECT_EQ(Paykan_heap_live_bytes(), 4096);
      EXPECT_EQ(Paykan_heap_total_reallocs(), 1);
    }
    Paykan_free(grown);
  }
}

// A size of 0 is a block of its own on every back-end and every platform
// (C lets malloc(0) return NULL and realloc(p, 0) free p), so NULL always
// means out of memory.
TEST(Heap, ASizeOfZeroIsABlock) {
  for (int tracking : {0, 1}) {
    Heap heap(tracking);
    void *a = Paykan_malloc(0);
    void *b = Paykan_tryMalloc(0);
    ASSERT_NE(a, nullptr) << tracking;
    ASSERT_NE(b, nullptr) << tracking;
    EXPECT_NE(a, b) << tracking;
    void *p = Paykan_malloc(16);
    void *shrunk = Paykan_tryRealloc(p, 0);
    ASSERT_NE(shrunk, nullptr) << tracking;
    a = Paykan_realloc(a, 0);
    ASSERT_NE(a, nullptr) << tracking;
    if (tracking) {
      EXPECT_EQ(Paykan_heap_live_blocks(), 3);
    }
    Paykan_free(a);
    Paykan_free(b);
    Paykan_free(shrunk);
  }
}

// Generated code and the runtime's constructors use what Paykan_malloc and
// Paykan_realloc return unchecked: they never return NULL.
TEST(HeapDeath, MallocAndReallocPanicWhenOutOfMemory) {
  const std::string message =
      "paykan: out of memory allocating " + std::to_string(kTooBig) + " bytes";
  for (int tracking : {0, 1}) {
    EXPECT_DEATH(
        {
          Paykan_heap_set_tracking(tracking);
          Paykan_malloc(kTooBig);
        },
        message)
        << tracking;
    EXPECT_DEATH(
        {
          Paykan_heap_set_tracking(tracking);
          void *p = Paykan_malloc(8);
          Paykan_realloc(p, kTooBig);
        },
        message)
        << tracking;
  }
}
