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
    fprintf(stderr, "paykan: array index %lu out of bounds (len=%lu)\n", idx,
            arr->len);
    abort();
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
    fprintf(stderr, "paykan: array index %lu out of bounds (len=%lu)\n", idx,
            arr->len);
    abort();
  }
  memcpy((char *)arr->data + idx * PAYKAN_ELEM_SIZE, &value, PAYKAN_ELEM_SIZE);
}

// Object set: release the old PaykanShared* in the slot (if any), retain the
// incoming one, then store it.
void PaykanArray_set_obj(PaykanArray *arr, unsigned long idx,
                         PaykanShared *value) {
  if (idx >= arr->len) {
    fprintf(stderr, "paykan: array index %lu out of bounds (len=%lu)\n", idx,
            arr->len);
    abort();
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
  // `other` arrives as a PaykanShared box and is consumed by this call.
  PaykanObject *o = PaykanShared_get((PaykanShared *)other);
  int64_t result = (self == o);
  Paykan_release((PaykanShared *)other);
  return result;
}

int64_t PaykanArray_length(PaykanObject *self) {
  return (int64_t)((PaykanArray *)self)->len;
}

// ============================================================================
// push / pop — dynamic resize operations
// ============================================================================
//
// Growth policy  : when len == cap, double capacity (minimum cap of 1).
// Shrink policy  : after pop, if len <= cap / 2, reallocate to len.
// ============================================================================

// Ensure there is room for at least one more element.
// On growth the new slots are zero-initialised.
static void array_grow(PaykanArray *arr) {
  if (arr->len < arr->cap)
    return; // still room
  unsigned long newCap = arr->cap == 0 ? 1 : arr->cap * 2;
  arr->data = Paykan_realloc(arr->data, newCap * PAYKAN_ELEM_SIZE);
  // Zero the freshly allocated slots.
  memset((char *)arr->data + arr->cap * PAYKAN_ELEM_SIZE, 0,
         (newCap - arr->cap) * PAYKAN_ELEM_SIZE);
  arr->cap = newCap;
}

// Shrink allocation to len when len has fallen to at most half of cap.
static void array_maybe_shrink(PaykanArray *arr) {
  if (arr->cap == 0 || arr->len > arr->cap / 2)
    return;
  if (arr->len == 0) {
    Paykan_free(arr->data);
    arr->data = NULL;
    arr->cap = 0;
  } else {
    arr->data = Paykan_realloc(arr->data, arr->len * PAYKAN_ELEM_SIZE);
    arr->cap = arr->len;
  }
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
    fprintf(stderr, "paykan: pop on empty array\n");
    abort();
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
    fprintf(stderr, "paykan: pop on empty array\n");
    abort();
  }
  arr->len -= 1;
  PaykanShared *val;
  memcpy(&val, (char *)arr->data + arr->len * PAYKAN_ELEM_SIZE,
         PAYKAN_ELEM_SIZE);
  // Clear the vacated slot before any potential realloc.
  memset((char *)arr->data + arr->len * PAYKAN_ELEM_SIZE, 0, PAYKAN_ELEM_SIZE);
  array_maybe_shrink(arr);
  return val;
}
