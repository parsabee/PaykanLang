// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — String type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- VTable

PaykanMethod PaykanString_vtable[PAYKAN_STRING_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanString_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanString_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanString_equals,
    [PAYKAN_SLOT_STRING_LENGTH] = (PaykanMethod)PaykanString_length,
    [PAYKAN_SLOT_STRING_CONCAT] = (PaykanMethod)PaykanString_concat_inplace,
};

// -- Constructors

/// A string of @p len bytes with an uninitialised, NUL-terminated buffer.
static PaykanString *string_alloc(int64_t len) {
  PaykanString *s = (PaykanString *)Paykan_malloc(sizeof(PaykanString));
  s->vtable = PaykanString_vtable;
  s->shared = NULL; // not yet boxed (unique-box invariant)
  s->len = len;
  s->data = (char *)Paykan_malloc((size_t)len + 1);
  s->data[len] = '\0';
  return s;
}

PaykanString *PaykanString_new(const char *data, int64_t len) {
  PaykanString *s = string_alloc(len);
  memcpy(s->data, data, (size_t)len);
  return s;
}

PaykanString *PaykanString_from_int(int64_t value) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_float(double value) {
  char buf[64];
  int n = Paykan_format_float(buf, sizeof(buf), value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_bool(int64_t value) {
  if (value)
    return PaykanString_new("True", 4);
  return PaykanString_new("False", 5);
}

PaykanString *PaykanString_from_char(int8_t c) {
  return PaykanString_new((const char *)&c, 1);
}

// -- Methods

static void string_check_index(const PaykanString *s, int64_t idx) {
  if (idx < 0 || idx >= s->len) {
    Paykan_runtime_panic("string index %lld out of bounds (len=%lld)",
                         (long long)idx, (long long)s->len);
  }
}

int8_t PaykanString_char_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  string_check_index(s, idx);
  return (int8_t)s->data[idx];
}

PaykanShared *PaykanString_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  string_check_index(s, idx);
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data + idx, 1));
}

void PaykanString_destroy(PaykanObject *self) {
  PaykanString *s = (PaykanString *)self;
  if (s) {
    Paykan_free(s->data);
    Paykan_free(s);
  }
}

PaykanShared *PaykanString_toString(PaykanObject *self) {
  // A fresh copy, so the caller owns an independent object.
  PaykanString *s = (PaykanString *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data, s->len));
}

int64_t PaykanString_equals(PaykanObject *self, PaykanShared *other) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  PaykanString *lhs = (PaykanString *)self;
  int64_t result;
  if (!o) {
    result = 0; // NULL box: equal to nothing
  } else if (o->vtable != PaykanString_vtable) {
    result = (self == o); // not a String: identity
  } else {
    PaykanString *rhs = (PaykanString *)o;
    result = lhs->len == rhs->len &&
             memcmp(lhs->data, rhs->data, (size_t)lhs->len) == 0;
  }
  return Paykan_equals_consume_other(other, result);
}

int64_t PaykanString_length(PaykanObject *self) {
  return ((PaykanString *)self)->len;
}

PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  PaykanString *s = string_alloc(lhs->len + rhs->len);
  memcpy(s->data, lhs->data, (size_t)lhs->len);
  memcpy(s->data + lhs->len, rhs->data, (size_t)rhs->len);
  return (PaykanObject *)s;
}

void PaykanString_concat_inplace(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  int64_t newLen = lhs->len + rhs->len;
  char *newData = (char *)Paykan_malloc((size_t)newLen + 1);
  memcpy(newData, lhs->data, (size_t)lhs->len);
  memcpy(newData + lhs->len, rhs->data, (size_t)rhs->len);
  newData[newLen] = '\0';
  Paykan_free(lhs->data);
  lhs->data = newData;
  lhs->len = newLen;
}
