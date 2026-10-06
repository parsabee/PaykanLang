// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — the pluggable heap allocator (Runtime.h, "Pluggable heap
// allocator").  Paykan_malloc / Paykan_realloc / Paykan_free dispatch through
// a function-pointer table to the passthrough back-end (plain libc) or the
// tracking one (a size header before every block, plus live counters).  The
// mode must not change while blocks are live: a block is freed by the
// back-end that allocated it.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- Passthrough back-end

static void *passthrough_malloc(size_t size) { return malloc(size); }
static void *passthrough_realloc(void *p, size_t size) {
  return realloc(p, size);
}
static void passthrough_free(void *p) { free(p); }

// -- Tracking back-end
//
// [ PaykanHeapHeader | payload ]: the caller's pointer is the payload's; the
// header is padded so the payload stays 16-byte aligned.

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
    Paykan_runtime_panic("heap corruption or invalid free of pointer %p",
                         payload);
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
  g_stats.totalReallocs += 1;
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

// -- Dispatch table (the passthrough back-end until tracking is requested)

static void *(*g_malloc)(size_t) = passthrough_malloc;
static void *(*g_realloc)(void *, size_t) = passthrough_realloc;
static void (*g_free)(void *) = passthrough_free;

static int g_tracking_enabled = 0;

// -- Entry points

void *Paykan_malloc(size_t size) { return g_malloc(size); }
void *Paykan_realloc(void *ptr, size_t size) { return g_realloc(ptr, size); }
void Paykan_free(void *ptr) { g_free(ptr); }

// -- Mode selection / diagnostics

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

int64_t Paykan_heap_live_blocks(void) { return g_stats.liveBlocks; }

int64_t Paykan_heap_live_bytes(void) { return g_stats.liveBytes; }

int64_t Paykan_heap_total_reallocs(void) { return g_stats.totalReallocs; }

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
          "  total reallocs    : %lld\n"
          "  live blocks       : %lld\n"
          "  live bytes        : %lld\n"
          "  peak bytes        : %lld\n",
          (long long)g_stats.totalAllocs, (long long)g_stats.totalFrees,
          (long long)g_stats.totalReallocs, (long long)g_stats.liveBlocks,
          (long long)g_stats.liveBytes, (long long)g_stats.peakBytes);
  if (g_stats.liveBlocks != 0)
    fprintf(stderr, "  ** LEAK: %lld block(s) / %lld byte(s) not freed **\n",
            (long long)g_stats.liveBlocks, (long long)g_stats.liveBytes);
}
