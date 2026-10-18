// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — String type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
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
  s->strong = 0; // unowned until boxed
  s->weak = 0;
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

PaykanString *PaykanString_from_uint(int64_t value) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
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

// -- Parses (`int<Str>`, `float<Str>`, `bool<Str>`)

/// True when @p s starts with a character strtoll / strtod would skip as
/// leading whitespace, which `int<Str>` / `float<Str>` reject.
static int string_starts_with_space(const PaykanString *s) {
  return s->len > 0 && isspace((unsigned char)s->data[0]);
}

int64_t PaykanString_parse_int(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0;
  if (s->len == 0 || string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  long long val = strtoll(s->data, &end, 10);
  // The whole string must be consumed (an embedded NUL ends strtoll early)
  // and the value must fit (ERANGE).
  if (end != s->data + s->len || errno != 0)
    return 0;
  *out = (int64_t)val;
  return 1;
}

int64_t PaykanString_parse_uint(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0;
  // strtoull accepts a sign and negates: only digits are an unsigned number.
  if (s->len == 0 || s->data[0] == '-' || s->data[0] == '+' ||
      string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  unsigned long long val = strtoull(s->data, &end, 10);
  if (end != s->data + s->len || errno != 0)
    return 0;
  *out = (int64_t)val;
  return 1;
}

int64_t PaykanString_parse_float(PaykanObject *str, double *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0.0;
  if (s->len == 0 || string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  double val = strtod(s->data, &end);
  if (end != s->data + s->len)
    return 0;
  // ERANGE flags overflow (±HUGE_VAL), a nonzero value underflowing to 0, and
  // also a subnormal result, which is representable and accepted.
  if (errno != 0 && (errno != ERANGE || isinf(val) || val == 0.0))
    return 0;
  *out = val;
  return 1;
}

int64_t PaykanString_parse_bool(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  // Exactly the spellings `Str<bool>` prints, so the two round-trip.
  *out = s->len == 4 && memcmp(s->data, "True", 4) == 0;
  return *out || (s->len == 5 && memcmp(s->data, "False", 5) == 0);
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

// The runtime frees the object itself after the destroy (Runtime.h,
// "Shared"); a string made and dropped unboxed goes through Paykan_dispose.
void PaykanString_destroy(PaykanObject *self) {
  PaykanString *s = (PaykanString *)self;
  if (s)
    Paykan_free(s->data);
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
