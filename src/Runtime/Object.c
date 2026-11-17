// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — Object base type implementation, and the runtime panics.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// -- VTable

PaykanMethod PaykanObject_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanObject_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanObject_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanObject_walk,
};

// -- The walk and the freeze (the concurrency prototype)

void PaykanObject_walk(PaykanObject *self) { (void)self; }

// The one walker: what a value holds uniquely, by each object's walk slot,
// and stopping at every other kind of link.  It freezes a value or checks that
// a value moved out of borrowed storage is held by nothing else (#368;
// docs/design/concurrency.md, "Owned paths and borrowed roots"). The objects to
// visit are kept on a stack of the thread's own: an inline array, then the C
// heap (never Paykan_malloc, so --track-heap counts are unaffected), so a long
// chain of unique objects cannot overflow the C stack.
enum { kWalkNone, kWalkCheck, kWalkFreeze };
#define PAYKAN_WALK_INLINE 32
/// A walk's state, the thread's own.  Reaching a thread's variable may be
/// a call (macOS), so it is one variable, whose address each function here
/// takes once.
typedef struct PaykanWalk {
  int mode;
  int failed;          // something else holds a part of it
  size_t len;          // the objects still to visit
  size_t cap;          // of `heap`
  PaykanObject **heap; // past `first`'s room
  PaykanObject *first[PAYKAN_WALK_INLINE];
} PaykanWalk;
static _Thread_local PaykanWalk g_walk;

static void walk_push(PaykanWalk *w, PaykanObject *obj) {
  if (w->len < PAYKAN_WALK_INLINE) {
    w->first[w->len++] = obj;
    return;
  }
  const size_t past = w->len - PAYKAN_WALK_INLINE;
  if (past == w->cap) {
    size_t cap = w->cap ? w->cap * 2 : 64;
    PaykanObject **grown =
        (PaykanObject **)realloc(w->heap, cap * sizeof(PaykanObject *));
    if (!grown)
      Paykan_runtime_panic("out of memory walking a value");
    w->heap = grown;
    w->cap = cap;
  }
  w->heap[past] = obj;
  ++w->len;
}

static void walk_visit(PaykanWalk *w, PaykanObject *obj) {
  if (!obj)
    return;
  const int32_t s = obj->strong;
  if (s >= PAYKAN_REF_ATOMIC)
    return; // a handle, another frozen value, or immortal
  if (s > 1 || (w->mode == kWalkFreeze && obj->weak != 0)) {
    w->failed = 1;
    return;
  }
  if (w->mode == kWalkFreeze)
    obj->strong = PAYKAN_REF_ATOMIC + 1;
  walk_push(w, obj);
}

/// Walks the stack empty, or until something else is found holding a part
/// of the value.  Whether nothing was.
static int64_t walk_drain(PaykanWalk *w) {
  while (w->len && !w->failed) {
    --w->len;
    Paykan_vcall_walk(w->len < PAYKAN_WALK_INLINE
                          ? w->first[w->len]
                          : w->heap[w->len - PAYKAN_WALK_INLINE]);
  }
  w->len = 0;
  w->mode = kWalkNone;
  if (w->heap) {
    free(w->heap); // a long chain's peak is not kept for the thread's life
    w->heap = NULL;
    w->cap = 0;
  }
  return !w->failed;
}

void Paykan_walk_child(PaykanObject *obj, int64_t unique) {
  if (!unique || !obj)
    return;
  PaykanWalk *w = &g_walk;
  if (w->mode != kWalkNone)
    walk_visit(w, obj);
}

void Paykan_freeze(PaykanObject *obj) {
  if (!obj || obj->strong >= PAYKAN_REF_ATOMIC)
    return;
  if (obj->strong != 1 || obj->weak != 0)
    Paykan_runtime_panic("a value made 'atomic shared' is still borrowed");
  PaykanWalk *w = &g_walk;
  w->mode = kWalkFreeze;
  w->failed = 0;
  Paykan_vcall_walk(obj);
  if (!walk_drain(w))
    Paykan_runtime_panic("a value made 'atomic shared' is still borrowed");
  obj->strong = PAYKAN_REF_ATOMIC + 1;
}

void Paykan_freeze_mem(const void *type, void *value) {
  const PaykanType *t = (const PaykanType *)type;
  PaykanWalk *w = &g_walk;
  for (int64_t k = 0; k < t->nrefs; ++k) {
    const int64_t word = PaykanType_ref(t, k);
    PaykanShared *ref =
        *(PaykanShared **)((unsigned char *)value + PaykanType_refOffset(word));
    if (w->mode == kWalkNone)
      Paykan_freeze(ref);
    else if (!(word & 2))
      walk_visit(w, ref);
  }
}

int64_t Paykan_moved_unique(PaykanShared *box) {
  PaykanWalk *w = &g_walk;
  w->mode = kWalkCheck;
  w->failed = 0;
  walk_visit(w, (PaykanObject *)box);
  return walk_drain(w);
}

int64_t Paykan_moved_unique_mem(const void *type, void *value) {
  PaykanWalk *w = &g_walk;
  w->mode = kWalkCheck;
  w->failed = 0;
  Paykan_freeze_mem(type, value);
  return walk_drain(w);
}

// -- Constructor / Destructor

