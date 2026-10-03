// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Array type implementation.
//
// There are two concrete array variants that share the same PaykanArray struct
// layout but have different vtables and helper functions:
//
//   Primitive arrays  (int / float / bool elements)
//     constructor : PaykanArray_new(len)
//     vtable      : PaykanArray_vtable
//     set         : PaykanArray_set(arr, idx, value)   — plain memcpy
//     get         : PaykanArray_get(arr, idx)           — plain memcpy
//     destroy     : PaykanArray_destroy                 — free buffer + self
//
//   Object arrays  (class-type elements stored as PaykanShared*)
//     constructor : PaykanArray_new_obj(len)
//     vtable      : PaykanArray_obj_vtable
//     set         : PaykanArray_set_obj(arr, idx, value) — retain new, release
//     old get         : PaykanArray_get(arr, idx)            — same plain read
//     destroy     : PaykanArray_destroy_obj              — release all + free
//     buffer + self
//
// Every element slot is exactly 8 bytes wide regardless of element type.
// CodeGen selects the right constructor/set/destroy based on the static
// element type known at compile time.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYKAN_ELEM_SIZE 8UL

// ============================================================================
// Forward declarations
// ============================================================================

void PaykanArray_destroy(PaykanObject *self);
void PaykanArray_destroy_obj(PaykanObject *self);
PaykanShared *PaykanArray_toString(PaykanObject *self);
int64_t PaykanArray_equals(PaykanObject *self, PaykanObject *other);
int64_t PaykanArray_length(PaykanObject *self);

// ============================================================================
// VTables
// ============================================================================

// Primitive elements — destroy just frees the buffer.
PaykanArrayVTable PaykanArray_vtable = {
    .destroy = PaykanArray_destroy,
    .toString = PaykanArray_toString,
    .equals = PaykanArray_equals,
    .length = PaykanArray_length,
};

// Object elements — destroy releases every PaykanShared* before freeing.
PaykanArrayVTable PaykanArray_obj_vtable = {
    .destroy = PaykanArray_destroy_obj,
    .toString = PaykanArray_toString,
    .equals = PaykanArray_equals,
    .length = PaykanArray_length,
};

// ============================================================================
// Constructors
// ============================================================================

// Primitive array (int / float / bool): elements are stored as raw 8-byte
// values; no reference counting.
PaykanArray *PaykanArray_new(unsigned long len) {
  PaykanArray *arr = (PaykanArray *)Paykan_malloc(sizeof(PaykanArray));
  arr->vtable = (PaykanObjectVTable *)&PaykanArray_vtable;
  arr->shared = NULL; // not yet boxed (unique-box invariant)
  arr->len = len;
  arr->cap = len;
  if (len > 0) {
    arr->data = Paykan_malloc(len * PAYKAN_ELEM_SIZE);
    memset(arr->data, 0, len * PAYKAN_ELEM_SIZE);
  } else {
    arr->data = NULL;
  }
  return arr;
}

// Primitive array from a compile-time constant data buffer (e.g. a literal).
// 'data' must point to (len * 8) bytes of packed i64 values.
PaykanArray *PaykanArray_new_from_data(unsigned long len, const void *data) {
  PaykanArray *arr = (PaykanArray *)Paykan_malloc(sizeof(PaykanArray));
  arr->vtable = (PaykanObjectVTable *)&PaykanArray_vtable;
  arr->shared = NULL; // not yet boxed (unique-box invariant)
  arr->len = len;
  arr->cap = len;
  if (len > 0) {
    arr->data = Paykan_malloc(len * PAYKAN_ELEM_SIZE);
    memcpy(arr->data, data, len * PAYKAN_ELEM_SIZE);
  } else {
    arr->data = NULL;
  }
  return arr;
}

