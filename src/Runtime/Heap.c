// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — pluggable heap allocator.
//
// The whole runtime *and* JIT/AOT-generated code allocate through the public
// symbols Paykan_malloc / Paykan_realloc / Paykan_free.  Those symbols do not
// allocate directly; they dispatch through a small function-pointer table that
// selects one of two back-ends:
//
//   • passthrough (default) — thin wrappers over libc malloc/realloc/free.
//     Zero overhead, no bookkeeping.
//
//   • tracking — wraps every block in a header recording its payload size and
//     a magic word, and maintains live-block / live-byte counters so a leak
//     check can assert the program freed everything it allocated.
//
// The back-end is chosen once at start-up via Paykan_heap_set_tracking(),
// driven by a command-line flag in the driver (and by the test harness).
// Because the choice is made before any allocation happens, every pointer is
// allocated and freed by the same matching back-end, so the size header that
// the tracking allocator prepends never confuses the passthrough free (and
// vice versa).  Do NOT toggle the mode while live allocations exist.
//
// Statically-allocated singletons (e.g. PaykanObject_None) never pass through
// here and so correctly do not affect the counters.

#include "Runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Passthrough back-end (default) — plain libc, no bookkeeping
// ============================================================================

static void *passthrough_malloc(size_t size) { return malloc(size); }
static void *passthrough_realloc(void *p, size_t size) {
  return realloc(p, size);
}
static void passthrough_free(void *p) { free(p); }

// ============================================================================
// Tracking back-end — size header + live counters
// ============================================================================
//
// Layout in memory:  [ PaykanHeapHeader | ... payload ... ]
// The pointer returned to the caller points at the payload; the header sits
// immediately before it.  The header is padded so the payload stays 16-byte
// aligned, which is enough for any runtime struct (pointers and 8-byte
// scalars).

#define PAYKAN_HEAP_MAGIC ((uint64_t)0x50414b4e48454150ULL) // "PAKNHEAP"

typedef struct PaykanHeapHeader {
  uint64_t magic; // sanity check against double-free / foreign pointers
  uint64_t size;  // payload size in bytes (excludes this header)
} PaykanHeapHeader;

// Bytes reserved before the payload.  Rounded up to 16 for alignment.
#define PAYKAN_HEAP_HEADER_SIZE                                                \
  ((sizeof(PaykanHeapHeader) + 15UL) & ~((size_t)15UL))

static PaykanHeapStats g_stats;

static void heap_record_alloc(uint64_t size) {
  g_stats.liveBlocks += 1;
  g_stats.liveBytes += (int64_t)size;
  g_stats.totalAllocs += 1;
  if (g_stats.liveBytes > g_stats.peakBytes)
    g_stats.peakBytes = g_stats.liveBytes;
}

static void heap_record_free(uint64_t size) {
  g_stats.liveBlocks -= 1;
  g_stats.liveBytes -= (int64_t)size;
  g_stats.totalFrees += 1;
}

// Convert a payload pointer back to its header, validating the magic word.
static PaykanHeapHeader *heap_header_of(void *payload) {
  PaykanHeapHeader *h =
      (PaykanHeapHeader *)((char *)payload - PAYKAN_HEAP_HEADER_SIZE);
  if (h->magic != PAYKAN_HEAP_MAGIC) {
    fprintf(stderr, "paykan: heap corruption or invalid free of pointer %p\n",
            payload);
    abort();
  }
  return h;
}

static void *tracking_malloc(size_t size) {
  PaykanHeapHeader *h =
      (PaykanHeapHeader *)malloc(PAYKAN_HEAP_HEADER_SIZE + size);
  if (!h)
    return NULL;
  h->magic = PAYKAN_HEAP_MAGIC;
  h->size = (uint64_t)size;
  heap_record_alloc((uint64_t)size);
  return (char *)h + PAYKAN_HEAP_HEADER_SIZE;
}

static void *tracking_realloc(void *ptr, size_t size) {
  if (!ptr)
    return tracking_malloc(size);

  PaykanHeapHeader *oldH = heap_header_of(ptr);
  uint64_t oldSize = oldH->size;

  PaykanHeapHeader *newH =
      (PaykanHeapHeader *)realloc(oldH, PAYKAN_HEAP_HEADER_SIZE + size);
  if (!newH)
    return NULL; // original block left intact, counters unchanged

  // Adjust the live-byte count for the size delta; block count is unchanged.
  g_stats.liveBytes += (int64_t)size - (int64_t)oldSize;
  if (g_stats.liveBytes > g_stats.peakBytes)
    g_stats.peakBytes = g_stats.liveBytes;

  newH->magic = PAYKAN_HEAP_MAGIC;
  newH->size = (uint64_t)size;
  return (char *)newH + PAYKAN_HEAP_HEADER_SIZE;
}

static void tracking_free(void *ptr) {
  if (!ptr)
    return;
  PaykanHeapHeader *h = heap_header_of(ptr);
  heap_record_free(h->size);
  h->magic = 0; // poison so a subsequent double-free is detected
  free(h);
}

// ============================================================================
// Dispatch table
// ============================================================================
//
// Defaults to the passthrough back-end so that, unless tracking is explicitly
// requested, the runtime behaves exactly like plain malloc/free with no
// overhead.

static void *(*g_malloc)(size_t) = passthrough_malloc;
static void *(*g_realloc)(void *, size_t) = passthrough_realloc;
static void (*g_free)(void *) = passthrough_free;

static int g_tracking_enabled = 0;

// ============================================================================
// Public allocator entry points (called by the runtime and generated code)
// ============================================================================

void *Paykan_malloc(size_t size) { return g_malloc(size); }
void *Paykan_realloc(void *ptr, size_t size) { return g_realloc(ptr, size); }
void Paykan_free(void *ptr) { g_free(ptr); }

// ============================================================================
// Mode selection / diagnostics
// ============================================================================

void Paykan_heap_set_tracking(int enable) {
  if (enable) {
    g_malloc = tracking_malloc;
    g_realloc = tracking_realloc;
    g_free = tracking_free;
    g_tracking_enabled = 1;
  } else {
    g_malloc = passthrough_malloc;
    g_realloc = passthrough_realloc;
    g_free = passthrough_free;
    g_tracking_enabled = 0;
  }
}

int Paykan_heap_tracking_enabled(void) { return g_tracking_enabled; }

void Paykan_heap_reset(void) { memset(&g_stats, 0, sizeof(g_stats)); }

PaykanHeapStats Paykan_heap_stats(void) { return g_stats; }

int64_t Paykan_heap_live_blocks(void) { return g_stats.liveBlocks; }

int64_t Paykan_heap_live_bytes(void) { return g_stats.liveBytes; }

void Paykan_heap_dump(void) {
  if (!g_tracking_enabled) {
    fprintf(stderr, "paykan heap: allocation tracking is disabled "
                    "(pass the tracking flag to enable it)\n");
    return;
  }
  fprintf(stderr,
          "paykan heap stats:\n"
          "  total allocations : %lld\n"
          "  total frees       : %lld\n"
          "  live blocks       : %lld\n"
          "  live bytes        : %lld\n"
          "  peak bytes        : %lld\n",
          (long long)g_stats.totalAllocs, (long long)g_stats.totalFrees,
          (long long)g_stats.liveBlocks, (long long)g_stats.liveBytes,
          (long long)g_stats.peakBytes);
  if (g_stats.liveBlocks != 0)
    fprintf(stderr, "  ** LEAK: %lld block(s) / %lld byte(s) not freed **\n",
            (long long)g_stats.liveBlocks, (long long)g_stats.liveBytes);
}
