// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — the storage of the standard library's memory::Buffer<T>.
//
// A buffer holds `len` live values of its PaykanType, `cap` allocated, each
// `type->size` bytes (Runtime.h, "Buffer").  The program reaches it only
// through generic native functions, which are passed T's PaykanType first
// and then the buffer: its object, borrowed (`view`), or the address of its
// box for a function that changes it (`inout`).  An index or a count is the
// program's `unsigned` in an int64_t, so one of 2^63 or more is negative
// here; a panic prints it unsigned, as the program wrote it.  A value that
// was never set is all zero bytes: 0, False, or None, or for a type whose
// zero bytes are no value (a class, a function value, a tagged union whose
// first variant holds one) storage the type's PaykanType tells apart.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static void PaykanBuffer_destroy(PaykanObject *self);
static void PaykanBuffer_walk(PaykanObject *self);

PaykanMethod PaykanBuffer_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanBuffer_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanObject_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanBuffer_walk,
};

static void *buffer_value(const PaykanBuffer *buf, int64_t i) {
  return buf->data + (size_t)i * (size_t)buf->type->size;
}

/// The walk of a buffer (the prototype): freezes the references its live
/// values hold (storage never set holds none).
static void PaykanBuffer_walk(PaykanObject *self) {
  PaykanBuffer *buf = (PaykanBuffer *)self;
  if (buf->type->nrefs == 0)
    return;
  for (int64_t i = 0; i < buf->len; ++i) {
    void *v = buffer_value(buf, i);
    if (!PaykanType_isUnset(buf->type, v))
      Paykan_freeze_mem(buf->type, v);
  }
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
  return (PaykanBuffer *)PaykanShared_get_inline(*(PaykanShared *const *)box);
}

/// Whether the value at @p v of type @p t was never set.  Only a type with
/// words that tell (`nunset`) is looked at.  The hint lays that look out of
/// line, not because such a type is rare: a read of a type whose zero bytes
/// are a value (an int, a Str?) then takes no branch.
static int buffer_unset(const PaykanType *t, const void *v) {
  return PAYKAN_UNLIKELY(t->nunset != 0) && PaykanType_unsetWords_inline(t, v);
}

static void buffer_check_index(const PaykanBuffer *buf, int64_t i) {
  if (i < 0 || i >= buf->len)
    Paykan_runtime_panic("buffer index %llu out of bounds (len=%llu)",
                         (unsigned long long)i, (unsigned long long)buf->len);
}

/// Room for @p cap values: 0, or ENOMEM (the size does not fit, or the
/// memory is not there) with the buffer unchanged.
static int buffer_try_realloc(PaykanBuffer *buf, int64_t cap) {
  const size_t size = (size_t)buf->type->size;
  if (size && (size_t)cap > SIZE_MAX / size)
    return ENOMEM;
  void *data = Paykan_tryRealloc(buf->data, (size_t)cap * size);
  if (!data)
    return ENOMEM;
  buf->data = data;
  buf->cap = cap;
  return 0;
}

/// Make the buffer @p len values long, the new ones all zero bytes: 0, or
/// ENOMEM with the buffer unchanged.  The allocation at least doubles, so
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

/// buffer_try_lengthen, for the callers that panic when it fails.  A length
/// of 2^63 or more (negative here) does not fit either.
static void buffer_lengthen(PaykanBuffer *buf, int64_t len) {
  if (len < 0 || buffer_try_lengthen(buf, len))
    Paykan_runtime_panic("out of memory growing a buffer to %llu values",
                         (unsigned long long)len);
}

static void buffer_realloc(PaykanBuffer *buf, int64_t cap) {
  if (buffer_try_realloc(buf, cap))
    Paykan_runtime_panic("out of memory resizing a buffer to %llu values",
                         (unsigned long long)cap);
}

PaykanShared *PaykanBuffer_new(void *t, int64_t n) {
  PaykanBuffer *buf = (PaykanBuffer *)Paykan_malloc(sizeof(PaykanBuffer));
  buf->vtable = PaykanBuffer_vtable;
  buf->strong = 0; // unowned until boxed
  buf->weak = 0;
  buf->type = t;
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
  buffer_lengthen(buf, n);
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
  if (buffer_unset((const PaykanType *)t, v))
    Paykan_panic_buffer_unset(i);
  return v;
}

int64_t PaykanBuffer_isSet(void *t, PaykanObject *b, int64_t i) {
  PaykanBuffer *buf = (PaykanBuffer *)b;
  return i >= 0 && i < buf->len &&
         !buffer_unset((const PaykanType *)t, buffer_value(buf, i));
}

void PaykanBuffer_copy(void *t, void *out, PaykanObject *b, int64_t i) {
  PaykanType_copy((const PaykanType *)t, out, PaykanBuffer_get(t, b, i));
}

void Paykan_mem_swap(void *t, void *a, void *b) {
  // Byte by byte: a value's references move with its bytes.
  unsigned char *x = (unsigned char *)a;
  unsigned char *y = (unsigned char *)b;
  for (int64_t n = ((const PaykanType *)t)->size; n > 0; --n, ++x, ++y) {
    unsigned char c = *x;
    *x = *y;
    *y = c;
  }
}

