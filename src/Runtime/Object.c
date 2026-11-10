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

void Paykan_freeze(PaykanObject *obj) {
  if (!obj || obj->strong == PAYKAN_REF_MAX)
    return;
  obj->strong = PAYKAN_REF_MAX;
  Paykan_vcall_walk(obj);
}

// The check of a value moved out of borrowed storage (#368;
// docs/design/concurrency.md, "Owned paths and borrowed roots"): every
// object the value uniquely holds has a count of 1, or something else still
// holds it.  The objects to visit are kept on a stack of the thread's own,
// on the C heap (never Paykan_malloc, so --track-heap counts are
// unaffected), so a long chain of unique objects cannot overflow the stack.
static _Thread_local int g_checking_move; // nonzero in Paykan_moved_unique
static _Thread_local int g_move_held;     // a count above 1 was met
#define PAYKAN_MOVE_INLINE 32
static _Thread_local PaykanObject *g_move_inline[PAYKAN_MOVE_INLINE];
static _Thread_local PaykanObject **g_move_heap; // past g_move_inline's room
static _Thread_local size_t g_move_len;
static _Thread_local size_t g_move_cap;

static void move_visit(PaykanObject *obj) {
  if (!obj || obj->strong == PAYKAN_REF_MAX)
    return;
  if (obj->strong > 1) {
    g_move_held = 1;
    return;
  }
  if (g_move_len < PAYKAN_MOVE_INLINE) {
    g_move_inline[g_move_len++] = obj;
    return;
  }
  const size_t past = g_move_len - PAYKAN_MOVE_INLINE;
  if (past == g_move_cap) {
    size_t cap = g_move_cap ? g_move_cap * 2 : 64;
    PaykanObject **grown =
        (PaykanObject **)realloc(g_move_heap, cap * sizeof(PaykanObject *));
    if (!grown)
      Paykan_runtime_panic("out of memory checking a moved value");
    g_move_heap = grown;
    g_move_cap = cap;
  }
  g_move_heap[past] = obj;
  ++g_move_len;
}

static int64_t move_drain(void) {
  while (g_move_len && !g_move_held) {
    --g_move_len;
    Paykan_vcall_walk(g_move_len < PAYKAN_MOVE_INLINE
                          ? g_move_inline[g_move_len]
                          : g_move_heap[g_move_len - PAYKAN_MOVE_INLINE]);
  }
  g_move_len = 0;
  g_checking_move = 0;
  free(g_move_heap); // a long chain's peak is not kept for the thread's life
  g_move_heap = NULL;
  g_move_cap = 0;
  return !g_move_held;
}

void Paykan_walk_child(PaykanObject *obj, int64_t unique) {
  if (!g_checking_move)
    Paykan_freeze(obj);
  else if (unique)
    move_visit(obj);
}

void Paykan_freeze_mem(const void *type, void *value) {
  const PaykanType *t = (const PaykanType *)type;
  for (int64_t k = 0; k < t->nrefs; ++k) {
    const int64_t word = PaykanType_ref(t, k);
    PaykanShared *ref =
        *(PaykanShared **)((unsigned char *)value + PaykanType_refOffset(word));
    if (!g_checking_move)
      Paykan_freeze(ref);
    else if (!(word & 2))
      move_visit(ref);
  }
}

int64_t Paykan_moved_unique(PaykanShared *box) {
  g_checking_move = 1;
  g_move_held = 0;
  move_visit((PaykanObject *)box);
  return move_drain();
}

int64_t Paykan_moved_unique_mem(const void *type, void *value) {
  g_checking_move = 1;
  g_move_held = 0;
  Paykan_freeze_mem(type, value);
  return move_drain();
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

// -- None singleton: an immortal Obj whose toString is "None".

static void PaykanNone_destroy(PaykanObject *self) { (void)self; }

static PaykanShared *PaykanNone_toString(PaykanObject *self) {
  (void)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new("None", 4));
}

static PaykanMethod PaykanNone_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanNone_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanNone_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanObject_equals,
    [PAYKAN_SLOT_WALK] = (PaykanMethod)PaykanObject_walk,
};

// Immortal: its strong count starts at 1, which nothing releases, so the
// boxings and releases of the singleton never destroy it.
PaykanObject PaykanObject_None = {PaykanNone_vtable, 1, 0};

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
