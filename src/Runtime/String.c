// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — String type implementation.

#include "Runtime.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// -- VTable ------------------------------------------------------------------

PaykanStringVTable PaykanString_vtable = {
    .toString = PaykanString_toString,
    .equals   = PaykanString_equals,
    .length   = PaykanString_length,
    .concat   = PaykanString_concat,
};

// -- Constructor / Destructor ------------------------------------------------

PaykanString *PaykanString_new(const char *data, int64_t len) {
  PaykanString *s = (PaykanString *)malloc(sizeof(PaykanString));
  // Cast: the Object-compatible prefix of PaykanStringVTable matches
  // PaykanObjectVTable, so this pointer cast is safe.
  s->vtable = (PaykanObjectVTable *)&PaykanString_vtable;
  s->len    = len;
  s->data   = (char *)malloc((size_t)len + 1);
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
  if (value) return PaykanString_new("True", 4);
  return PaykanString_new("False", 5);
}

void PaykanString_delete(PaykanString *self) {
  if (self) {
    free(self->data);
    free(self);
  }
}

// -- Method implementations --------------------------------------------------

PaykanString *PaykanString_toString(PaykanObject *self) {
  // String's toString returns itself.
  return (PaykanString *)self;
}

int64_t PaykanString_equals(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  // If the other object is not a String, fall back to identity.
  if (other->vtable != (PaykanObjectVTable *)&PaykanString_vtable)
    return self == other;
  PaykanString *rhs = (PaykanString *)other;
  if (lhs->len != rhs->len)
    return 0;
  return memcmp(lhs->data, rhs->data, (size_t)lhs->len) == 0;
}

int64_t PaykanString_length(PaykanObject *self) {
  return ((PaykanString *)self)->len;
}

PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  int64_t newLen = lhs->len + rhs->len;
  char *buf = (char *)malloc((size_t)newLen + 1);
  memcpy(buf, lhs->data, (size_t)lhs->len);
  memcpy(buf + lhs->len, rhs->data, (size_t)rhs->len);
  buf[newLen] = '\0';
  PaykanString *result = PaykanString_new(buf, newLen);
  free(buf);
  return (PaykanObject *)result;
}
