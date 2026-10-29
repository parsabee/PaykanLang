// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — String type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- VTable

PaykanMethod PaykanString_vtable[PAYKAN_STRING_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanString_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanString_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanString_equals,
    [PAYKAN_SLOT_STRING_LENGTH] = (PaykanMethod)PaykanString_length,
    [PAYKAN_SLOT_STRING_CONCAT] = (PaykanMethod)PaykanString_concat_inplace,
};

// -- Constructors

/// A string of @p len bytes with an uninitialised, NUL-terminated buffer.
static PaykanString *string_alloc(int64_t len) {
  PaykanString *s = (PaykanString *)Paykan_malloc(sizeof(PaykanString));
  s->vtable = PaykanString_vtable;
  s->strong = 0; // unowned until boxed
  s->weak = 0;
  s->len = len;
  s->data = (char *)Paykan_malloc((size_t)len + 1);
  s->data[len] = '\0';
  return s;
}

PaykanString *PaykanString_new(const char *data, int64_t len) {
  PaykanString *s = string_alloc(len);
  memcpy(s->data, data, (size_t)len);
  return s;
}

PaykanString *PaykanString_from_int(int64_t value) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_uint(int64_t value) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_float(double value) {
  char buf[64];
  int n = Paykan_format_float(buf, sizeof(buf), value);
  return PaykanString_new(buf, n);
}

PaykanString *PaykanString_from_bool(int64_t value) {
  if (value)
    return PaykanString_new("True", 4);
  return PaykanString_new("False", 5);
}

PaykanString *PaykanString_from_char(int8_t c) {
  return PaykanString_new((const char *)&c, 1);
}

// -- Parses (Str's toInt() & co.)

/// True when @p s starts with a character strtoll / strtod would skip as
/// leading whitespace, which every parse rejects.
static int string_starts_with_space(const PaykanString *s) {
  return s->len > 0 && isspace((unsigned char)s->data[0]);
}

