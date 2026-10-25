// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — copying and dropping a value of any type through its
// PaykanType (Runtime.h, "Generic native functions"), for the C code of
// generic native functions, and telling storage never set from a value.

#include "Runtime.h"

#include <string.h>

/// The reference the @p type value at @p value holds at its @p k-th place.
static PaykanShared *type_ref(const PaykanType *type, const void *value,
                              int64_t k) {
  PaykanShared *ref;
  memcpy(&ref, (const char *)value + (PaykanType_ref(type, k) >> 1),
         sizeof(PaykanShared *));
  return ref;
}

void PaykanType_copy(const PaykanType *type, void *dst, const void *src) {
  memcpy(dst, src, (size_t)type->size);
  for (int64_t k = 0; k < type->nrefs; ++k)
    Paykan_retain_inline(type_ref(type, dst, k));
}

void PaykanType_drop(const PaykanType *type, void *value) {
  for (int64_t k = 0; k < type->nrefs; ++k)
    Paykan_release_inline(type_ref(type, value, k));
}

int PaykanType_isUnset(const PaykanType *type, const void *value) {
  for (int64_t k = 0; k < type->nunset; ++k) {
    int64_t word;
    memcpy(&word, (const char *)value + PaykanType_unset(type, k), sizeof word);
    if (word)
      return 0;
  }
  return type->nunset != 0;
}
