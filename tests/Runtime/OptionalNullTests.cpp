// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// NULL-box tolerance optional types (issue #5) rely on: a
// `T?` is a possibly-NULL PaykanShared*, so every runtime entry point that
// generated code may hand such a box to must accept NULL — reference
// counting, unboxing and printing.

#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

// -- Reference counting and unboxing

TEST(OptionalNull, RetainReleaseGetTolerateNull) {
  Paykan_retain(nullptr);
  Paykan_release(nullptr);
  EXPECT_EQ(PaykanShared_get(nullptr), nullptr);
}

TEST(OptionalNull, PrintTolerateNull) {
  // print_object returns early on a NULL object; must not crash.
  Paykan_print(nullptr);
  Paykan_println(nullptr);
  Paykan_printerr(nullptr);
  Paykan_printerrln(nullptr);
}

// `equals` with a NULL `other` box (the two-optional protocol never calls
// equals with NULL, but the runtime convention is documented as tolerant)

TEST(OptionalNull, EqualsWithNullOtherIsFalse) {
  PaykanObject *o = PaykanObject_new();
  EXPECT_EQ(PaykanObject_equals(o, nullptr), 0);
  PaykanString *s = PaykanString_new("x", 1);
  EXPECT_EQ(PaykanString_equals((PaykanObject *)s, nullptr), 0);
  Paykan_dispose((PaykanObject *)s);
  Paykan_dispose(o);
}
