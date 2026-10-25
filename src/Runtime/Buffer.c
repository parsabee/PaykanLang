// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — the storage of the standard library's memory::Buffer<T>.
//
// A buffer holds `len` live values of its PaykanType, `cap` allocated, each
// `type->size` bytes (Runtime.h, "Buffer").  The program reaches it only
// through generic native functions, which are passed T's PaykanType first
// and then the buffer: its object, borrowed (`view`), or the address of its
// box for a function that changes it (`inout`).  A value that was never set
// is all zero bytes: 0, False, or None.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/// The capacity a buffer never shrinks below on its own.
#define PAYKAN_BUFFER_MIN_CAP 8

static void PaykanBuffer_destroy(PaykanObject *self);

PaykanMethod PaykanBuffer_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanBuffer_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanObject_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
};

static void *buffer_value(const PaykanBuffer *buf, int64_t i) {
  return buf->data + (size_t)i * (size_t)buf->type->size;
}

/// Drop values [from, to): release the references they hold.
static void buffer_drop(PaykanBuffer *buf, int64_t from, int64_t to) {
  if (buf->type->nrefs == 0)
    return;
  for (int64_t i = from; i < to; ++i)
    PaykanType_drop(buf->type, buffer_value(buf, i));
}

static void PaykanBuffer_destroy(PaykanObject *self) {
  PaykanBuffer *buf = (PaykanBuffer *)self;
  buffer_drop(buf, 0, buf->len);
  Paykan_free(buf->data);
}

/// The buffer whose box is at @p box (an `inout Obj` argument).
static PaykanBuffer *buffer_at(const void *box) {
  return (PaykanBuffer *)PaykanShared_get_inline(
      *(PaykanShared *const *)box);
}

/// @p n as a value count; a negative one is the program's mistake.
static int64_t buffer_count(const char *what, int64_t n) {
  if (n < 0)
    Paykan_runtime_panic("buffer %s %lld: the count cannot be negative", what,
                         (long long)n);
  return n;
}

static void buffer_check_index(const PaykanBuffer *buf, int64_t i) {
  if (i < 0 || i >= buf->len)
    Paykan_runtime_panic("buffer index %lld out of bounds (len=%lld)",
                         (long long)i, (long long)buf->len);
}

/// Room for @p cap values: 0, or C's errno (ENOMEM when the size does not
/// fit) with the buffer unchanged.
static int buffer_try_realloc(PaykanBuffer *buf, int64_t cap) {
  const size_t size = (size_t)buf->type->size;
  if (size && (size_t)cap > SIZE_MAX / size)
    return ENOMEM;
  errno = 0;
  void *data = Paykan_realloc(buf->data, (size_t)cap * size);
  if (!data && cap > 0 && size > 0)
    return errno ? errno : ENOMEM;
  buf->data = data;
  buf->cap = cap;
  return 0;
}

/// Make the buffer @p len values long, the new ones all zero bytes: 0, or
/// C's errno with the buffer unchanged.  The allocation at least doubles, so
/// growing a value at a time is amortised O(1).
static int buffer_try_lengthen(PaykanBuffer *buf, int64_t len) {
  if (len <= buf->len)
    return 0;
  if (len > buf->cap) {
    int err = buffer_try_realloc(
        buf,
        buf->cap > len / 2 && buf->cap <= INT64_MAX / 2 ? buf->cap * 2 : len);
    // The doubled size may be what does not fit: then exactly @p len.
    if (err && len > buf->cap)
      err = buffer_try_realloc(buf, len);
    if (err)
      return err;
  }
  memset(buffer_value(buf, buf->len), 0,
         (size_t)(len - buf->len) * (size_t)buf->type->size);
  buf->len = len;
  return 0;
}

/// buffer_try_lengthen, for the callers that panic when it fails.
static void buffer_lengthen(PaykanBuffer *buf, int64_t len) {
  if (buffer_try_lengthen(buf, len))
    Paykan_runtime_panic("out of memory growing a buffer to %lld values",
                         (long long)len);
}

static void buffer_realloc(PaykanBuffer *buf, int64_t cap) {
  if (buffer_try_realloc(buf, cap))
    Paykan_runtime_panic("out of memory resizing a buffer to %lld values",
                         (long long)cap);
}