// Object array (class-type elements stored as PaykanShared*): reference
// counts are managed by PaykanArray_set_obj / PaykanArray_destroy_obj.
PaykanArray *PaykanArray_new_obj(unsigned long len) {
  PaykanArray *arr = (PaykanArray *)Paykan_malloc(sizeof(PaykanArray));
  arr->vtable = (PaykanObjectVTable *)&PaykanArray_obj_vtable;
  arr->shared = NULL; // not yet boxed (unique-box invariant)
  arr->len = len;
  arr->cap = len;
  if (len > 0) {
    arr->data = Paykan_malloc(len * PAYKAN_ELEM_SIZE);
    // Zero-init so every slot starts as NULL (no accidental release on first
    // set).
    memset(arr->data, 0, len * PAYKAN_ELEM_SIZE);
  } else {
    arr->data = NULL;
  }
  return arr;
}

// ============================================================================
// Destructors
// ============================================================================

// Primitive destructor: just free the buffer and the array itself.
void PaykanArray_destroy(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  Paykan_free(arr->data);
  Paykan_free(arr);
}

// Object destructor: release every non-null PaykanShared* slot, then free.
void PaykanArray_destroy_obj(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  if (arr->data) {
    for (unsigned long i = 0; i < arr->len; ++i) {
      PaykanShared *elem;
      memcpy(&elem, (char *)arr->data + i * PAYKAN_ELEM_SIZE, PAYKAN_ELEM_SIZE);
      if (elem)
        Paykan_release(elem);
    }
  }
  Paykan_free(arr->data);
  Paykan_free(arr);
}

// ============================================================================
// Element access — get (shared by both variants)
// ============================================================================

// Read an 8-byte slot as a void*.
// For primitive arrays the caller reinterprets the bits as int64_t or double.
// For object arrays the caller uses the result as a PaykanShared*.
void *PaykanArray_get(PaykanArray *arr, unsigned long idx) {
  if (idx >= arr->len) {
    Paykan_runtime_panic("array index %lu out of bounds (len=%lu)", idx,
                         arr->len);
  }
  void *val;
  memcpy(&val, (char *)arr->data + idx * PAYKAN_ELEM_SIZE, PAYKAN_ELEM_SIZE);
  return val;
}

// ============================================================================
// Element write — two variants
// ============================================================================

// Primitive set: plain 8-byte store, no reference counting.
void PaykanArray_set(PaykanArray *arr, unsigned long idx, void *value) {
  if (idx >= arr->len) {
    Paykan_runtime_panic("array index %lu out of bounds (len=%lu)", idx,
                         arr->len);
  }
  memcpy((char *)arr->data + idx * PAYKAN_ELEM_SIZE, &value, PAYKAN_ELEM_SIZE);
}

// Object set: release the old PaykanShared* in the slot (if any), retain the
// incoming one, then store it.
void PaykanArray_set_obj(PaykanArray *arr, unsigned long idx,
                         PaykanShared *value) {
  if (idx >= arr->len) {
    Paykan_runtime_panic("array index %lu out of bounds (len=%lu)", idx,
                         arr->len);
  }
  void *slot = (char *)arr->data + idx * PAYKAN_ELEM_SIZE;
  PaykanShared *old;
  memcpy(&old, slot, PAYKAN_ELEM_SIZE);
  if (old)
    Paykan_release(old);
  if (value)
    Paykan_retain(value);
  memcpy(slot, &value, PAYKAN_ELEM_SIZE);
}

// ============================================================================
// Method implementations (shared by both variants)
// ============================================================================