void Paykan_mem_move_out(void *t, void *out, void *slot) {
  const size_t size = (size_t)((const PaykanType *)t)->size;
  memcpy(out, slot, size);
  memset(slot, 0, size);
}

void PaykanBuffer_set(void *type, void *b, int64_t i, void *v) {
  const PaykanType *t = (const PaykanType *)type;
  PaykanBuffer *buf = buffer_at(b);
  buffer_check_index(buf, i);
  void *slot = buffer_value(buf, i);
  // Retain the new value before releasing the old, which may be the same.
  for (int64_t k = 0; k < t->nrefs; ++k) {
    PaykanShared *ref;
    memcpy(&ref, (const char *)v + PaykanType_refOffset(PaykanType_ref(t, k)),
           sizeof(PaykanShared *));
    Paykan_retain_inline(ref);
  }
  if (t->nrefs == 0) {
    memmove(slot, v, (size_t)t->size);
    return;
  }
  // The old value leaves the slot before it is dropped: what that runs (a
  // `__deinit__`) may read the buffer, and finds the new value there.
  unsigned char aside[64];
  const size_t size = (size_t)t->size;
  void *old = size <= sizeof aside ? aside : Paykan_malloc(size);
  memcpy(old, slot, size);
  memmove(slot, v, size);
  PaykanType_drop(t, old);
  if (old != aside)
    Paykan_free(old);
}

/// Halve the allocation while at most a quarter of it is in use (never
/// below PAYKAN_BUFFER_MIN_CAP), so a grow / shrink pair never reallocates
/// twice.
static void buffer_fit(PaykanBuffer *buf) {
  int64_t cap = buf->cap;
  while (cap / 2 >= PAYKAN_BUFFER_MIN_CAP && buf->len <= cap / 4)
    cap /= 2;
  if (cap < buf->cap)
    buffer_realloc(buf, cap);
}

void PaykanBuffer_pop(void *t, void *out, void *b) {
  const size_t size = (size_t)((const PaykanType *)t)->size;
  PaykanBuffer *buf = buffer_at(b);
  void *slot = PaykanBuffer_get(t, (PaykanObject *)buf, buf->len - 1);
  // The value's references leave with its bytes: nothing of it is dropped.
  memcpy(out, slot, size);
  buf->len -= 1;
  buffer_fit(buf);
}

void PaykanBuffer_swap(void *t, void *b, int64_t i, int64_t j) {
  PaykanBuffer *buf = buffer_at(b);
  buffer_check_index(buf, i);
  buffer_check_index(buf, j);
  Paykan_mem_swap(t, buffer_value(buf, i), buffer_value(buf, j));
}

void PaykanBuffer_reserve(void *t, void *b, int64_t n) {
  (void)t;
  buffer_lengthen(buffer_at(b), n);
}

void PaykanBuffer_grow(void *t, void *b, int64_t by) {
  (void)t;
  PaykanBuffer *buf = buffer_at(b);
  // A count of 2^63 or more (negative here) cannot fit either.
  if (by < 0 || by > INT64_MAX - buf->len)
    Paykan_runtime_panic("out of memory growing a buffer by %llu values",
                         (unsigned long long)by);
  buffer_lengthen(buf, buf->len + by);
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

/// Drop the last @p by values, then see to the allocation (buffer_fit).
///
/// The values leave the buffer before they are dropped: what a drop runs (a
/// `__deinit__`) may read the buffer or change it, and finds none of them
/// in it, nor writes over one still to drop.  A buffer that is emptied
/// gives them its whole storage; a few bytes go to the stack, more to an
/// allocation (with no memory for one they are dropped where they are,
/// past the buffer's new end).  A drop may also let go of the buffer
/// itself, by assigning the place that names it, so it is held until the
/// allocation is seen to.
void PaykanBuffer_shrink(void *t, void *b, int64_t by) {
  (void)t;
  PaykanBuffer *buf = buffer_at(b);
  if (by < 0 || by > buf->len)
    Paykan_runtime_panic("buffer shrink by %llu: it has only %llu values",
                         (unsigned long long)by, (unsigned long long)buf->len);
  buf->len -= by;
  const PaykanType *type = buf->type;
  if (type->nrefs == 0 || by == 0) {
    buffer_fit(buf);
    return;
  }
  Paykan_retain((PaykanShared *)buf);
  unsigned char few[256];
  const size_t size = (size_t)type->size;
  const size_t bytes = (size_t)by * size;
  unsigned char *aside = few;
  const int whole = bytes > sizeof few && buf->len == 0;
  if (whole) {
    aside = buf->data;
    buf->data = NULL;
    buf->cap = 0;
  } else if (bytes > sizeof few) {
    aside = (unsigned char *)Paykan_tryRealloc(NULL, bytes);
  }
  if (!aside) {
    buffer_drop(buf, buf->len, buf->len + by);
  } else {
    if (!whole)
      memcpy(aside, buffer_value(buf, buf->len), bytes);
    for (int64_t i = 0; i < by; ++i)
      PaykanType_drop(type, aside + (size_t)i * size);
    if (aside != few)
      Paykan_free(aside);
  }
  buffer_fit(buf);
  Paykan_release((PaykanShared *)buf);
}
