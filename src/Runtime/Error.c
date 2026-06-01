// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Error type implementation.
//
// Error is a builtin class that inherits Obj.  It is returned by open()
// when the underlying fopen call fails.  Its toString returns the
// human-readable error message.

#include "Runtime.h"

#include <stdlib.h>

// ============================================================================
// VTable
// ============================================================================

PaykanObjectVTable PaykanError_vtable = {
    .destroy  = PaykanError_destroy,
    .toString = PaykanError_toString,
    .equals   = PaykanError_equals,
};

// ============================================================================
// Constructor
// ============================================================================

PaykanError *PaykanError_new(const char *msg, int64_t len) {
  PaykanError *e = (PaykanError *)malloc(sizeof(PaykanError));
  e->vtable  = &PaykanError_vtable;
  e->message = PaykanString_new(msg, len);
  return e;
}

// ============================================================================
// Method implementations
// ============================================================================

void PaykanError_destroy(PaykanObject *self) {
  PaykanError *e = (PaykanError *)self;
  PaykanString_destroy((PaykanObject *)e->message);
  free(e);
}

PaykanShared *PaykanError_toString(PaykanObject *self) {
  PaykanError *e = (PaykanError *)self;
  // Return a new shared wrapper around a copy of the message string.
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(e->message->data, e->message->len));
}

int64_t PaykanError_equals(PaykanObject *self, PaykanObject *other) {
  return self == other;
}