int64_t PaykanString_parse_int(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0;
  if (s->len == 0 || string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  long long val = strtoll(s->data, &end, 10);
  // The whole string must be consumed (an embedded NUL ends strtoll early)
  // and the value must fit (ERANGE).
  if (end != s->data + s->len || errno != 0)
    return 0;
  *out = (int64_t)val;
  return 1;
}

int64_t PaykanString_parse_uint(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0;
  // strtoull accepts a sign and negates: only digits are an unsigned number.
  if (s->len == 0 || s->data[0] == '-' || s->data[0] == '+' ||
      string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  unsigned long long val = strtoull(s->data, &end, 10);
  if (end != s->data + s->len || errno != 0)
    return 0;
  *out = (int64_t)val;
  return 1;
}

int64_t PaykanString_parse_float(PaykanObject *str, double *out) {
  PaykanString *s = (PaykanString *)str;
  *out = 0.0;
  if (s->len == 0 || string_starts_with_space(s))
    return 0;
  char *end;
  errno = 0;
  double val = strtod(s->data, &end);
  if (end != s->data + s->len)
    return 0;
  // ERANGE flags overflow (±HUGE_VAL), a nonzero value underflowing to 0, and
  // also a subnormal result, which is representable and accepted.
  if (errno != 0 && (errno != ERANGE || isinf(val) || val == 0.0))
    return 0;
  *out = val;
  return 1;
}

/// Whether @p s is an integer text: an optional sign, then digits, the
/// whole string.
static int string_is_integer(const PaykanString *s) {
  int64_t i = s->len > 0 && (s->data[0] == '+' || s->data[0] == '-');
  if (i == s->len)
    return 0;
  for (; i < s->len; ++i)
    if (s->data[i] < '0' || s->data[i] > '9')
      return 0;
  return 1;
}

/// Bit @p b of the little-endian 32-bit limbs @p limb.
static unsigned limb_bit(const uint32_t *limb, int b) {
  return (limb[b / 32] >> (b % 32)) & 1u;
}

/// The integer text @p s (string_is_integer) rounded once to the nearest
/// f32, ties to even, as `f32(n)` rounds an integer, at any magnitude:
/// through its f64 it could be rounded twice from 2^53 on.  Refused (0) when
/// it rounds to infinity in f32; "-0" is -0, as strtod reads it.
static int64_t string_integer_to_f32(const PaykanString *s, double *out) {
  enum { kLimbs = 5 }; // 160 bits hold every magnitude of 39 digits
  uint32_t limb[kLimbs] = {0};
  const int negative = s->data[0] == '-';
  int64_t i = negative || s->data[0] == '+';
  while (i < s->len && s->data[i] == '0')
    ++i;
  // 40 digits are at least 10^39, beyond the f32 range.
  if (s->len - i > 39) {
    *out = 0.0;
    return 0;
  }
  for (; i < s->len; ++i) {
    uint64_t carry = (uint64_t)(s->data[i] - '0');
    for (int k = 0; k < kLimbs; ++k) {
      uint64_t v = (uint64_t)limb[k] * 10u + carry;
      limb[k] = (uint32_t)v;
      carry = v >> 32;
    }
  }
  // The magnitude's top 24 bits, rounded on the bit after them and the
  // bits below that.
  int bits = 32 * kLimbs;
  while (bits > 0 && !limb_bit(limb, bits - 1))
    --bits;
  int shift = bits > 24 ? bits - 24 : 0;
  uint32_t mantissa = 0;
  for (int b = bits - 1; b >= shift; --b)
    mantissa = mantissa << 1 | limb_bit(limb, b);
  if (shift > 0 && limb_bit(limb, shift - 1)) {
    unsigned below = 0;
    for (int b = 0; b < shift - 1 && !below; ++b)
      below = limb_bit(limb, b);
    if (below || (mantissa & 1u)) {
      ++mantissa;
      if (mantissa == (UINT32_C(1) << 24)) {
        mantissa >>= 1;
        ++shift;
      }
    }
  }
  // The largest f32 is (2^24 - 1) * 2^104.
  if (shift > 104) {
    *out = 0.0;
    return 0;
  }
  double value = ldexp((double)mantissa, shift);
  *out = negative ? -value : value;
  return 1;
}

/// `s.toF32()`: an integer text is that integer rounded once (as `f32(n)`);
/// any other text is PaykanString_parse_float's value rounded to f32 (C's
/// (float)strtod).  Finite text whose value rounds to infinity in f32 is
/// refused; "inf" and "nan" are kept; a value below the f32 range is 0 or a
/// subnormal, as an f32 literal is.
int64_t PaykanString_parse_f32(PaykanObject *str, double *out) {
  if (string_is_integer((PaykanString *)str))
    return string_integer_to_f32((PaykanString *)str, out);
  double d;
  if (!PaykanString_parse_float(str, &d)) {
    *out = 0.0;
    return 0;
  }
  // From 2^128 - 2^103, the midpoint above FLT_MAX (a tie, rounded to even,
  // that is up), a value rounds to infinity; the guard also keeps the
  // conversion below in range, as C11 6.3.1.5 requires.
  if (!isinf(d) && !isnan(d) && fabs(d) >= 0x1.ffffffp+127) {
    *out = 0.0;
    return 0;
  }
  *out = isinf(d) || isnan(d) ? d : (double)(float)d;
  return 1;
}

/// `s.toF16()`: PaykanString_parse_float's value rounded to f16
/// (Paykan_round_f16); finite text that rounds to infinity is refused.
int64_t PaykanString_parse_f16(PaykanObject *str, double *out) {
  double d;
  if (!PaykanString_parse_float(str, &d)) {
    *out = 0.0;
    return 0;
  }
  double r = Paykan_round_f16(d);
  if (isinf(r) && !isinf(d)) {
    *out = 0.0;
    return 0;
  }
  *out = r;
  return 1;
}

int64_t PaykanString_parse_bool(PaykanObject *str, int64_t *out) {
  PaykanString *s = (PaykanString *)str;
  // Exactly the spellings `Str<bool>` prints, so the two round-trip.
  *out = s->len == 4 && memcmp(s->data, "True", 4) == 0;
  return *out || (s->len == 5 && memcmp(s->data, "False", 5) == 0);
}

// -- Methods

static void string_check_index(const PaykanString *s, int64_t idx) {
  if (idx < 0 || idx >= s->len) {
    Paykan_runtime_panic("string index %lld out of bounds (len=%lld)",
                         (long long)idx, (long long)s->len);
  }
}

int8_t PaykanString_char_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  string_check_index(s, idx);
  return (int8_t)s->data[idx];
}

PaykanShared *PaykanString_at(PaykanObject *self, int64_t idx) {
  PaykanString *s = (PaykanString *)self;
  string_check_index(s, idx);
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data + idx, 1));
}

