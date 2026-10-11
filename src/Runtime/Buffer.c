// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — the storage of the standard library's memory::Buffer<T>.
//
// A buffer is a PaykanArray every slot of which is live: `len` is how many
// slots the program can read and write, `cap` how many are allocated.  The
// program reaches it only through these generic native functions (Runtime.h,
// "Generic native functions"), which are passed T's kind first and then the
// buffer: its box, borrowed (`view`), or the address of the slot holding it
// for a function that changes it (`inout`).  A buffer of references has the
// object array's vtable, so destroying it releases them.  A slot that was
// never set holds 0, or NULL: None.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdint.h>
#include <string.h>

#define PAYKAN_BUFFER_SLOT 8UL

/// The capacity a buffer never shrinks below on its own.
#define PAYKAN_BUFFER_MIN_CAP 8UL

static int is_ref(int64_t kind) {
  return kind == PAYKAN_KIND_OBJECT || kind == PAYKAN_KIND_OPTIONAL;
}

/// The box a slot's bits are, read by memcpy as Tuple.c does (Runtime.h
/// asserts a pointer is the slot's 8 bytes).
static PaykanShared *as_box(int64_t bits) {
  PaykanShared *box;
  memcpy(&box, &bits, sizeof bits);
  return box;
}

static int64_t box_bits(PaykanShared *box) {
  int64_t bits;
  memcpy(&bits, &box, sizeof bits);
  return bits;
}

static PaykanArray *buffer_of(int64_t buf) {
  return (PaykanArray *)PaykanShared_get(as_box(buf));
}

/// The buffer whose box is in the slot at @p slot (an `inout` argument).
static PaykanArray *buffer_at(const void *slot) {
  int64_t buf;
  memcpy(&buf, slot, sizeof buf);
  return buffer_of(buf);
}

static int64_t *buffer_slots(const PaykanArray *buf) {
  return (int64_t *)buf->data;
}

/// @p n as a slot count; a negative one is the program's mistake.
static unsigned long buffer_count(const char *what, int64_t n) {
  if (n < 0)
    Paykan_runtime_panic("buffer %s %lld: the count cannot be negative", what,
                         (long long)n);
  return (unsigned long)n;
}

static void buffer_check_index(const PaykanArray *buf, int64_t i) {
  if (i < 0 || (unsigned long)i >= buf->len)
    Paykan_runtime_panic("buffer index %lld out of bounds (len=%lu)",
                         (long long)i, buf->len);
}

static void buffer_realloc(PaykanArray *buf, unsigned long cap) {
  if (cap > SIZE_MAX / PAYKAN_BUFFER_SLOT)
    Paykan_runtime_panic("out of memory growing a buffer to %lu slots", cap);
  buf->data = Paykan_realloc(buf->data, cap * PAYKAN_BUFFER_SLOT);
  if (!buf->data && cap > 0)
    Paykan_runtime_panic("out of memory growing a buffer to %lu slots", cap);
  buf->cap = cap;
}

/// Make the buffer @p len slots long, the new ones 0 / None.  The
/// allocation at least doubles, so growing a slot at a time is amortised
/// O(1).
static void buffer_lengthen(PaykanArray *buf, unsigned long len) {
  if (len <= buf->len)
    return;
  if (len > buf->cap)
    buffer_realloc(buf, buf->cap * 2 > len ? buf->cap * 2 : len);
  memset(buffer_slots(buf) + buf->len, 0,
         (len - buf->len) * PAYKAN_BUFFER_SLOT);
  buf->len = len;
}

int64_t PaykanBuffer_new(int64_t kind, int64_t n) {
  unsigned long len = buffer_count("of", n);
  PaykanArray *buf =
      is_ref(kind) ? PaykanArray_new_obj(len) : PaykanArray_new(len); // zeroed
  return box_bits(PaykanShared_new((PaykanObject *)buf));
}

int64_t PaykanBuffer_len(int64_t kind, int64_t buf) {
  (void)kind;
  return (int64_t)buffer_of(buf)->len;
}

int64_t PaykanBuffer_get(int64_t kind, int64_t buf, int64_t i) {
  PaykanArray *b = buffer_of(buf);
  buffer_check_index(b, i);
  int64_t v = buffer_slots(b)[i];
  if (is_ref(kind)) {
    if (!v) {
      if (kind == PAYKAN_KIND_OBJECT)
        Paykan_runtime_panic("buffer slot %lld was never set", (long long)i);
      return 0; // None
    }
    Paykan_retain(as_box(v)); // the result is owned
  }
  return v;
}

void PaykanBuffer_set(int64_t kind, void *buf, int64_t i, int64_t v) {
  PaykanArray *b = buffer_at(buf);
  buffer_check_index(b, i);
  int64_t *slot = buffer_slots(b) + i;
  if (is_ref(kind)) {
    if (v)
      Paykan_retain(as_box(v));
    if (*slot)
      Paykan_release(as_box(*slot));
  }
  *slot = v;
}

void PaykanBuffer_reserve(int64_t kind, void *buf, int64_t n) {
  (void)kind;
  buffer_lengthen(buffer_at(buf), buffer_count("reserve", n));
}

void PaykanBuffer_grow(int64_t kind, void *buf, int64_t by) {
  (void)kind;
  PaykanArray *b = buffer_at(buf);
  unsigned long n = buffer_count("grow by", by);
  if (n > ULONG_MAX - b->len)
    Paykan_runtime_panic("out of memory growing a buffer by %lu slots", n);
  buffer_lengthen(b, b->len + n);
}

/// Drop the last @p by slots, releasing what they hold.  Once at most a
/// quarter of the allocation is in use it is halved (never below
/// PAYKAN_BUFFER_MIN_CAP), so a grow / shrink pair never reallocates twice.
void PaykanBuffer_shrink(int64_t kind, void *buf, int64_t by) {
  PaykanArray *b = buffer_at(buf);
  unsigned long n = buffer_count("shrink by", by);
  if (n > b->len)
    Paykan_runtime_panic("buffer shrink by %lu: it has only %lu slots", n,
                         b->len);
  if (is_ref(kind))
    for (unsigned long i = b->len - n; i < b->len; ++i)
      if (buffer_slots(b)[i])
        Paykan_release(as_box(buffer_slots(b)[i]));
  b->len -= n;
  unsigned long cap = b->cap;
  while (cap / 2 >= PAYKAN_BUFFER_MIN_CAP && b->len <= cap / 4)
    cap /= 2;
  if (cap < b->cap)
    buffer_realloc(b, cap);
}
