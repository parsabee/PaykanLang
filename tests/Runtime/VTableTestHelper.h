// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Test helper for vtable dispatch.  A vtable is an array of PaykanMethod
// (Runtime.h); a slot is converted back to its method's own type before the
// call, exactly as the runtime and the generated code do.

#ifndef PAYKAN_VTABLE_TEST_HELPER_H
#define PAYKAN_VTABLE_TEST_HELPER_H

extern "C" {
#include "Runtime.h"
}

/// Slot @p slot of @p obj's vtable as a @p Fn.
template <typename Fn> Fn vtSlot(const void *obj, int slot) {
  return reinterpret_cast<Fn>(static_cast<const PaykanObject *>(obj)->vtable[slot]);
}

using PaykanDestroyFn = void (*)(PaykanObject *);
using PaykanToStringFn = PaykanShared *(*)(PaykanObject *);
using PaykanEqualsFn = int64_t (*)(PaykanObject *, PaykanShared *);
using PaykanLengthFn = int64_t (*)(PaykanObject *);

inline PaykanDestroyFn vtDestroy(const void *obj) {
  return vtSlot<PaykanDestroyFn>(obj, PAYKAN_SLOT_DESTROY);
}
inline PaykanToStringFn vtToString(const void *obj) {
  return vtSlot<PaykanToStringFn>(obj, PAYKAN_SLOT_TO_STRING);
}
inline PaykanEqualsFn vtEquals(const void *obj) {
  return vtSlot<PaykanEqualsFn>(obj, PAYKAN_SLOT_EQUALS);
}

#endif // PAYKAN_VTABLE_TEST_HELPER_H
