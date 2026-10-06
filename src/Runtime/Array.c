// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Array type implementation.
//
// Two variants share the PaykanArray layout and differ in their vtable: a
// primitive array (int / float / bool elements, PaykanArray_new,
// PaykanArray_vtable) stores raw 8-byte values, an object array
// (PaykanArray_new_obj, PaykanArray_obj_vtable) stores PaykanShared* that
// set_obj / push_obj retain and destroy_obj releases.  Reads are the same
// plain 8-byte copy for both.  The compiler picks the variant from the
// static element type.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAYKAN_ELEM_SIZE 8UL

// -- VTables

PaykanMethod PaykanArray_vtable[PAYKAN_ARRAY_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanArray_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanArray_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanArray_equals,
    [PAYKAN_SLOT_ARRAY_LENGTH] = (PaykanMethod)PaykanArray_length,
};

PaykanMethod PaykanArray_obj_vtable[PAYKAN_ARRAY_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanArray_destroy_obj,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanArray_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanArray_equals,
    [PAYKAN_SLOT_ARRAY_LENGTH] = (PaykanMethod)PaykanArray_length,
};

// -- Constructors

/// An array of @p len elements copied from @p data, or zeroed when @p data
/// is NULL (so every object slot starts as a NULL box).
static PaykanArray *array_alloc(PaykanMethod *vtable, unsigned long len,
                                const void *data) {
  PaykanArray *arr = (PaykanArray *)Paykan_malloc(sizeof(PaykanArray));
  arr->vtable = vtable;
  arr->shared = NULL; // not yet boxed (unique-box invariant)
  arr->len = len;
  arr->cap = len;
  arr->data = NULL;
  if (len > 0) {
    arr->data = Paykan_malloc(len * PAYKAN_ELEM_SIZE);
    if (data)
      memcpy(arr->data, data, len * PAYKAN_ELEM_SIZE);
    else
      memset(arr->data, 0, len * PAYKAN_ELEM_SIZE);
  }
  return arr;
}

PaykanArray *PaykanArray_new(unsigned long len) {
  return array_alloc(PaykanArray_vtable, len, NULL);
}

PaykanArray *PaykanArray_new_from_data(unsigned long len, const void *data) {
  return array_alloc(PaykanArray_vtable, len, data);
}

PaykanArray *PaykanArray_new_obj(unsigned long len) {
  return array_alloc(PaykanArray_obj_vtable, len, NULL);
}

// -- Element access

static void *array_slot(const PaykanArray *arr, unsigned long idx) {
  return (char *)arr->data + idx * PAYKAN_ELEM_SIZE;
}

static void array_check_index(const PaykanArray *arr, unsigned long idx) {
  if (idx >= arr->len) {
    Paykan_runtime_panic("array index %lu out of bounds (len=%lu)", idx,
                         arr->len);
  }
}

void *PaykanArray_get(PaykanArray *arr, unsigned long idx) {
  array_check_index(arr, idx);
  void *val;
  memcpy(&val, array_slot(arr, idx), PAYKAN_ELEM_SIZE);
  return val;
}

void PaykanArray_set(PaykanArray *arr, unsigned long idx, void *value) {
  array_check_index(arr, idx);
  memcpy(array_slot(arr, idx), &value, PAYKAN_ELEM_SIZE);
}

void PaykanArray_set_obj(PaykanArray *arr, unsigned long idx,
                         PaykanShared *value) {
  array_check_index(arr, idx);
  void *slot = array_slot(arr, idx);
  PaykanShared *old;
  memcpy(&old, slot, PAYKAN_ELEM_SIZE);
  if (old)
    Paykan_release(old);
  if (value)
    Paykan_retain(value);
  memcpy(slot, &value, PAYKAN_ELEM_SIZE);
}

// -- Methods

void PaykanArray_destroy(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  Paykan_free(arr->data);
  Paykan_free(arr);
}

void PaykanArray_destroy_obj(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  if (arr->data) {
    for (unsigned long i = 0; i < arr->len; ++i) {
      PaykanShared *elem;
      memcpy(&elem, array_slot(arr, i), PAYKAN_ELEM_SIZE);
      if (elem)
        Paykan_release(elem);
    }
  }
  Paykan_free(arr->data);
  Paykan_free(arr);
}

PaykanShared *PaykanArray_toString(PaykanObject *self) {
  PaykanArray *arr = (PaykanArray *)self;
  char buf[80];
  int n = snprintf(buf, sizeof(buf), "Array@%p[len=%lu]", (void *)arr,
                   (unsigned long)arr->len);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanArray_equals(PaykanObject *self, PaykanShared *other) {
  // Identity: two arrays are equal only when they are the same object.
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

int64_t PaykanArray_length(PaykanObject *self) {
  return (int64_t)((PaykanArray *)self)->len;
}

// -- push / pop
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

#define PAYKAN_ARRAY_MIN_CAP 8UL

static void array_resize(PaykanArray *arr, unsigned long newCap) {
  arr->data = Paykan_realloc(arr->data, newCap * PAYKAN_ELEM_SIZE);
  if (!arr->data)
    Paykan_runtime_panic("out of memory resizing array to %lu elements",
                         newCap);
  arr->cap = newCap;
}

/// Make room for one more element.
static void array_grow(PaykanArray *arr) {
  if (arr->len < arr->cap)
    return;
  unsigned long newCap = arr->cap * 2;
  if (newCap < PAYKAN_ARRAY_MIN_CAP)
    newCap = PAYKAN_ARRAY_MIN_CAP;
  array_resize(arr, newCap);
}

/// Halve the allocation once len has fallen to a quarter of cap.
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
  memcpy(array_slot(arr, arr->len), &value, PAYKAN_ELEM_SIZE);
  arr->len += 1;
}

void PaykanArray_push_obj(PaykanArray *arr, PaykanShared *value) {
  if (value)
    Paykan_retain(value);
  PaykanArray_push(arr, value);
}

void *PaykanArray_pop(PaykanArray *arr) {
  if (arr->len == 0) {
    Paykan_runtime_panic("pop on empty array");
  }
  arr->len -= 1;
  void *val;
  memcpy(&val, array_slot(arr, arr->len), PAYKAN_ELEM_SIZE);
  array_maybe_shrink(arr);
  return val;
}

PaykanShared *PaykanArray_pop_obj(PaykanArray *arr) {
  return (PaykanShared *)PaykanArray_pop(arr);
}
