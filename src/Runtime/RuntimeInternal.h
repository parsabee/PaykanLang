// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
#include <string.h>

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
                                          PaykanShared *other, uint64_t owned) {
  return ((int64_t (*)(PaykanObject *, PaykanShared *, uint64_t))
              Paykan_vtable_of(self)[PAYKAN_SLOT_EQUALS])(self, other, owned);
}

// -- branch hints
//
// PAYKAN_UNLIKELY(x) is x, telling the compiler it is seldom true;
// PAYKAN_COLD marks a function only such a branch calls, kept out of line.

#if defined(__GNUC__) || defined(__clang__)
#define PAYKAN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define PAYKAN_COLD __attribute__((cold, noinline))
#else
#define PAYKAN_UNLIKELY(x) (x)
#define PAYKAN_COLD
#endif

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

// -- allocations that may fail
//
// Paykan_malloc and Paykan_realloc panic when the memory is not there.
// These are the same allocations for the callers that report it instead
// (memory::Buffer's OutOfMemory, an io::File read's): NULL with errno ENOMEM,
// a realloc's @p ptr then still valid and unchanged.  A size of 0 gives a
// block too, so NULL always means out of memory.

void *Paykan_tryMalloc(size_t size);
void *Paykan_tryRealloc(void *ptr, size_t size);

// -- storage never set
//
// PaykanType_isUnset (Runtime.h) as an inline function, for memory::Buffer's
// checked reads (Buffer.c).  Every read tests it, so it is no call there: a
// call made each read of a type with words to look at (a tagged union whose
// first variant holds a Str) save and reload its registers around it.

/// True if @p value is storage that was never set: @p type has words that
/// tell it (`nunset`), and they are all 0 there.
static inline int PaykanType_isUnset_inline(const PaykanType *type,
                                            const void *value) {
  for (int64_t k = 0; k < type->nunset; ++k) {
    int64_t word;
    memcpy(&word, (const char *)value + PaykanType_unset(type, k), sizeof word);
    if (word)
      return 0;
  }
  return type->nunset != 0;
}

// -- equals helpers
//
// Every `*_equals` has the same calling convention, its slot's: `other`
// arrives as a PaykanShared box (the convention for class-typed arguments)
// that the call releases when bit 0 of the ownership mask `owned` says it is
// handed over (Runtime.h, "The ownership mask"), and may be NULL (an absent
// optional is a null box), which is equal to nothing.  So an implementation
// is
//
//   int64_t PaykanFoo_equals(PaykanObject *self, PaykanShared *other,
//                            uint64_t owned) {
//     PaykanObject *o = Paykan_equals_unbox_other(other);
//     int64_t result = o && <compare self against o>;
//     return Paykan_equals_drop_other(other, owned, result);
//   }
//
// and compares BEFORE dropping: releasing the box can destroy the object.

/// The object of the `other` box, or NULL for a NULL box.
static inline PaykanObject *Paykan_equals_unbox_other(PaykanShared *other) {
  return PaykanShared_get_inline(other);
}

/// Release the `other` box (null-safe) when bit 0 of @p owned says it was
/// handed over; returns @p result.
static inline int64_t Paykan_equals_drop_other(PaykanShared *other,
                                               uint64_t owned, int64_t result) {
  if (owned & 1)
    Paykan_release_inline(other);
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
