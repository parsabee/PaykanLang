// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — errors (Runtime.h, "Errors"): what a C library failure
// means to the standard library's ::result (stdlib/result.pkn), whose Error is
// a value the Paykan code makes from these.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <errno.h>
#include <string.h>

int64_t Paykan_error_code(int64_t err) {
  switch (err) {
  case 0:
    return PAYKAN_ERROR_OK;
  case ENOENT:
    return PAYKAN_ERROR_NOT_FOUND;
  case EACCES:
  case EPERM:
    return PAYKAN_ERROR_PERMISSION_DENIED;
  case EEXIST:
    return PAYKAN_ERROR_ALREADY_EXISTS;
  case EINVAL:
  case EBADF: // a file not open for the operation, or closed
    return PAYKAN_ERROR_INVALID_ARGUMENT;
  case EISDIR:
    return PAYKAN_ERROR_IS_A_DIRECTORY;
  case ENOTDIR:
    return PAYKAN_ERROR_NOT_A_DIRECTORY;
  case EMFILE:
  case ENFILE:
    return PAYKAN_ERROR_TOO_MANY_OPEN_FILES;
  case ENOSPC:
    return PAYKAN_ERROR_NO_SPACE;
  case EROFS:
    return PAYKAN_ERROR_READ_ONLY;
  case EBUSY:
    return PAYKAN_ERROR_BUSY;
  case EINTR:
    return PAYKAN_ERROR_INTERRUPTED;
  case EAGAIN:
#if EWOULDBLOCK != EAGAIN
  case EWOULDBLOCK:
#endif
    return PAYKAN_ERROR_WOULD_BLOCK;
  case EPIPE:
    return PAYKAN_ERROR_BROKEN_PIPE;
  case ETIMEDOUT:
    return PAYKAN_ERROR_TIMED_OUT;
  case ENOMEM:
    return PAYKAN_ERROR_OUT_OF_MEMORY;
  case ENOSYS:
  case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
  case EOPNOTSUPP:
#endif
    return PAYKAN_ERROR_UNSUPPORTED;
  case EIO:
    return PAYKAN_ERROR_IO;
  default:
    return PAYKAN_ERROR_OTHER;
  }
}

PaykanShared *Paykan_error_text(int64_t err) {
  const char *text = strerror((int)err);
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(text, (int64_t)strlen(text)));
}
