// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Unit tests for the runtime's errors (Runtime.h, "Errors"): what a C
// library failure's errno means to the standard library's ::result.

#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

TEST(ErrorCode, EveryErrnoMapsToItsCode) {
  EXPECT_EQ(Paykan_error_code(0), PAYKAN_ERROR_OK);
  EXPECT_EQ(Paykan_error_code(ENOENT), PAYKAN_ERROR_NOT_FOUND);
  EXPECT_EQ(Paykan_error_code(EACCES), PAYKAN_ERROR_PERMISSION_DENIED);
  EXPECT_EQ(Paykan_error_code(EPERM), PAYKAN_ERROR_PERMISSION_DENIED);
  EXPECT_EQ(Paykan_error_code(EEXIST), PAYKAN_ERROR_ALREADY_EXISTS);
  EXPECT_EQ(Paykan_error_code(EINVAL), PAYKAN_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(Paykan_error_code(EBADF), PAYKAN_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(Paykan_error_code(EISDIR), PAYKAN_ERROR_IS_A_DIRECTORY);
  EXPECT_EQ(Paykan_error_code(ENOTDIR), PAYKAN_ERROR_NOT_A_DIRECTORY);
  EXPECT_EQ(Paykan_error_code(EMFILE), PAYKAN_ERROR_TOO_MANY_OPEN_FILES);
  EXPECT_EQ(Paykan_error_code(ENFILE), PAYKAN_ERROR_TOO_MANY_OPEN_FILES);
  EXPECT_EQ(Paykan_error_code(ENOSPC), PAYKAN_ERROR_NO_SPACE);
  EXPECT_EQ(Paykan_error_code(EROFS), PAYKAN_ERROR_READ_ONLY);
  EXPECT_EQ(Paykan_error_code(EBUSY), PAYKAN_ERROR_BUSY);
  EXPECT_EQ(Paykan_error_code(EINTR), PAYKAN_ERROR_INTERRUPTED);
  EXPECT_EQ(Paykan_error_code(EAGAIN), PAYKAN_ERROR_WOULD_BLOCK);
  EXPECT_EQ(Paykan_error_code(EWOULDBLOCK), PAYKAN_ERROR_WOULD_BLOCK);
  EXPECT_EQ(Paykan_error_code(EPIPE), PAYKAN_ERROR_BROKEN_PIPE);
  EXPECT_EQ(Paykan_error_code(ETIMEDOUT), PAYKAN_ERROR_TIMED_OUT);
  EXPECT_EQ(Paykan_error_code(ENOMEM), PAYKAN_ERROR_OUT_OF_MEMORY);
  EXPECT_EQ(Paykan_error_code(ENOSYS), PAYKAN_ERROR_UNSUPPORTED);
  EXPECT_EQ(Paykan_error_code(ENOTSUP), PAYKAN_ERROR_UNSUPPORTED);
  EXPECT_EQ(Paykan_error_code(EIO), PAYKAN_ERROR_IO);
}

TEST(ErrorCode, AnUnknownErrnoIsOther) {
  EXPECT_EQ(Paykan_error_code(-1), PAYKAN_ERROR_OTHER);
  EXPECT_EQ(Paykan_error_code(1000000), PAYKAN_ERROR_OTHER);
}

TEST(ErrorText, IsStrerror) {
  PaykanShared *s = Paykan_error_text(ENOENT);
  auto *str = (PaykanString *)PaykanShared_get(s);
  EXPECT_EQ(std::string(str->data, (size_t)str->len), strerror(ENOENT));
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s);
}
