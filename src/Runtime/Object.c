// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Object base type implementation.

#include "Runtime.h"

#include <stdio.h>
#include <stdlib.h>

// -- VTable ------------------------------------------------------------------

PaykanObjectVTable PaykanObject_vtable = {
    .toString = PaykanObject_toString,
    .equals   = PaykanObject_equals,
};

// -- Constructor / Destructor ------------------------------------------------

PaykanObject *PaykanObject_new(void) {
  PaykanObject *obj = (PaykanObject *)malloc(sizeof(PaykanObject));
  obj->vtable = &PaykanObject_vtable;
  return obj;
}

void PaykanObject_delete(PaykanObject *self) {
  free(self);
}

// -- Default method implementations ------------------------------------------

PaykanString *PaykanObject_toString(PaykanObject *self) {
  // Default: "Object@<hex address>"
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "Object@%p", (void *)self);
  return PaykanString_new(buf, n);
}

int64_t PaykanObject_equals(PaykanObject *self, PaykanObject *other) {
  // Default: identity (pointer) equality.
  return self == other;
}
