// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The `atomic T` cell (docs/design/concurrency.md, "`atomic`"): one heap
// object holding 64 bits, an integer or a double's bits, that any thread
// reads and changes by atomic operations.  Its count is atomic
// (PAYKAN_REF_ATOMIC + n, Runtime.h), so the last handle on any thread
// frees it.

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

/// @p v wrapped to the integer type of @p kind.
static int64_t wrap_to_kind(int64_t kind, int64_t v) {
  switch (kind) {
  case PAYKAN_ATOMIC_I8:
    return (int8_t)v;
  case PAYKAN_ATOMIC_I16:
    return (int16_t)v;
  case PAYKAN_ATOMIC_I32:
    return (int32_t)v;
  case PAYKAN_ATOMIC_U8:
    return (uint8_t)v;
  case PAYKAN_ATOMIC_U16:
    return (uint16_t)v;
  case PAYKAN_ATOMIC_U32:
    return (uint32_t)v;
  default:
    return v;
  }
}

/// An atomic add: a narrow integer's wraps in the cell, so the cell always
/// holds the value its type can (compareExchange compares bits).
static int64_t add_wrapping(_Atomic int64_t *at, int64_t kind, int64_t delta) {
  if (kind < PAYKAN_ATOMIC_I8 || kind > PAYKAN_ATOMIC_U32)
    return atomic_fetch_add(at, delta);
  int64_t old = atomic_load(at);
  while (!atomic_compare_exchange_weak(
      at, &old,
      wrap_to_kind(kind, (int64_t)((uint64_t)old + (uint64_t)delta)))) {
  }
  return old;
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
  c->strong = PAYKAN_REF_ATOMIC + 1;
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
  PaykanAtomic *c = (PaykanAtomic *)cell;
  return add_wrapping(&c->value, c->kind, delta);
}

int64_t PaykanAtomic_exchange(PaykanObject *cell, int64_t value) {
  return atomic_exchange(&((PaykanAtomic *)cell)->value, value);
}

int64_t PaykanAtomic_compare_exchange(PaykanObject *cell, int64_t *expected,
                                      int64_t desired) {
  return atomic_compare_exchange_strong(&((PaykanAtomic *)cell)->value,
                                        expected, desired)
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

int64_t PaykanAtomic_fcompare_exchange(PaykanObject *cell, double *expected,
                                       double desired) {
  PaykanAtomic *c = (PaykanAtomic *)cell;
  int64_t e = double_to_bits(*expected);
  if (atomic_compare_exchange_strong(
          &c->value, &e, double_to_bits(round_to_kind(c->kind, desired))))
    return 1;
  *expected = bits_to_double(e);
  return 0;
}

// -- `atomic T[n]`: one object of n atomic elements, used by element.

typedef struct PaykanAtomicArray {
  PaykanMethod *vtable; // points to PaykanAtomicArray_vtable
  int32_t strong;       // object header (see PaykanObject)
  int32_t weak;
  int64_t len;
  int64_t kind;           // PaykanAtomicKind
  _Atomic int64_t data[]; // the integers, or the doubles' bits
} PaykanAtomicArray;

static void PaykanAtomicArray_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanAtomicArray_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("atomic array", 12));
}

PaykanMethod PaykanAtomicArray_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanAtomicArray_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanAtomicArray_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanAtomic_equals,
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanObject_walk,
};

PaykanShared *PaykanAtomicArray_new(int64_t len, int64_t kind) {
  PaykanAtomicArray *a = (PaykanAtomicArray *)Paykan_malloc(
      sizeof(PaykanAtomicArray) + (size_t)len * sizeof(_Atomic int64_t));
  a->vtable = PaykanAtomicArray_vtable;
  a->strong = PAYKAN_REF_ATOMIC + 1;
  a->weak = 0;
  a->len = len;
  a->kind = kind;
  for (int64_t i = 0; i < len; ++i)
    atomic_init(&a->data[i], 0);
  return (PaykanShared *)a;
}

PaykanShared *PaykanAtomicArray_clone(PaykanObject *array) {
  PaykanAtomicArray *a = (PaykanAtomicArray *)array;
  PaykanShared *copy = PaykanAtomicArray_new(a->len, a->kind);
  PaykanAtomicArray *c = (PaykanAtomicArray *)copy;
  for (int64_t i = 0; i < a->len; ++i)
    atomic_store(&c->data[i], atomic_load(&a->data[i]));
  return copy;
}

static _Atomic int64_t *element(PaykanObject *array, int64_t i) {
  PaykanAtomicArray *a = (PaykanAtomicArray *)array;
  if (PAYKAN_UNLIKELY(i < 0 || i >= a->len))
    Paykan_runtime_panic("index %lld out of range for an atomic array of "
                         "%lld elements",
                         (long long)i, (long long)a->len);
  return &a->data[i];
}

int64_t PaykanAtomicArray_load(PaykanObject *array, int64_t i) {
  return atomic_load(element(array, i));
}

void PaykanAtomicArray_store(PaykanObject *array, int64_t i, int64_t value) {
  atomic_store(element(array, i), value);
}

int64_t PaykanAtomicArray_add(PaykanObject *array, int64_t i, int64_t delta) {
  return add_wrapping(element(array, i), ((PaykanAtomicArray *)array)->kind,
                      delta);
}

int64_t PaykanAtomicArray_exchange(PaykanObject *array, int64_t i,
                                   int64_t value) {
  return atomic_exchange(element(array, i), value);
}

int64_t PaykanAtomicArray_compare_exchange(PaykanObject *array, int64_t i,
                                           int64_t *expected, int64_t desired) {
  return atomic_compare_exchange_strong(element(array, i), expected, desired)
             ? 1
             : 0;
}

double PaykanAtomicArray_fload(PaykanObject *array, int64_t i) {
  return bits_to_double(atomic_load(element(array, i)));
}

void PaykanAtomicArray_fstore(PaykanObject *array, int64_t i, double value) {
  atomic_store(
      element(array, i),
      double_to_bits(round_to_kind(((PaykanAtomicArray *)array)->kind, value)));
}

double PaykanAtomicArray_fadd(PaykanObject *array, int64_t i, double delta) {
  _Atomic int64_t *at = element(array, i);
  const int64_t kind = ((PaykanAtomicArray *)array)->kind;
  int64_t old = atomic_load(at);
  for (;;) {
    const double sum = round_to_kind(kind, bits_to_double(old) + delta);
    if (atomic_compare_exchange_weak(at, &old, double_to_bits(sum)))
      return bits_to_double(old);
  }
}

double PaykanAtomicArray_fexchange(PaykanObject *array, int64_t i,
                                   double value) {
  return bits_to_double(atomic_exchange(
      element(array, i), double_to_bits(round_to_kind(
                             ((PaykanAtomicArray *)array)->kind, value))));
}

int64_t PaykanAtomicArray_fcompare_exchange(PaykanObject *array, int64_t i,
                                            double *expected, double desired) {
  int64_t e = double_to_bits(*expected);
  if (atomic_compare_exchange_strong(
          element(array, i), &e,
          double_to_bits(
              round_to_kind(((PaykanAtomicArray *)array)->kind, desired))))
    return 1;
  *expected = bits_to_double(e);
  return 0;
}
