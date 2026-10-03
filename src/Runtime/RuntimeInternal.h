// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — internal helpers shared between the runtime's translation
// units.  Not part of the public C ABI (Runtime.h); nothing here is visible
// to generated code or the JIT symbol table.

#ifndef PAYKAN_RUNTIME_INTERNAL_H
#define PAYKAN_RUNTIME_INTERNAL_H

#include "Runtime.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

// -- panics ------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define PAYKAN_PRINTF_FORMAT(fmt, args)                                        \
  __attribute__((format(printf, fmt, args)))
#else
#define PAYKAN_PRINTF_FORMAT(fmt, args)
#endif

/// Abnormal termination for every runtime panic: flush whatever the program
/// has already written to stdout (and to any open File: every output stream
/// is flushed), then print the "paykan: <message>\n" diagnostic (@p fmt is a
/// printf format, without the prefix or the newline) to stderr, flush it, and
/// abort().  abort() flushes no stdio buffer, and
/// stdout is fully buffered when it is not a terminal (a pipe or a file), so
/// without the first flush the output printed before the panic would be lost
/// -- and flushing it before writing the message keeps the two streams in
/// program order when they are merged (`2>&1`).
PAYKAN_NORETURN void Paykan_runtime_panic(const char *fmt, ...)
    PAYKAN_PRINTF_FORMAT(1, 2);

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

/// Format a float the way Str<float> prints it ("%g"), with NaN and the
/// infinities spelled canonically: "nan" (whatever its sign or payload),
/// "inf" and "-inf".  printf's spelling of these is platform-dependent
/// ("-nan" for a NaN with the sign bit set on glibc), and whether a NaN has
/// its sign bit set depends on the hardware and on how it was produced, so
/// without this the output would differ between backends and platforms.
/// Returns the length written to @p buf (at most @p size - 1).
static inline int Paykan_format_float(char *buf, size_t size, double value) {
  if (isnan(value))
    return snprintf(buf, size, "nan");
  if (isinf(value))
    return snprintf(buf, size, value < 0 ? "-inf" : "inf");
  return snprintf(buf, size, "%g", value);
}

#endif // PAYKAN_RUNTIME_INTERNAL_H
