// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — `cp` support for the runtime classes (Runtime.h, "Clone").

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <string.h>

static int is_obj_array(PaykanMethod *vt) {
  return vt == PaykanArray_obj_vtable;
}

PaykanShared *Paykan_clone_shallow(PaykanObject *obj) {
  PaykanMethod *vt = Paykan_vtable_of(obj);
  if (vt == PaykanString_vtable) {
    PaykanString *s = (PaykanString *)obj;
    return PaykanShared_new((PaykanObject *)PaykanString_new(s->data, s->len));
  }
  if (vt == PaykanArray_vtable || is_obj_array(vt)) {
    PaykanArray *a = (PaykanArray *)obj;
    PaykanArray *c = is_obj_array(vt) ? PaykanArray_new_obj(a->len)
                                      : PaykanArray_new(a->len);
    if (a->len)
      memcpy(c->data, a->data, a->len * sizeof(void *));
    if (is_obj_array(vt))
      for (unsigned long i = 0; i < a->len; ++i)
        Paykan_retain((PaykanShared *)PaykanArray_get(c, i));
    return PaykanShared_new((PaykanObject *)c);
  }
  if (vt == PaykanTuple_vtable) {
    PaykanTuple *t = (PaykanTuple *)obj;
    PaykanTuple *c = PaykanTuple_new(t->count, t->kinds);
    if (t->count)
      memcpy(c->slots, t->slots, (size_t)t->count * sizeof(uint64_t));
    for (int64_t i = 0; i < t->count; ++i)
      if (t->kinds[i] == PAYKAN_TUPLE_REF)
        Paykan_retain(Paykan_clone_slot_get((PaykanObject *)c, i));
    return PaykanShared_new((PaykanObject *)c);
  }
  if (vt == PaykanInt_vtable || vt == PaykanFloat_vtable ||
      vt == PaykanBool_vtable || vt == PaykanChar_vtable ||
      vt == PaykanObject_vtable || vt == PaykanFile_vtable ||
      vt == PaykanError_vtable)
    return PaykanShared_new(obj);
  Paykan_runtime_panic("cp: no clone function for this object's class");
}

int64_t Paykan_clone_slots(PaykanObject *obj) {
  PaykanMethod *vt = Paykan_vtable_of(obj);
  if (is_obj_array(vt))
    return (int64_t)((PaykanArray *)obj)->len;
  if (vt == PaykanTuple_vtable)
    return ((PaykanTuple *)obj)->count;
  return 0;
}

PaykanShared *Paykan_clone_slot_get(PaykanObject *obj, int64_t idx) {
  PaykanMethod *vt = Paykan_vtable_of(obj);
  if (is_obj_array(vt))
    return (PaykanShared *)PaykanArray_get((PaykanArray *)obj,
                                           (unsigned long)idx);
  if (vt == PaykanTuple_vtable) {
    PaykanTuple *t = (PaykanTuple *)obj;
    if (PaykanTuple_kind(t, idx) != PAYKAN_TUPLE_REF)
      return NULL;
    int64_t bits = PaykanTuple_get(t, idx);
    PaykanShared *box;
    memcpy(&box, &bits, sizeof bits);
    return box;
  }
  return NULL;
}

void Paykan_clone_slot_set(PaykanObject *obj, int64_t idx,
                           PaykanShared *value) {
  if (is_obj_array(Paykan_vtable_of(obj)))
    PaykanArray_set_obj((PaykanArray *)obj, (unsigned long)idx, value);
  else
    PaykanTuple_set_obj((PaykanTuple *)obj, idx, value);
}
