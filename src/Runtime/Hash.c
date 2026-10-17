// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — `hash()` of the types that implement the builtin
// `Hashable` trait natively: int, bool, char (PaykanInt_hash) and Str
// (PaykanString_hash).  Both end in the SplitMix64 finalizer, so every bit of
// the input affects every bit of the result.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdint.h>

static uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

int64_t PaykanInt_hash(int64_t n) {
  return (int64_t)mix64((uint64_t)n + UINT64_C(0x9e3779b97f4a7c15));
}

int64_t PaykanString_hash(PaykanObject *self) {
  const PaykanString *s = (const PaykanString *)self;
  // FNV-1a over the bytes, then the finalizer.
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  for (int64_t i = 0; i < s->len; ++i) {
    h ^= (unsigned char)s->data[i];
    h *= UINT64_C(0x100000001b3);
  }
  return (int64_t)mix64(h ^ (uint64_t)s->len);
}
