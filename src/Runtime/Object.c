// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Object base type implementation.

#include "Runtime.h"

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
  // Default: identity (pointer) equality.
  return self == other;
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
  return self == other;
}

static PaykanObjectVTable PaykanNone_vtable = {
    .destroy = PaykanNone_destroy,
    .toString = PaykanNone_toString,
    .equals = PaykanNone_equals,
};

PaykanObject PaykanObject_None = {&PaykanNone_vtable};
