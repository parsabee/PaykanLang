// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — internal helpers shared between the runtime's translation
// units.  Not part of the public C ABI (Runtime.h); nothing here is visible
// to generated code or the JIT symbol table.

#ifndef PAYKAN_RUNTIME_INTERNAL_H
#define PAYKAN_RUNTIME_INTERNAL_H

#include "Runtime.h"

// -- equals helpers ----------------------------------------------------------
//
// Every `*_equals` implementation shares the same calling convention: `other`
// arrives as a PaykanShared box (the convention for class-typed method
// arguments) and is CONSUMED by the call.  The box may be NULL — e.g. the
// null hole a `mov` leaves behind — and a NULL box compares unequal to
// everything (identity: NULL equals nothing) instead of crashing.
//
// Usage pattern:
//
//   int64_t PaykanFoo_equals(PaykanObject *self, PaykanObject *other) {
//     PaykanObject *o = Paykan_equals_unbox_other(other);
//     int64_t result = o && <compare self against o>;
//     return Paykan_equals_consume_other(other, result);
//   }
//
// The comparison must happen BEFORE Paykan_equals_consume_other: releasing
// the box can drop its refcount to zero and destroy the compared object.

/// Unbox the consumed `other` argument of an equals implementation.
/// Returns the underlying object, or NULL when the box is NULL
/// (PaykanShared_get is null-safe) — callers must treat NULL as "not equal".
static inline PaykanObject *Paykan_equals_unbox_other(PaykanObject *other) {
  return PaykanShared_get((PaykanShared *)other);
}

/// Epilogue: release the consumed `other` box (null-safe) and pass the
/// already-computed comparison result through.
static inline int64_t Paykan_equals_consume_other(PaykanObject *other,
                                                  int64_t result) {
  Paykan_release((PaykanShared *)other);
  return result;
}

#endif // PAYKAN_RUNTIME_INTERNAL_H
