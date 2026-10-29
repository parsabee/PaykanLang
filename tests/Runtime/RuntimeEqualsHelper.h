// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Test helper for the equals ABI.
//
// The vtable `equals` slot takes its `other` argument as a PaykanShared box
// and an ownership mask whose bit 0 says whether the call releases it, as a
// Paykan function's box parameter is passed (Runtime.h, "The ownership
// mask").  These unit tests keep `other` and dispose of it themselves, so
// this helper lends it: it boxes the argument, calls equals with mask 0,
// then drops the box's reference without destroying the object (the count
// goes back to what it was).

#ifndef PAYKAN_RUNTIME_EQUALS_HELPER_H
#define PAYKAN_RUNTIME_EQUALS_HELPER_H

extern "C" {
#include "Runtime.h"
}

inline int64_t paykanTestEquals(int64_t (*fn)(PaykanObject *, PaykanShared *,
                                              uint64_t),
                                PaykanObject *self, PaykanObject *other) {
  PaykanShared *box = PaykanShared_new(other);
  int64_t result = fn(self, box, 0); // lent: equals releases nothing
  if (other)
    --other->strong;
  return result;
}

#endif // PAYKAN_RUNTIME_EQUALS_HELPER_H
