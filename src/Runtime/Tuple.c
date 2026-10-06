// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — Tuple type implementation.
//
// One object backs every tuple type (Runtime.h, "Tuple"): the per-slot kind
// bytes let a single vtable destroy (release the REF slots), compare
// (element-wise, dispatching `equals` on references) and render (`(1, a)`)
// a tuple whose element types only the compiler knows.  Header, slots and
// kind bytes are one heap block.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A REF slot is read and written by memcpy of the slot width, so the two
// widths must agree.
_Static_assert(sizeof(PaykanShared *) == sizeof(uint64_t),
               "a tuple slot must hold a PaykanShared* exactly");

// -- VTable

PaykanMethod PaykanTuple_vtable[PAYKAN_OBJECT_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanTuple_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanTuple_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanTuple_equals,
};

// -- Constructor / destructor

PaykanTuple *PaykanTuple_new(int64_t count, const uint8_t *kinds) {
  if (count < 0) {
    Paykan_runtime_panic("negative tuple arity %lld", (long long)count);
  }
  size_t n = (size_t)count;
  // Slots follow the fixed header (sizeof(PaykanTuple) is a multiple of 8, so
  // they stay 8-byte aligned); the kind bytes follow the slots.  Slots are
  // zeroed so an early destroy (or a REF slot never set) releases nothing.
  PaykanTuple *t = (PaykanTuple *)Paykan_malloc(sizeof(PaykanTuple) +
                                                n * sizeof(uint64_t) + n);
  t->vtable = PaykanTuple_vtable;
  t->shared = NULL; // not yet boxed (unique-box invariant)
  t->count = count;
  t->slots = (uint64_t *)(t + 1);
  memset(t->slots, 0, n * sizeof(uint64_t));
  t->kinds = (uint8_t *)(t->slots + n);
  if (n)
    memcpy(t->kinds, kinds, n);
  return t;
}

void PaykanTuple_destroy(PaykanObject *self) {
  PaykanTuple *t = (PaykanTuple *)self;
  for (int64_t i = 0; i < t->count; ++i) {
    if (t->kinds[i] != PAYKAN_TUPLE_REF)
      continue;
    PaykanShared *box;
    memcpy(&box, &t->slots[i], sizeof(t->slots[i]));
    if (box)
      Paykan_release(box);
  }
  Paykan_free(t);
}

// -- Element access

static void tuple_check_index(const PaykanTuple *t, int64_t idx) {
  if (idx < 0 || idx >= t->count) {
    Paykan_runtime_panic("tuple index %lld out of bounds (arity=%lld)",
                         (long long)idx, (long long)t->count);
  }
}

int64_t PaykanTuple_count(PaykanTuple *t) { return t->count; }

int64_t PaykanTuple_kind(PaykanTuple *t, int64_t idx) {
  tuple_check_index(t, idx);
  return t->kinds[idx];
}

int64_t PaykanTuple_get(PaykanTuple *t, int64_t idx) {
  tuple_check_index(t, idx);
  int64_t bits;
  memcpy(&bits, &t->slots[idx], sizeof(bits));
  return bits;
}

void PaykanTuple_set(PaykanTuple *t, int64_t idx, int64_t bits) {
  tuple_check_index(t, idx);
  if (t->kinds[idx] == PAYKAN_TUPLE_REF) {
    Paykan_runtime_panic("PaykanTuple_set on reference slot %lld",
                         (long long)idx);
  }
  memcpy(&t->slots[idx], &bits, sizeof(bits));
}

void PaykanTuple_set_obj(PaykanTuple *t, int64_t idx, PaykanShared *value) {
  tuple_check_index(t, idx);
  if (t->kinds[idx] != PAYKAN_TUPLE_REF) {
    Paykan_runtime_panic("PaykanTuple_set_obj on value slot %lld",
                         (long long)idx);
  }
  PaykanShared *old;
  memcpy(&old, &t->slots[idx], sizeof(t->slots[idx]));
  if (old)
    Paykan_release(old);
  if (value)
    Paykan_retain(value);
  memcpy(&t->slots[idx], &value, sizeof(t->slots[idx]));
}

// -- toString

/// A growable byte buffer for the rendering.
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

