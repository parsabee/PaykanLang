// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — String type implementation.

#include "Runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
void PaykanString_destroy(PaykanObject *self);

// -- VTable ------------------------------------------------------------------

PaykanStringVTable PaykanString_vtable = {
    .destroy = PaykanString_destroy,
    .toString = PaykanString_toString,
    .equals = PaykanString_equals,
    .length = PaykanString_length,
    .concat = PaykanString_concat_inplace,
};

// -- Constructor / Destructor ------------------------------------------------

PaykanString *PaykanString_new(const char *data, int64_t len) {
  PaykanString *s = (PaykanString *)Paykan_malloc(sizeof(PaykanString));
  // Cast: the Object-compatible prefix of PaykanStringVTable matches
  // PaykanObjectVTable, so this pointer cast is safe.
  s->vtable = (PaykanObjectVTable *)&PaykanString_vtable;
  s->len = len;
  s->data = (char *)Paykan_malloc((size_t)len + 1);
  memcpy(s->data, data, (size_t)len);
  s->data[len] = '\0';
  return s;
}

PaykanString *PaykanString_from_int(int64_t value) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_float(double value) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", value);
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

int8_t PaykanString_char_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  if (idx < 0 || idx >= s->len) {
    fprintf(stderr, "paykan: string index %lld out of bounds (len=%lld)\n",
            (long long)idx, (long long)s->len);
    abort();
  }
  return (int8_t)s->data[idx];
}

void PaykanString_destroy(PaykanObject *self) {
  PaykanString *s = (PaykanString *)self;
  if (s) {
    Paykan_free(s->data);
    Paykan_free(s);
  }
}

// -- Method implementations --------------------------------------------------

PaykanShared *PaykanString_toString(PaykanObject *self) {
  // Return a fresh copy so the caller takes ownership of an independent
  // object.
  PaykanString *s = (PaykanString *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data, s->len));
}

int64_t PaykanString_equals(PaykanObject *self, PaykanObject *other) {
  // `other` arrives as a PaykanShared box and is consumed by this call.
  PaykanObject *o = PaykanShared_get((PaykanShared *)other);
  PaykanString *lhs = (PaykanString *)self;
  int64_t result;
  if (o->vtable != (PaykanObjectVTable *)&PaykanString_vtable) {
    // Not a String — fall back to identity.
    result = (self == o);
  } else {
    PaykanString *rhs = (PaykanString *)o;
    result = lhs->len == rhs->len &&
             memcmp(lhs->data, rhs->data, (size_t)lhs->len) == 0;
  }
  Paykan_release((PaykanShared *)other);
  return result;
}

int64_t PaykanString_length(PaykanObject *self) {
  return ((PaykanString *)self)->len;
}

PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  int64_t newLen = lhs->len + rhs->len;
  PaykanString *s = (PaykanString *)Paykan_malloc(sizeof(PaykanString));
  s->vtable = (PaykanObjectVTable *)&PaykanString_vtable;
  s->len = newLen;
  s->data = (char *)Paykan_malloc((size_t)newLen + 1);
  memcpy(s->data, lhs->data, (size_t)lhs->len);
  memcpy(s->data + lhs->len, rhs->data, (size_t)rhs->len);
  s->data[newLen] = '\0';
  return (PaykanObject *)s;
}

PaykanShared *PaykanString_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  if (idx < 0 || idx >= s->len) {
    fprintf(stderr, "paykan: string index %lld out of bounds (len=%lld)\n",
            (long long)idx, (long long)s->len);
    abort();
  }
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data + idx, 1));
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
