// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Test helper for the equals ABI.
//
// The vtable `equals` slot takes its `other` argument as a PaykanShared box and
// consumes it (releases one reference), matching how class-typed method
// arguments are passed in generated code. These unit tests, however, retain
// ownership of `other` and destroy it themselves. This helper wraps a call so
// the test keeps `other`: it boxes the argument, adds an extra retain to offset
// the callee's release, invokes equals, then frees the box shell — leaving the
// underlying object untouched. Freeing the shell also detaches the object
// (clears its unique-box backpointer and the box's object pointer) so a later
// boxing of the same object starts fresh instead of acquiring the freed shell.

#ifndef PAYKAN_RUNTIME_EQUALS_HELPER_H
#define PAYKAN_RUNTIME_EQUALS_HELPER_H

extern "C" {
#include "Runtime.h"
}

inline int64_t paykanTestEquals(int64_t (*fn)(PaykanObject *, PaykanShared *),
                                PaykanObject *self, PaykanObject *other) {
  PaykanShared *box = PaykanShared_new(other);
  Paykan_retain(box); // offset the release performed by the callee
  int64_t result = fn(self, box);
  // Drop the extra reference without destroying `other` (the test owns it,
  // and disposes of it itself): the count goes back to what it was.
  if (other)
    --other->strong;
  return result;
}

#endif // PAYKAN_RUNTIME_EQUALS_HELPER_H
