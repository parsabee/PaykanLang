// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's `atomic int` cell (docs/design/concurrency.md):
// one heap object holding an int64 that any thread reads and changes by
// atomic operations.  The prototype never counts a cell: it is immortal
// (its strong count is PAYKAN_REF_MAX, which retains and releases leave
// alone), so a handle crosses threads without an atomic count.  The design
// counts cells atomically and frees them; this does not.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdatomic.h>

typedef struct PaykanAtomic {
  PaykanMethod *vtable; // points to PaykanAtomic_vtable
  int32_t strong;       // object header (see PaykanObject)
  int32_t weak;
  _Atomic int64_t value;
} PaykanAtomic;

static void PaykanAtomic_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanAtomic_toString(PaykanObject *self) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_from_int(PaykanAtomic_load(self)));
}

/// Two handles are equal when they name the same cell.
static int64_t PaykanAtomic_equals(PaykanObject *self, PaykanShared *other,
                                   uint64_t owned) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_drop_other(other, owned, result);
}

PaykanMethod PaykanAtomic_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanAtomic_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanAtomic_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanAtomic_equals,
};

PaykanShared *PaykanAtomic_new(int64_t value) {
  PaykanAtomic *c = (PaykanAtomic *)Paykan_malloc(sizeof(PaykanAtomic));
  c->vtable = PaykanAtomic_vtable;
  c->strong = PAYKAN_REF_MAX; // immortal (see above)
  c->weak = 0;
  atomic_init(&c->value, value);
  return (PaykanShared *)c;
}

int64_t PaykanAtomic_load(PaykanObject *cell) {
  return atomic_load(&((PaykanAtomic *)cell)->value);
}

void PaykanAtomic_store(PaykanObject *cell, int64_t value) {
  atomic_store(&((PaykanAtomic *)cell)->value, value);
}

int64_t PaykanAtomic_add(PaykanObject *cell, int64_t delta) {
  return atomic_fetch_add(&((PaykanAtomic *)cell)->value, delta);
}
