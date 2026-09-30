// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Object base type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <stdlib.h>

// -- VTable ------------------------------------------------------------------

PaykanObjectVTable PaykanObject_vtable = {
    .destroy = PaykanObject_destroy,
    .toString = PaykanObject_toString,
    .equals = PaykanObject_equals,
};

// -- Constructor / Destructor ------------------------------------------------

PaykanObject *PaykanObject_new(void) {
  PaykanObject *obj = (PaykanObject *)Paykan_malloc(sizeof(PaykanObject));
  obj->vtable = &PaykanObject_vtable;
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  return obj;
}

void PaykanObject_destroy(PaykanObject *self) { Paykan_free(self); }

// -- Default method implementations ------------------------------------------

PaykanShared *PaykanObject_toString(PaykanObject *self) {
  // Default: "Object@<hex address>"
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "Object@%p", (void *)self);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanObject_equals(PaykanObject *self, PaykanObject *other) {
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h);
  // unbox so identity compares the underlying objects, matching the unboxed
  // `self`.  A NULL box compares unequal.
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

// -- None singleton ----------------------------------------------------------
//
// None is a shared immortal Obj instance.  Its toString returns "None" and
// its equals always returns 0 (None is only equal to itself via identity).

static void PaykanNone_destroy(PaykanObject *self) {
  (void)self; // immortal — never freed
}

static PaykanShared *PaykanNone_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("None", 4));
}

static int64_t PaykanNone_equals(PaykanObject *self, PaykanObject *other) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

static PaykanObjectVTable PaykanNone_vtable = {
    .destroy = PaykanNone_destroy,
    .toString = PaykanNone_toString,
    .equals = PaykanNone_equals,
};

// shared starts NULL; boxing None installs a box and Paykan_release clears it
// again (destroy is a no-op), so the singleton cycles cleanly through boxings.
PaykanObject PaykanObject_None = {&PaykanNone_vtable, NULL};

// -- Runtime panics ----------------------------------------------------------

void Paykan_panic_div_by_zero(void) {
  fprintf(stderr, "paykan: integer division or modulo by zero\n");
  abort();
}

void Paykan_panic_div_overflow(void) {
  fprintf(stderr, "paykan: integer overflow in division\n");
  abort();
}
