// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Shared (reference-counted) wrapper implementation.

#include "Runtime.h"

#include <stdlib.h>

// -- Constructor -------------------------------------------------------------

PaykanShared *PaykanShared_new(PaykanObject *obj,
                               void (*destroyFn)(PaykanObject *)) {
  PaykanShared *s = (PaykanShared *)malloc(sizeof(PaykanShared));
  s->refCount = 1;
  s->object   = obj;
  s->destroy  = destroyFn;
  return s;
}

// -- Reference counting ------------------------------------------------------

void Paykan_retain(PaykanShared *shared) {
  if (shared)
    ++shared->refCount;
}

void Paykan_release(PaykanShared *shared) {
  if (shared && --shared->refCount <= 0) {
    if (shared->destroy && shared->object)
      shared->destroy(shared->object);
    free(shared);
  }
}

// -- Accessor ----------------------------------------------------------------

PaykanObject *PaykanShared_get(PaykanShared *shared) {
  return shared ? shared->object : (PaykanObject *)0;
}
