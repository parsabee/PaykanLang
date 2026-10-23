// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — Object base type implementation, and the runtime panics.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// -- VTable

PaykanMethod PaykanObject_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanObject_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanObject_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
};

// -- Constructor / Destructor

PaykanObject *PaykanObject_new(void) {
  PaykanObject *obj = (PaykanObject *)Paykan_malloc(sizeof(PaykanObject));
  obj->vtable = PaykanObject_vtable;
  obj->strong = 0; // unowned until boxed
  obj->weak = 0;
  return obj;
}

// An Obj holds nothing: there is nothing to tear down (the runtime frees the
// memory after the destroy, Runtime.h "Shared").
void PaykanObject_destroy(PaykanObject *self) { (void)self; }

// -- Default methods

PaykanShared *PaykanObject_toString(PaykanObject *self) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "Object@%p", (void *)self);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanObject_equals(PaykanObject *self, PaykanShared *other) {
  // Identity, compared on the unboxed objects (`self` arrives unboxed); a
  // NULL box is equal to nothing.
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

// -- None singleton: an immortal Obj whose toString is "None".

static void PaykanNone_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanNone_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("None", 4));
}

static PaykanMethod PaykanNone_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanNone_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanNone_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
};

// Immortal: its strong count starts at 1, which nothing releases, so the
// boxings and releases of the singleton never destroy it.
PaykanObject PaykanObject_None = {PaykanNone_vtable, 1, 0};

// -- Runtime panics

void Paykan_runtime_panic(const char *fmt, ...) {
  va_list ap;
  fflush(NULL); // stdout, and every File the program is writing
  fputs("paykan: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  abort();
}

void Paykan_panic_message(PaykanObject *message) {
  const PaykanString *s = (const PaykanString *)message;
  Paykan_runtime_panic("%.*s", (int)s->len, s->data);
}

void Paykan_panic_value(void *t, void *out, PaykanObject *message) {
  (void)t;
  (void)out;
  Paykan_panic_message(message);
}

void *Paykan_unreachable_view(void *t, PaykanObject *owner) {
  (void)t;
  (void)owner;
  Paykan_runtime_panic("internal error: an unreachable 'view' was reached");
}

void Paykan_panic_div_by_zero(void) {
  Paykan_runtime_panic("integer division or modulo by zero");
}

void Paykan_panic_div_overflow(void) {
  Paykan_runtime_panic("integer overflow in division");
}

void Paykan_panic_for_step(void) {
  Paykan_runtime_panic("the step of a 'for' loop is 0");
}

void Paykan_panic_float_to_int(double value) {
  char buf[64];
  Paykan_format_float(buf, sizeof(buf), value);
  Paykan_runtime_panic(
      "int<float>(%s): the value is NaN, infinite or outside the int range",
      buf);
}

void Paykan_panic_float_to_type(double value, const char *target, int64_t len) {
  char buf[64];
  Paykan_format_float(buf, sizeof(buf), value);
  Paykan_runtime_panic(
      "%.*s<float>(%s): the value is NaN, infinite or outside the %.*s range",
      (int)len, target, buf, (int)len, target);
}

double Paykan_round_f16(double value) {
  if (isnan(value) || isinf(value) || value == 0.0)
    return value;
  double magnitude = fabs(value);
  // 65504 is the largest f16; from 65520 (half an ulp above it) a value
  // rounds to the next binade, which overflows.
  if (magnitude >= 65520.0)
    return value < 0 ? -INFINITY : INFINITY;
  // The quantum of the value's binade: 2^(e-10) for a value in [2^e,
  // 2^(e+1)); every subnormal shares 2^-24.
  int e;
  frexp(magnitude, &e); // magnitude = m * 2^e with m in [0.5, 1)
  int binade = e - 1;
  if (binade < -14)
    binade = -14;
  double quantum = ldexp(1.0, binade - 10);
  // The division is exact (a power of two), and nearbyint rounds to
  // nearest, ties to even, in the default rounding mode.
  double rounded = nearbyint(magnitude / quantum) * quantum;
  return copysign(rounded, value);
}

void Paykan_panic_index(int64_t index, int64_t len) {
  Paykan_runtime_panic("array index %lld out of bounds (len=%lld)",
                       (long long)index, (long long)len);
}

void Paykan_panic_int_to_char(int64_t value) {
  Paykan_runtime_panic(
      "char<int>(%lld): the value is outside the char range 0..255",
      (long long)value);
}