static void sb_append(StrBuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t newCap = b->cap ? b->cap : 32;
    while (b->len + n + 1 > newCap)
      newCap *= 2;
    b->data = (char *)Paykan_realloc(b->data, newCap);
    b->cap = newCap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

PaykanShared *PaykanTuple_toString(PaykanObject *self) {
  PaykanTuple *t = (PaykanTuple *)self;
  StrBuf sb = {NULL, 0, 0};
  sb_append(&sb, "(", 1);
  for (int64_t i = 0; i < t->count; ++i) {
    if (i)
      sb_append(&sb, ", ", 2);
    char buf[64];
    int n;
    int64_t bits;
    memcpy(&bits, &t->slots[i], sizeof(bits));
    switch (t->kinds[i]) {
    case PAYKAN_TUPLE_FLOAT: {
      double d;
      memcpy(&d, &bits, sizeof(d));
      n = Paykan_format_float(buf, sizeof(buf), d); // matches Str<float>
      sb_append(&sb, buf, (size_t)n);
      break;
    }
    case PAYKAN_TUPLE_BOOL:
      if (bits)
        sb_append(&sb, "True", 4);
      else
        sb_append(&sb, "False", 5);
      break;
    case PAYKAN_TUPLE_CHAR: {
      char c = (char)bits;
      sb_append(&sb, &c, 1);
      break;
    }
    case PAYKAN_TUPLE_REF: {
      PaykanShared *box;
      memcpy(&box, &bits, sizeof(bits));
      PaykanObject *obj = PaykanShared_get(box);
      if (!obj) {
        sb_append(&sb, "None", 4);
        break;
      }
      // Dispatch the element's own toString (a fresh +1 Str box we consume).
      PaykanShared *s = Paykan_vcall_toString(obj);
      PaykanString *str = (PaykanString *)PaykanShared_get(s);
      sb_append(&sb, str->data, (size_t)str->len);
      Paykan_release(s);
      break;
    }
    case PAYKAN_TUPLE_INT:
    default:
      n = snprintf(buf, sizeof(buf), "%lld", (long long)bits);
      sb_append(&sb, buf, (size_t)n);
      break;
    }
  }
  sb_append(&sb, ")", 1);
  PaykanString *out =
      PaykanString_new(sb.data ? sb.data : "()", sb.data ? (int64_t)sb.len : 2);
  Paykan_free(sb.data);
  return PaykanShared_new((PaykanObject *)out);
}

// -- equals

int64_t PaykanTuple_equals(PaykanObject *self, PaykanShared *other) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = 0;
  // No identity shortcut (`o == self` -> equal): equality is element-wise
  // even for one object, so a tuple holding a NaN is unequal to itself, as
  // the NaN is (IEEE 754, #111).
  if (o && o->vtable == PaykanTuple_vtable) {
    PaykanTuple *a = (PaykanTuple *)self;
    PaykanTuple *b = (PaykanTuple *)o;
    // Same arity and the same element kinds (a `(int, Str)` never equals a
    // `(int, int)`; Sema already forbids comparing them statically).
    result = a->count == b->count &&
             memcmp(a->kinds, b->kinds, (size_t)a->count) == 0;
    for (int64_t i = 0; result && i < a->count; ++i) {
      switch (a->kinds[i]) {
      case PAYKAN_TUPLE_FLOAT: {
        double da, db;
        memcpy(&da, &a->slots[i], sizeof(da));
        memcpy(&db, &b->slots[i], sizeof(db));
        result = da == db;
        break;
      }
      case PAYKAN_TUPLE_REF: {
        PaykanShared *sa, *sb;
        memcpy(&sa, &a->slots[i], sizeof(a->slots[i]));
        memcpy(&sb, &b->slots[i], sizeof(b->slots[i]));
        PaykanObject *oa = PaykanShared_get(sa);
        PaykanObject *ob = PaykanShared_get(sb);
        if (!oa || !ob) {
          result = oa == ob; // two empty slots are equal; one empty is not
          break;
        }
        // The virtual `equals` consumes its argument box: hand it a +1.
        Paykan_retain(sb);
        result = Paykan_vcall_equals(oa, sb) != 0;
        break;
      }
      case PAYKAN_TUPLE_INT:
      case PAYKAN_TUPLE_BOOL:
      case PAYKAN_TUPLE_CHAR:
      default:
        result = a->slots[i] == b->slots[i];
        break;
      }
    }
  }
  return Paykan_equals_consume_other(other, result);
}
