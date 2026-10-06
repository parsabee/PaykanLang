// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Object base type implementation, and the runtime panics.

#include "Runtime.h"
#include "RuntimeInternal.h"

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
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  return obj;
}

void PaykanObject_destroy(PaykanObject *self) { Paykan_free(self); }

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

// Boxing None installs a box and Paykan_release clears it again (destroy is
// a no-op), so the singleton cycles cleanly through boxings.
PaykanObject PaykanObject_None = {PaykanNone_vtable, NULL};

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

void Paykan_panic_div_by_zero(void) {
  Paykan_runtime_panic("integer division or modulo by zero");
}

void Paykan_panic_div_overflow(void) {
  Paykan_runtime_panic("integer overflow in division");
}

void Paykan_panic_float_to_int(double value) {
  char buf[64];
  Paykan_format_float(buf, sizeof(buf), value);
  Paykan_runtime_panic(
      "int<float>(%s): the value is NaN, infinite or outside the int range",
      buf);
}

void Paykan_panic_int_to_char(int64_t value) {
  Paykan_runtime_panic(
      "char<int>(%lld): the value is outside the char range 0..255",
      (long long)value);
}
