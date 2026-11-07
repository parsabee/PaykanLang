// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The concurrency prototype's `atomic T` cell (docs/design/concurrency.md):
// one heap object holding 64 bits, an integer or a double's bits, that any
// thread reads and changes by atomic operations.  The prototype never
// counts a cell: it is immortal (its strong count is PAYKAN_REF_MAX, which
// retains and releases leave alone), so a handle crosses threads without an
// atomic count.  The design counts cells atomically and frees them; this
// does not.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

typedef struct PaykanAtomic {
  PaykanMethod *vtable; // points to PaykanAtomic_vtable
  int32_t strong;       // object header (see PaykanObject)
  int32_t weak;
  _Atomic int64_t value; // the integer, or the double's bits
  int64_t kind;          // PaykanAtomicKind
} PaykanAtomic;

static double bits_to_double(int64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof d);
  return d;
}

static int64_t double_to_bits(double d) {
  int64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return bits;
}

/// @p v rounded to the float type of @p kind (f32, f16), as the lowering
/// rounds every arithmetic result of that type.
static double round_to_kind(int64_t kind, double v) {
  if (kind == PAYKAN_ATOMIC_F32) {
    // Beyond the f32 maximum plus half an ulp a value rounds to infinity
    // (the C conversion of such a value is undefined).
    if (fabs(v) >= 3.4028235677973366e+38)
      return copysign(INFINITY, v);
    return (double)(float)v;
  }
  if (kind == PAYKAN_ATOMIC_F16)
    return Paykan_round_f16(v);
  return v;
}

static void PaykanAtomic_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanAtomic_toString(PaykanObject *self) {
  PaykanAtomic *c = (PaykanAtomic *)self;
  int64_t v = atomic_load(&c->value);
  PaykanString *s;
  switch (c->kind) {
  case PAYKAN_ATOMIC_UINT:
    s = PaykanString_from_uint(v);
    break;
  case PAYKAN_ATOMIC_BOOL:
    s = PaykanString_from_bool(v);
    break;
  case PAYKAN_ATOMIC_CHAR:
    s = PaykanString_from_char((int8_t)v);
    break;
  default:
    s = c->kind >= PAYKAN_ATOMIC_F64
            ? PaykanString_from_float(bits_to_double(v))
            : PaykanString_from_int(v);
    break;
  }
  return PaykanShared_new((PaykanObject *)s);
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
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanObject_walk,
};

static PaykanAtomic *PaykanAtomic_alloc(int64_t bits, int64_t kind) {
  PaykanAtomic *c = (PaykanAtomic *)Paykan_malloc(sizeof(PaykanAtomic));
  c->vtable = PaykanAtomic_vtable;
  c->strong = PAYKAN_REF_MAX; // immortal (see above)
  c->weak = 0;
  atomic_init(&c->value, bits);
  c->kind = kind;
  return c;
}

PaykanShared *PaykanAtomic_new(int64_t value, int64_t kind) {
  return (PaykanShared *)PaykanAtomic_alloc(value, kind);
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

int64_t PaykanAtomic_exchange(PaykanObject *cell, int64_t value) {
  return atomic_exchange(&((PaykanAtomic *)cell)->value, value);
}

int64_t PaykanAtomic_compare_exchange(PaykanObject *cell, int64_t expected,
                                      int64_t desired) {
  int64_t e = expected;
  return atomic_compare_exchange_strong(&((PaykanAtomic *)cell)->value, &e,
                                        desired)
             ? 1
             : 0;
}

// -- Floats: the cell holds the double's bits, rounded to the kind.

PaykanShared *PaykanAtomic_fnew(double value, int64_t kind) {
  return (PaykanShared *)PaykanAtomic_alloc(
      double_to_bits(round_to_kind(kind, value)), kind);
}

double PaykanAtomic_fload(PaykanObject *cell) {
  return bits_to_double(atomic_load(&((PaykanAtomic *)cell)->value));
}

void PaykanAtomic_fstore(PaykanObject *cell, double value) {
  PaykanAtomic *c = (PaykanAtomic *)cell;
  atomic_store(&c->value, double_to_bits(round_to_kind(c->kind, value)));
}

double PaykanAtomic_fadd(PaykanObject *cell, double delta) {
  PaykanAtomic *c = (PaykanAtomic *)cell;
  int64_t old = atomic_load(&c->value);
  for (;;) {
    const double sum = round_to_kind(c->kind, bits_to_double(old) + delta);
    if (atomic_compare_exchange_weak(&c->value, &old, double_to_bits(sum)))
      return bits_to_double(old);
    // `old` now holds the value found; try again on it.
  }
}

double PaykanAtomic_fexchange(PaykanObject *cell, double value) {
  PaykanAtomic *c = (PaykanAtomic *)cell;
  return bits_to_double(atomic_exchange(
      &c->value, double_to_bits(round_to_kind(c->kind, value))));
}

int64_t PaykanAtomic_fcompare_exchange(PaykanObject *cell, double expected,
                                       double desired) {
  PaykanAtomic *c = (PaykanAtomic *)cell;
  int64_t e = double_to_bits(expected);
  return atomic_compare_exchange_strong(
             &c->value, &e, double_to_bits(round_to_kind(c->kind, desired)))
             ? 1
             : 0;
}