PaykanShared *PaykanArray_toString(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  char buf[80];
  int n = snprintf(buf, sizeof(buf), "Array@%p[len=%lu]", (void *)arr,
                   (unsigned long)arr->len);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanArray_equals(PaykanObject *self, PaykanObject *other) {
  // Identity equality — two arrays are equal only if they are the same object.
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h).
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

int64_t PaykanArray_length(PaykanObject *self) {
  return (int64_t)((PaykanArray *)self)->len;
}

// ============================================================================
// push / pop — dynamic resize operations
// ============================================================================
//
// Capacity policy (with hysteresis, so a push/pop pair never reallocates
// twice; see issue #95):
//
//   Growth : when len == cap, double the capacity, but never below
//            PAYKAN_ARRAY_MIN_CAP.
//   Shrink : after a pop, only if cap > PAYKAN_ARRAY_MIN_CAP and
//            len <= cap / 4, halve the capacity (never below
//            PAYKAN_ARRAY_MIN_CAP, never to exactly len).
//
// After a shrink len <= newCap / 2, so the array must double in length before
// the next growth and halve again before the next shrink: every resize is
// separated by at least newCap / 4 push/pop operations, which keeps push and
// pop amortised O(1) even when the length oscillates around a power of two.
// The array never shrinks on its own below PAYKAN_ARRAY_MIN_CAP slots; an
// array built with a smaller exact capacity (e.g. a short literal) keeps it.
//
// Slots in [len, cap) are never read: get/set are bounds-checked against
// len, push writes a slot before counting it, and destroy_obj releases only
// [0, len).  So freshly grown slots are left uninitialised and pop does not
// clear the slot it vacates.  (The constructors still zero [0, len) because
// those slots are live and destroy_obj / set_obj read them.)
// ============================================================================

#define PAYKAN_ARRAY_MIN_CAP 8UL

static void array_resize(PaykanArray *arr, unsigned long newCap) {
  arr->data = Paykan_realloc(arr->data, newCap * PAYKAN_ELEM_SIZE);
  if (!arr->data)
    Paykan_runtime_panic("out of memory resizing array to %lu elements",
                         newCap);
  arr->cap = newCap;
}

// Ensure there is room for at least one more element.
static void array_grow(PaykanArray *arr) {
  if (arr->len < arr->cap)
    return; // still room
  unsigned long newCap = arr->cap * 2;
  if (newCap < PAYKAN_ARRAY_MIN_CAP)
    newCap = PAYKAN_ARRAY_MIN_CAP;
  array_resize(arr, newCap);
}

// Halve the allocation once len has fallen to a quarter of cap.
static void array_maybe_shrink(PaykanArray *arr) {
  if (arr->cap <= PAYKAN_ARRAY_MIN_CAP || arr->len > arr->cap / 4)
    return;
  unsigned long newCap = arr->cap / 2;
  if (newCap < PAYKAN_ARRAY_MIN_CAP)
    newCap = PAYKAN_ARRAY_MIN_CAP;
  array_resize(arr, newCap);
}

void PaykanArray_push(PaykanArray *arr, void *value) {
  array_grow(arr);
  memcpy((char *)arr->data + arr->len * PAYKAN_ELEM_SIZE, &value,
         PAYKAN_ELEM_SIZE);
  arr->len += 1;
}

void PaykanArray_push_obj(PaykanArray *arr, PaykanShared *value) {
  array_grow(arr);
  if (value)
    Paykan_retain(value);
  memcpy((char *)arr->data + arr->len * PAYKAN_ELEM_SIZE, &value,
         PAYKAN_ELEM_SIZE);
  arr->len += 1;
}

void *PaykanArray_pop(PaykanArray *arr) {
  if (arr->len == 0) {
    Paykan_runtime_panic("pop on empty array");
  }
  arr->len -= 1;
  void *val;
  memcpy(&val, (char *)arr->data + arr->len * PAYKAN_ELEM_SIZE,
         PAYKAN_ELEM_SIZE);
  array_maybe_shrink(arr);
  return val;
}

PaykanShared *PaykanArray_pop_obj(PaykanArray *arr) {
  if (arr->len == 0) {
    Paykan_runtime_panic("pop on empty array");
  }
  arr->len -= 1;
  PaykanShared *val;
  memcpy(&val, (char *)arr->data + arr->len * PAYKAN_ELEM_SIZE,
         PAYKAN_ELEM_SIZE);
  array_maybe_shrink(arr);
  return val;
}
