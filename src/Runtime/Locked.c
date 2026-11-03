// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's `locked T` cell (docs/design/concurrency.md,
// "locked and lock blocks"): one heap object holding an object's box behind
// a mutex, which a `lock` block takes for its length.  Immortal, as the
// atomic cell is (Atomic.c): never counted, never freed.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <pthread.h>

typedef struct PaykanLocked {
  PaykanMethod *vtable; // points to PaykanLocked_vtable
  int32_t strong;       // object header (see PaykanObject)
  int32_t weak;
  pthread_mutex_t mutex;
  PaykanShared *payload; // the contents, owned by the cell
} PaykanLocked;

static void PaykanLocked_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanLocked_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("locked", 6));
}

/// Two handles are equal when they name the same cell.
static int64_t PaykanLocked_equals(PaykanObject *self, PaykanShared *other,
                                   uint64_t owned) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_drop_other(other, owned, result);
}

PaykanMethod PaykanLocked_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanLocked_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanLocked_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanLocked_equals,
};

PaykanShared *PaykanLocked_new(PaykanShared *payload) {
  PaykanLocked *c = (PaykanLocked *)Paykan_malloc(sizeof(PaykanLocked));
  c->vtable = PaykanLocked_vtable;
  c->strong = PAYKAN_REF_MAX; // immortal (see above)
  c->weak = 0;
  if (pthread_mutex_init(&c->mutex, NULL) != 0)
    Paykan_runtime_panic("cannot create a lock");
  c->payload = payload;
  return (PaykanShared *)c;
}

void *PaykanLocked_lock(PaykanObject *cell) {
  PaykanLocked *c = (PaykanLocked *)cell;
  if (pthread_mutex_lock(&c->mutex) != 0)
    Paykan_runtime_panic("cannot take a lock");
  return &c->payload;
}

void PaykanLocked_unlock(PaykanObject *cell) {
  PaykanLocked *c = (PaykanLocked *)cell;
  if (pthread_mutex_unlock(&c->mutex) != 0)
    Paykan_runtime_panic("cannot release a lock");
}
