// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — boxed primitive types: Int, Float, Bool.
//
// Each type inherits from Obj (PaykanObject-compatible prefix) and wraps a
// single primitive value.  These boxed types allow primitives to be stored
// in Obj-typed variables and passed through the same ARC machinery as any
// other heap object.

#include "Runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Int
// ============================================================================

PaykanObjectVTable PaykanInt_vtable = {
    .destroy = PaykanInt_destroy,
    .toString = PaykanInt_toString,
    .equals = PaykanInt_equals,
};

PaykanInt *PaykanInt_new(int64_t value) {
  PaykanInt *obj = (PaykanInt *)Paykan_malloc(sizeof(PaykanInt));
  obj->vtable = &PaykanInt_vtable;
  obj->value = value;
  return obj;
}

void PaykanInt_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanInt_toString(PaykanObject *self) {
  PaykanInt *obj = (PaykanInt *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_int(obj->value));
}

int64_t PaykanInt_equals(PaykanObject *self, PaykanObject *other) {
  if (other->vtable != &PaykanInt_vtable)
    return 0;
  return ((PaykanInt *)self)->value == ((PaykanInt *)other)->value;
}

PaykanShared *PaykanInt_from_str(PaykanObject *str) {
  PaykanString *s = (PaykanString *)str;
  char *end;
  errno = 0;
  long long val = strtoll(s->data, &end, 10);
  if (end == s->data || *end != '\0' || errno != 0) {
    const char *msg = "IntStr: invalid integer string";
    return PaykanShared_new(
        (PaykanObject *)PaykanError_new(msg, (int64_t)strlen(msg)));
  }
  return PaykanShared_new((PaykanObject *)PaykanInt_new((int64_t)val));
}

// ============================================================================
// Float
// ============================================================================

PaykanObjectVTable PaykanFloat_vtable = {
    .destroy = PaykanFloat_destroy,
    .toString = PaykanFloat_toString,
    .equals = PaykanFloat_equals,
};

PaykanFloat *PaykanFloat_new(double value) {
  PaykanFloat *obj = (PaykanFloat *)Paykan_malloc(sizeof(PaykanFloat));
  obj->vtable = &PaykanFloat_vtable;
  obj->value = value;
  return obj;
}

void PaykanFloat_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanFloat_toString(PaykanObject *self) {
  PaykanFloat *obj = (PaykanFloat *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_float(obj->value));
}

int64_t PaykanFloat_equals(PaykanObject *self, PaykanObject *other) {
  if (other->vtable != &PaykanFloat_vtable)
    return 0;
  return ((PaykanFloat *)self)->value == ((PaykanFloat *)other)->value;
}

PaykanShared *PaykanFloat_from_str(PaykanObject *str) {
  PaykanString *s = (PaykanString *)str;
  char *end;
  errno = 0;
  double val = strtod(s->data, &end);
  if (end == s->data || *end != '\0' || errno != 0) {
    const char *msg = "FloatStr: invalid float string";
    return PaykanShared_new(
        (PaykanObject *)PaykanError_new(msg, (int64_t)strlen(msg)));
  }
  return PaykanShared_new((PaykanObject *)PaykanFloat_new(val));
}

// ============================================================================
// Bool
// ============================================================================

PaykanObjectVTable PaykanBool_vtable = {
    .destroy = PaykanBool_destroy,
    .toString = PaykanBool_toString,
    .equals = PaykanBool_equals,
};

PaykanBool *PaykanBool_new(int64_t value) {
  PaykanBool *obj = (PaykanBool *)Paykan_malloc(sizeof(PaykanBool));
  obj->vtable = &PaykanBool_vtable;
  obj->value = value ? 1 : 0;
  return obj;
}

void PaykanBool_destroy(PaykanObject *self) { Paykan_free(self); }

PaykanShared *PaykanBool_toString(PaykanObject *self) {
  PaykanBool *obj = (PaykanBool *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_from_bool(obj->value));
}

int64_t PaykanBool_equals(PaykanObject *self, PaykanObject *other) {
  if (other->vtable != &PaykanBool_vtable)
    return 0;
  return ((PaykanBool *)self)->value == ((PaykanBool *)other)->value;
}
