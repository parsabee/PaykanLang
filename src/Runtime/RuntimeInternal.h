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

// -- virtual calls
//
// A vtable slot holds a PaykanMethod; it is converted back to the method's
// own type (the PAYKAN_SLOT_* comments in Runtime.h) before the call.

static inline void Paykan_vcall_destroy(PaykanObject *self) {
  ((void (*)(PaykanObject *))Paykan_vtable_of(self)[PAYKAN_SLOT_DESTROY])(self);
}

static inline PaykanShared *Paykan_vcall_toString(PaykanObject *self) {
  return ((PaykanShared * (*)(PaykanObject *))
              Paykan_vtable_of(self)[PAYKAN_SLOT_TO_STRING])(self);
}

static inline int64_t Paykan_vcall_equals(PaykanObject *self,
                                          PaykanShared *other) {
  return ((int64_t(*)(PaykanObject *, PaykanShared *))Paykan_vtable_of(
      self)[PAYKAN_SLOT_EQUALS])(self, other);
}

// -- panics

#if defined(__GNUC__) || defined(__clang__)
#define PAYKAN_PRINTF_FORMAT(fmt, args)                                        \
  __attribute__((format(printf, fmt, args)))
#else
#define PAYKAN_PRINTF_FORMAT(fmt, args)
#endif

/// Every runtime panic: flush every output stream (abort() flushes none, and
/// stdout is fully buffered when it is a pipe or a file, so what the program
/// printed before the panic would otherwise be lost; flushing first also
/// keeps stdout and stderr in program order under `2>&1`), print "paykan:
/// <message>\n" to stderr (@p fmt is a printf format without the prefix or
/// the newline), and abort().
PAYKAN_NORETURN void Paykan_runtime_panic(const char *fmt, ...)
    PAYKAN_PRINTF_FORMAT(1, 2);

// -- equals helpers
//
// Every `*_equals` has the same calling convention: `other` arrives as a
// PaykanShared box (the convention for class-typed arguments) that the call
// CONSUMES, and may be NULL (the hole a `mov` leaves behind), which is equal
// to nothing.  So an implementation is
//
//   int64_t PaykanFoo_equals(PaykanObject *self, PaykanShared *other) {
//     PaykanObject *o = Paykan_equals_unbox_other(other);
//     int64_t result = o && <compare self against o>;
//     return Paykan_equals_consume_other(other, result);
//   }
//
// and compares BEFORE consuming: releasing the box can destroy the object.

/// The object of the consumed `other` box, or NULL for a NULL box.
static inline PaykanObject *Paykan_equals_unbox_other(PaykanShared *other) {
  return PaykanShared_get(other);
}

/// Release the consumed `other` box (null-safe); returns @p result.
static inline int64_t Paykan_equals_consume_other(PaykanShared *other,
                                                  int64_t result) {
  Paykan_release(other);
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
