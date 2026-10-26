// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// 15_native_generic — the C side of main.pkn's generic native functions.
//
// Each gets a PaykanType for its type argument first.  A `T` argument is the
// address of the value: borrowed for the call, or for an `inout` one the
// caller's storage.  A `T` result is written to `out`, owning the references
// it holds (PaykanType_copy retains them).

#include "Runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

PaykanShared *sample_describe(const PaykanType *t, const void *x) {
  char what[48];
  switch (t->kind) {
  case PAYKAN_KIND_INT:
    snprintf(what, sizeof what, "int %lld", (long long)*(const int64_t *)x);
    break;
  case PAYKAN_KIND_FLOAT:
    snprintf(what, sizeof what, "float %g", *(const double *)x);
    break;
  case PAYKAN_KIND_BOOL:
    snprintf(what, sizeof what, "bool %s", *(const bool *)x ? "True" : "False");
    break;
  case PAYKAN_KIND_CHAR:
    snprintf(what, sizeof what, "char %c", (char)*(const int8_t *)x);
    break;
  case PAYKAN_KIND_TUPLE: // its elements in place, references at offsets
    snprintf(what, sizeof what, "a tuple holding %lld reference(s)",
             (long long)t->nrefs);
    break;
  default: // a reference: None if the optional's is NULL
    snprintf(what, sizeof what, "%s",
             *(PaykanShared *const *)x ? "an object" : "None");
    break;
  }
  char buf[64];
  snprintf(buf, sizeof buf, "%s (size %lld)", what, (long long)t->size);
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(buf, (int64_t)strlen(buf)));
}

void sample_pick(const PaykanType *t, void *out, bool first, const void *a,
                 const void *b) {
  PaykanType_copy(t, out, first ? a : b);
}

// Exchanging two values moves their references: no count changes.
void sample_swap(const PaykanType *t, void *a, void *b) {
  unsigned char *x = a, *y = b;
  for (int64_t i = 0; i < t->size; ++i) {
    unsigned char c = x[i];
    x[i] = y[i];
    y[i] = c;
  }
}
