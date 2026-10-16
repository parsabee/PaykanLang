// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — the boxed primitives Int, Float, Bool and Char: a
// primitive in an Obj-typed variable (an `int?` & co. there is one of them,
// or the None object).

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdlib.h>

// -- Int

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
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanInt_vtable &&
                   ((PaykanInt *)self)->value == ((PaykanInt *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

PaykanShared *PaykanInt_from_str(PaykanObject *str) {
  int64_t val;
  if (!PaykanString_parse_int(str, &val))
    return NULL;
  return PaykanShared_new((PaykanObject *)PaykanInt_new(val));
}

// -- Float

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
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanFloat_vtable &&
                   ((PaykanFloat *)self)->value == ((PaykanFloat *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

PaykanShared *PaykanFloat_from_str(PaykanObject *str) {
  double val;
  if (!PaykanString_parse_float(str, &val))
    return NULL;
  return PaykanShared_new((PaykanObject *)PaykanFloat_new(val));
}

// -- Bool

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
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanBool_vtable &&
                   ((PaykanBool *)self)->value == ((PaykanBool *)o)->value;
  return Paykan_equals_consume_other(other, result);
}

PaykanShared *PaykanBool_from_str(PaykanObject *str) {
  int64_t val;
  if (!PaykanString_parse_bool(str, &val))
    return NULL;
  return PaykanShared_new((PaykanObject *)PaykanBool_new(val));
}

// -- Char

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
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && o->vtable == PaykanChar_vtable &&
                   ((PaykanChar *)self)->value == ((PaykanChar *)o)->value;
  return Paykan_equals_consume_other(other, result);
}
