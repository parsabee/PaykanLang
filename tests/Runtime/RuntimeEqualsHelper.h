// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Test helper for the equals ABI.
//
// The vtable `equals` slot takes its `other` argument as a PaykanShared box and
// consumes it (releases one reference), matching how class-typed method
// arguments are passed in generated code. These unit tests, however, retain
// ownership of `other` and destroy it themselves. This helper wraps a call so
// the test keeps `other`: it boxes the argument, adds an extra retain to offset
// the callee's release, invokes equals, then frees the box shell — leaving the
// underlying object untouched.

#ifndef PAYKAN_RUNTIME_EQUALS_HELPER_H
#define PAYKAN_RUNTIME_EQUALS_HELPER_H

extern "C" {
#include "Runtime.h"
}

inline int64_t paykanTestEquals(int64_t (*fn)(PaykanObject *, PaykanObject *),
                                PaykanObject *self, PaykanObject *other) {
  PaykanShared *box = PaykanShared_new(other);
  Paykan_retain(box); // offset the release performed by the callee
  int64_t result = fn(self, reinterpret_cast<PaykanObject *>(box));
  Paykan_free(box); // free the box shell; `other` remains owned by the test
  return result;
}

#endif // PAYKAN_RUNTIME_EQUALS_HELPER_H