// The runtime frees the object itself after the destroy (Runtime.h,
// "Shared"); a string made and dropped unboxed goes through Paykan_dispose.
void PaykanString_destroy(PaykanObject *self) {
  PaykanString *s = (PaykanString *)self;
  if (s)
    Paykan_free(s->data);
}

PaykanShared *PaykanString_toString(PaykanObject *self) {
  // A fresh copy, so the caller owns an independent object.
  PaykanString *s = (PaykanString *)self;
  return PaykanShared_new((PaykanObject *)PaykanString_new(s->data, s->len));
}

int64_t PaykanString_equals(PaykanObject *self, PaykanShared *other,
                            uint64_t owned) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  PaykanString *lhs = (PaykanString *)self;
  int64_t result;
  if (!o) {
    result = 0; // NULL box: equal to nothing
  } else if (o->vtable != PaykanString_vtable) {
    result = (self == o); // not a String: identity
  } else {
    PaykanString *rhs = (PaykanString *)o;
    result = lhs->len == rhs->len &&
             memcmp(lhs->data, rhs->data, (size_t)lhs->len) == 0;
  }
  return Paykan_equals_drop_other(other, owned, result);
}

int64_t PaykanString_length(PaykanObject *self) {
  return ((PaykanString *)self)->len;
}

PaykanObject *PaykanString_concat(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  PaykanString *s = string_alloc(lhs->len + rhs->len);
  memcpy(s->data, lhs->data, (size_t)lhs->len);
  memcpy(s->data + lhs->len, rhs->data, (size_t)rhs->len);
  return (PaykanObject *)s;
}

// `s.concat(t)`.  Every string's bytes are a block of its own from
// Paykan_malloc (string_alloc; a literal's use is a copy of its bytes), so
// the block grows by a realloc, which the C library can often do where it
// is: appending to a string does not copy it each time.  @p other may be
// @p self, whose bytes are read after the realloc.
void PaykanString_concat_inplace(PaykanObject *self, PaykanObject *other) {
  PaykanString *lhs = (PaykanString *)self;
  PaykanString *rhs = (PaykanString *)other;
  int64_t oldLen = lhs->len;
  int64_t newLen = oldLen + rhs->len;
  lhs->data = (char *)Paykan_realloc(lhs->data, (size_t)newLen + 1);
  memcpy(lhs->data + oldLen, rhs->data, (size_t)(newLen - oldLen));
  lhs->data[newLen] = '\0';
  lhs->len = newLen;
}

// -- The standard library's ::collections (its string functions)
//
// An index is the standard library's `unsigned`, in an int64_t: one of 2^63
// or more is negative here, so each is compared as a uint64_t, and a panic
// prints it as the program wrote it.

PaykanShared *PaykanString_slice(PaykanObject *self, int64_t from, int64_t to) {
  PaykanString *s = (PaykanString *)self;
  if ((uint64_t)from > (uint64_t)to || (uint64_t)to > (uint64_t)s->len)
    Paykan_runtime_panic("string slice %llu..%llu out of bounds (len=%lld)",
                         (unsigned long long)from, (unsigned long long)to,
                         (long long)s->len);
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s->data + from, to - from));
}

int64_t PaykanString_find(PaykanObject *self, PaykanObject *needle,
                          int64_t from) {
  const PaykanString *s = (const PaykanString *)self;
  const PaykanString *n = (const PaykanString *)needle;
  if ((uint64_t)from > (uint64_t)s->len || n->len > s->len - from)
    return -1;
  if (n->len == 0)
    return from;
  // Each place the needle's first byte is, up to the last one the whole
  // needle fits after, compared with the rest of it.
  const char *at = s->data + from;
  const char *last = s->data + (s->len - n->len);
  while (at <= last) {
    at = (const char *)memchr(at, n->data[0], (size_t)(last - at) + 1);
    if (!at)
      return -1;
    if (memcmp(at + 1, n->data + 1, (size_t)n->len - 1) == 0)
      return at - s->data;
    ++at;
  }
  return -1;
}