PaykanObject *PaykanObject_new(void) {
  PaykanObject *obj = (PaykanObject *)Paykan_malloc(sizeof(PaykanObject));
  obj->vtable = PaykanObject_vtable;
  obj->strong = 0; // unowned until boxed
  obj->weak = 0;
  return obj;
}

// An Obj holds nothing: there is nothing to tear down (the runtime frees the
// memory after the destroy, Runtime.h "Shared").
void PaykanObject_destroy(PaykanObject *self) { (void)self; }

// -- Default methods

PaykanShared *PaykanObject_toString(PaykanObject *self) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "Object@%p", (void *)self);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanObject_equals(PaykanObject *self, PaykanShared *other,
                            uint64_t owned) {
  // Identity, compared on the unboxed objects (`self` arrives unboxed); a
  // NULL box is equal to nothing.
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_drop_other(other, owned, result);
}

// -- Runtime panics

void Paykan_runtime_panic(const char *fmt, ...) {
  va_list ap;
  fflush(NULL); // stdout, and every File the program is writing
  fputs("paykan: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  abort();
}

void Paykan_panic_moved(const char *place) {
  Paykan_runtime_panic("a value moved out of '%s' is still borrowed", place);
}

void Paykan_panic_message(PaykanObject *message) {
  const PaykanString *s = (const PaykanString *)message;
  Paykan_runtime_panic("%.*s", (int)s->len, s->data);
}

void Paykan_panic_value(void *t, void *out, PaykanObject *message) {
  (void)t;
  (void)out;
  Paykan_panic_message(message);
}

void *Paykan_unreachable_view(void *t, PaykanObject *owner) {
  (void)t;
  (void)owner;
  Paykan_runtime_panic("internal error: an unreachable 'view' was reached");
}

void Paykan_panic_div_by_zero(void) {
  Paykan_runtime_panic("integer division or modulo by zero");
}

void Paykan_panic_div_overflow(void) {
  Paykan_runtime_panic("integer overflow in division");
}

void Paykan_panic_for_step(void) {
  Paykan_runtime_panic("the step of a 'for' loop is 0");
}

void Paykan_panic_pure_virtual(const char *cls, int64_t cls_len,
                               const char *method, int64_t method_len) {
  Paykan_runtime_panic("call of the pure virtual method '%.*s.%.*s', which has "
                       "no implementation",
                       (int)cls_len, cls, (int)method_len, method);
}

void Paykan_panic_float_to_int(double value) {
  char buf[64];
  Paykan_format_float(buf, sizeof(buf), value);
  Paykan_runtime_panic(
      "int<float>(%s): the value is NaN, infinite or outside the int range",
      buf);
}

void Paykan_panic_float_to_type(double value, const char *target, int64_t len) {
  char buf[64];
  Paykan_format_float(buf, sizeof(buf), value);
  Paykan_runtime_panic(
      "%.*s<float>(%s): the value is NaN, infinite or outside the %.*s range",
      (int)len, target, buf, (int)len, target);
}

double Paykan_round_f16(double value) {
  if (isnan(value) || isinf(value) || value == 0.0)
    return value;
  double magnitude = fabs(value);
  // 65504 is the largest f16; from 65520 (half an ulp above it) a value
  // rounds to the next binade, which overflows.
  if (magnitude >= 65520.0)
    return value < 0 ? -INFINITY : INFINITY;
  // The quantum of the value's binade: 2^(e-10) for a value in [2^e,
  // 2^(e+1)); every subnormal shares 2^-24.
  int e;
  frexp(magnitude, &e); // magnitude = m * 2^e with m in [0.5, 1)
  int binade = e - 1;
  if (binade < -14)
    binade = -14;
  double quantum = ldexp(1.0, binade - 10);
  // The division is exact (a power of two), and nearbyint rounds to
  // nearest, ties to even, in the default rounding mode.
  double rounded = nearbyint(magnitude / quantum) * quantum;
  return copysign(rounded, value);
}

double Paykan_int_to_f32(int64_t value) { return (double)(float)value; }

double Paykan_uint_to_f32(int64_t bits) {
  return (double)(float)(uint64_t)bits;
}

void Paykan_panic_index(int64_t index, int64_t len) {
  Paykan_runtime_panic("array index %lld out of bounds (len=%lld)",
                       (long long)index, (long long)len);
}

void Paykan_panic_buffer_index(int64_t index, int64_t len) {
  // As Buffer.c's buffer_check_index prints it.
  Paykan_runtime_panic("buffer index %llu out of bounds (len=%llu)",
                       (unsigned long long)index, (unsigned long long)len);
}

void Paykan_panic_buffer_unset(int64_t index) {
  Paykan_runtime_panic("buffer value %llu was never set",
                       (unsigned long long)index);
}

void Paykan_panic_int_to_char(int64_t value) {
  Paykan_runtime_panic(
      "char<int>(%lld): the value is outside the char range 0..255",
      (long long)value);
}

void Paykan_panic_to_char(int64_t value, int64_t isUnsigned, const char *source,
                          int64_t len) {
  if (isUnsigned)
    Paykan_runtime_panic(
        "char<%.*s>(%llu): the value is outside the char range 0..255",
        (int)len, source, (unsigned long long)(uint64_t)value);
  Paykan_runtime_panic(
      "char<%.*s>(%lld): the value is outside the char range 0..255", (int)len,
      source, (long long)value);
}
