// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — boxed primitive types: Int, Float, Bool, Char.
//
// Each type inherits from Obj (PaykanObject-compatible prefix) and wraps a
// single primitive value.  These boxed types allow primitives to be stored
// in Obj-typed variables and passed through the same ARC machinery as any
// other heap object; they are also the boxes of the optional primitives
// (`int?` holds a PaykanShared* to a PaykanInt, NULL meaning None).

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Int
// ============================================================================

PaykanMethod PaykanInt_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanInt_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanInt_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanInt_equals,
};

PaykanInt *PaykanInt_new(int64_t value) {
  PaykanInt *obj = (PaykanInt *)Paykan_malloc(sizeof(PaykanInt));
  obj->vtable = PaykanInt_vtable;
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  obj->value = value;
  return obj;
}

int64_t PaykanInt_value(PaykanObject *self) {
  return ((PaykanInt *)self)->value;
}

void PaykanInt_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanInt_toString(PaykanObject *self) {
  PaykanInt *obj = (PaykanInt *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_int(obj->value));
}

int64_t PaykanInt_equals(PaykanObject *self, PaykanShared *other) {
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h).
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanInt_vtable &&
                   ((PaykanInt *)self)->value == ((PaykanInt *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

/// True when @p s starts with a character strtoll / strtod would skip as
/// leading whitespace, which `int<Str>` / `float<Str>` reject.
static int Paykan_starts_with_space(const PaykanString *s) {
  return s->len > 0 && isspace((unsigned char)s->data[0]);
}

PaykanShared *PaykanInt_from_str(PaykanObject *str) {
  PaykanString *s = (PaykanString *)str;
  if (s->len == 0 || Paykan_starts_with_space(s))
    return NULL;
  char *end;
  errno = 0;
  long long val = strtoll(s->data, &end, 10);
  // The whole string must be consumed (an embedded NUL ends strtoll early)
  // and the value must fit (ERANGE).
  if (end != s->data + s->len || errno != 0)
    return NULL;
  return PaykanShared_new((PaykanObject *)PaykanInt_new((int64_t)val));
}

// ============================================================================
// Float
// ============================================================================

PaykanMethod PaykanFloat_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanFloat_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanFloat_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanFloat_equals,
};

PaykanFloat *PaykanFloat_new(double value) {
  PaykanFloat *obj = (PaykanFloat *)Paykan_malloc(sizeof(PaykanFloat));
  obj->vtable = PaykanFloat_vtable;
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  obj->value = value;
  return obj;
}

double PaykanFloat_value(PaykanObject *self) {
  return ((PaykanFloat *)self)->value;
}

void PaykanFloat_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanFloat_toString(PaykanObject *self) {
  PaykanFloat *obj = (PaykanFloat *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_float(obj->value));
}

int64_t PaykanFloat_equals(PaykanObject *self, PaykanShared *other) {
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h).
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanFloat_vtable &&
                   ((PaykanFloat *)self)->value == ((PaykanFloat *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

PaykanShared *PaykanFloat_from_str(PaykanObject *str) {
  PaykanString *s = (PaykanString *)str;
  if (s->len == 0 || Paykan_starts_with_space(s))
    return NULL;
  char *end;
  errno = 0;
  double val = strtod(s->data, &end);
  if (end != s->data + s->len)
    return NULL;
  // ERANGE flags overflow (±HUGE_VAL), a nonzero value underflowing to 0, and
  // also a subnormal result, which is representable and accepted.
  if (errno != 0 && (errno != ERANGE || isinf(val) || val == 0.0))
    return NULL;
  return PaykanShared_new((PaykanObject *)PaykanFloat_new(val));
}

// ============================================================================
// Bool
// ============================================================================

PaykanMethod PaykanBool_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanBool_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanBool_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanBool_equals,
};

PaykanBool *PaykanBool_new(int64_t value) {
  PaykanBool *obj = (PaykanBool *)Paykan_malloc(sizeof(PaykanBool));
  obj->vtable = PaykanBool_vtable;
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  obj->value = value ? 1 : 0;
  return obj;
}

int64_t PaykanBool_value(PaykanObject *self) {
  return ((PaykanBool *)self)->value;
}

void PaykanBool_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanBool_toString(PaykanObject *self) {
  PaykanBool *obj = (PaykanBool *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_bool(obj->value));
}

int64_t PaykanBool_equals(PaykanObject *self, PaykanShared *other) {
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h).
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanBool_vtable &&
                   ((PaykanBool *)self)->value == ((PaykanBool *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

PaykanShared *PaykanBool_from_str(PaykanObject *str) {
  PaykanString *s = (PaykanString *)str;
  // Exactly the spellings `Str<bool>` prints, so the two round-trip.
  if (s->len == 4 && memcmp(s->data, "True", 4) == 0)
    return PaykanShared_new((PaykanObject *)PaykanBool_new(1));
  if (s->len == 5 && memcmp(s->data, "False", 5) == 0)
    return PaykanShared_new((PaykanObject *)PaykanBool_new(0));
  return NULL;
}

// ============================================================================
// Char
// ============================================================================

PaykanMethod PaykanChar_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanChar_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanChar_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanChar_equals,
};

PaykanChar *PaykanChar_new(int8_t value) {
  PaykanChar *obj = (PaykanChar *)Paykan_malloc(sizeof(PaykanChar));
  obj->vtable = PaykanChar_vtable;
  obj->shared = NULL; // not yet boxed (unique-box invariant)
  obj->value = value;
  return obj;
}

int8_t PaykanChar_value(PaykanObject *self) {
  return ((PaykanChar *)self)->value;
}

void PaykanChar_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanChar_toString(PaykanObject *self) {
  PaykanChar *obj = (PaykanChar *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_char(obj->value));
}

int64_t PaykanChar_equals(PaykanObject *self, PaykanShared *other) {
  // `other` arrives as a consumed PaykanShared box (see RuntimeInternal.h).
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanChar_vtable &&
                   ((PaykanChar *)self)->value == ((PaykanChar *)o)->value;
  return Paykan_equals_consume_other(other, result);
}