PaykanShared *PaykanBuffer_new(void *t, int64_t n) {
  int64_t len = buffer_count("of", n);
  PaykanBuffer *buf = (PaykanBuffer *)Paykan_malloc(sizeof(PaykanBuffer));
  buf->vtable = PaykanBuffer_vtable;
  buf->strong = 0; // unowned until boxed
  buf->weak = 0;
  buf->type = t;
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  buffer_lengthen(buf, len);
  return PaykanShared_new((PaykanObject *)buf);
}

int64_t PaykanBuffer_len(void *t, PaykanObject *b) {
  (void)t;
  return ((PaykanBuffer *)b)->len;
}

void *PaykanBuffer_get(void *t, PaykanObject *b, int64_t i) {
  PaykanBuffer *buf = (PaykanBuffer *)b;
  buffer_check_index(buf, i);
  void *v = buffer_value(buf, i);
  if (PaykanType_isUnset((const PaykanType *)t, v))
    Paykan_runtime_panic("buffer value %lld was never set", (long long)i);
  return v;
}

int64_t PaykanBuffer_isSet(void *t, PaykanObject *b, int64_t i) {
  PaykanBuffer *buf = (PaykanBuffer *)b;
  return i >= 0 && i < buf->len &&
         !PaykanType_isUnset((const PaykanType *)t, buffer_value(buf, i));
}

void PaykanBuffer_copy(void *t, void *out, PaykanObject *b, int64_t i) {
  PaykanType_copy((const PaykanType *)t, out, PaykanBuffer_get(t, b, i));
}

void PaykanBuffer_set(void *type, void *b, int64_t i, void *v) {
  const PaykanType *t = (const PaykanType *)type;
  PaykanBuffer *buf = buffer_at(b);
  buffer_check_index(buf, i);
  void *slot = buffer_value(buf, i);
  // Retain the new value before releasing the old, which may be the same.
  for (int64_t k = 0; k < t->nrefs; ++k) {
    PaykanShared *ref;
    memcpy(&ref, (const char *)v + (PaykanType_ref(t, k) >> 1),
           sizeof(PaykanShared *));
    Paykan_retain_inline(ref);
  }
  PaykanType_drop(t, slot);
  memmove(slot, v, (size_t)t->size);
}

void PaykanBuffer_reserve(void *t, void *b, int64_t n) {
  (void)t;
  buffer_lengthen(buffer_at(b), buffer_count("reserve", n));
}

void PaykanBuffer_grow(void *t, void *b, int64_t by) {
  (void)t;
  PaykanBuffer *buf = buffer_at(b);
  int64_t n = buffer_count("grow by", by);
  if (n > INT64_MAX - buf->len)
    Paykan_runtime_panic("out of memory growing a buffer by %lld values",
                         (long long)n);
  buffer_lengthen(buf, buf->len + n);
}

int64_t PaykanBuffer_tryGrow(void *t, void *b, int64_t by) {
  (void)t;
  PaykanBuffer *buf = buffer_at(b);
  // A count past INT64_MAX (an `unsigned`'s high half) cannot fit either.
  if (by < 0 || by > INT64_MAX - buf->len)
    return ENOMEM;
  return buffer_try_lengthen(buf, buf->len + by);
}

int64_t PaykanBuffer_tryReserve(void *t, void *b, int64_t n) {
  (void)t;
  return n < 0 ? ENOMEM : buffer_try_lengthen(buffer_at(b), n);
}

/// Drop the last @p by values.  Once at most a quarter of the allocation is
/// in use it is halved (never below PAYKAN_BUFFER_MIN_CAP), so a grow /
/// shrink pair never reallocates twice.
void PaykanBuffer_shrink(void *t, void *b, int64_t by) {
  (void)t;
  PaykanBuffer *buf = buffer_at(b);
  int64_t n = buffer_count("shrink by", by);
  if (n > buf->len)
    Paykan_runtime_panic("buffer shrink by %lld: it has only %lld values",
                         (long long)n, (long long)buf->len);
  buffer_drop(buf, buf->len - n, buf->len);
  buf->len -= n;
  int64_t cap = buf->cap;
  while (cap / 2 >= PAYKAN_BUFFER_MIN_CAP && buf->len <= cap / 4)
    cap /= 2;
  if (cap < buf->cap)
    buffer_realloc(buf, cap);
}
