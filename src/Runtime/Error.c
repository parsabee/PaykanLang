// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Error type implementation: what open() returns when
// fopen fails; toString is the message.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdlib.h>

PaykanMethod PaykanError_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanError_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanError_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanError_equals,
};

PaykanError *PaykanError_new(const char *msg, int64_t len) {
  PaykanError *e = (PaykanError *)Paykan_malloc(sizeof(PaykanError));
  e->vtable = PaykanError_vtable;
  e->shared = NULL; // not yet boxed (unique-box invariant)
  e->message = PaykanString_new(msg, len);
  return e;
}

void PaykanError_destroy(PaykanObject *self) {
  PaykanError *e = (PaykanError *)self;
  PaykanString_destroy((PaykanObject *)e->message);
  Paykan_free(e);
}

PaykanShared *PaykanError_toString(PaykanObject *self) {
  PaykanError *e = (PaykanError *)self;
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(e->message->data, e->message->len));
}

int64_t PaykanError_equals(PaykanObject *self, PaykanShared *other) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}
