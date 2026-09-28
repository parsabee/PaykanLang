// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Shared (reference-counted) wrapper implementation.

#include "Runtime.h"

#include <stdlib.h>

// -- Constructor / acquire ---------------------------------------------------
//
// PaykanShared_new enforces the unique-box invariant (see Runtime.h): an
// object that already carries a box gets that SAME box back, retained — never
// a second, independently-counted box that would double-free the object.

PaykanShared *PaykanShared_new(PaykanObject *obj) {
  if (obj && obj->shared) {
    // Acquire: the object is already boxed — hand out another +1 reference to
    // its unique box.
    ++obj->shared->refCount;
    return obj->shared;
  }
  PaykanShared *s = (PaykanShared *)Paykan_malloc(sizeof(PaykanShared));
  s->refCount = 1;
  s->object = obj;
  if (obj)
    obj->shared = s; // install the unique-box backpointer
  return s;
}

// -- Reference counting ------------------------------------------------------

void Paykan_retain(PaykanShared *shared) {
  if (shared)
    ++shared->refCount;
}

void Paykan_release(PaykanShared *shared) {
  if (shared && --shared->refCount <= 0) {
    PaykanObject *obj = shared->object;
    if (obj) {
      // Break the object→box backpointer before destroy.  For ordinary
      // objects the memory is about to be freed anyway; for immortal statics
      // (None, Stdin) whose destroy is a no-op this returns them to the
      // unboxed state instead of leaving a dangling pointer to the freed box.
      if (obj->shared == shared)
        obj->shared = (PaykanShared *)0;
      obj->vtable->destroy(obj);
    }
    Paykan_free(shared);
  }
}

// -- Accessor ----------------------------------------------------------------

PaykanObject *PaykanShared_get(PaykanShared *shared) {
  return shared ? shared->object : (PaykanObject *)0;
}
