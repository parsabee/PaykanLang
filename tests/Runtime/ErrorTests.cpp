// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Unit tests for the PaykanError runtime type.

#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

#include "RuntimeEqualsHelper.h"

// -- Helpers

static const char *sharedStr(PaykanShared *s) {
  return ((PaykanString *)PaykanShared_get(s))->data;
}

// -- PaykanError

TEST(ErrorObject, New) {
  const char *msg = "something went wrong";
  PaykanError *e = PaykanError_new(msg, (int64_t)strlen(msg));
  ASSERT_NE(e, nullptr);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(ErrorObject, VtableIsErrorVtable) {
  PaykanError *e = PaykanError_new("err", 3);
  EXPECT_EQ(e->vtable, PaykanError_vtable);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(ErrorObject, ToStringContainsMessage) {
  PaykanError *e = PaykanError_new("oops", 4);
  PaykanShared *s = PaykanError_toString((PaykanObject *)e);
  ASSERT_NE(s, nullptr);
  std::string str = sharedStr(s);
  EXPECT_NE(str.find("oops"), std::string::npos);
  Paykan_release(s);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(ErrorObject, EqualsSelf) {
  PaykanError *e = PaykanError_new("err", 3);
  EXPECT_EQ(paykanTestEquals(PaykanError_equals, (PaykanObject *)e,
                             (PaykanObject *)e),
            1);
  PaykanError_destroy((PaykanObject *)e);
}

TEST(ErrorObject, NotEqualDifferentErrors) {
  PaykanError *a = PaykanError_new("a", 1);
  PaykanError *b = PaykanError_new("b", 1);
  EXPECT_EQ(paykanTestEquals(PaykanError_equals, (PaykanObject *)a,
                             (PaykanObject *)b),
            0);
  PaykanError_destroy((PaykanObject *)a);
  PaykanError_destroy((PaykanObject *)b);
}
