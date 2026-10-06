// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — File type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// -- VTable

PaykanMethod PaykanFile_vtable[PAYKAN_FILE_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanFile_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanFile_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanFile_equals,
    [PAYKAN_SLOT_FILE_WRITE] = (PaykanMethod)PaykanFile_write,
    [PAYKAN_SLOT_FILE_READLN] = (PaykanMethod)PaykanFile_readln,
    [PAYKAN_SLOT_FILE_READBYTES] = (PaykanMethod)PaykanFile_readbytes,
    [PAYKAN_SLOT_FILE_READ] = (PaykanMethod)PaykanFile_read,
};

// -- Constructor

PaykanFile *PaykanFile_new(void) {
  PaykanFile *f = (PaykanFile *)Paykan_malloc(sizeof(PaykanFile));
  f->vtable = PaykanFile_vtable;
  f->shared = NULL; // not yet boxed (unique-box invariant)
  f->handle = NULL;
  return f;
}

// -- Methods

PaykanShared *PaykanFile_open(PaykanObject *pathObj, PaykanObject *modeObj) {
  PaykanString *path = (PaykanString *)pathObj;
  PaykanString *mode = (PaykanString *)modeObj;
  FILE *handle = fopen(path->data, mode->data);
  if (!handle) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "open(\"%s\", \"%s\"): %s", path->data,
                     mode->data, strerror(errno));
    return PaykanShared_new((PaykanObject *)PaykanError_new(buf, (int64_t)n));
  }
  PaykanFile *f = PaykanFile_new();
  f->handle = handle;
  return PaykanShared_new((PaykanObject *)f);
}

void PaykanFile_destroy(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (f->handle)
    fclose(f->handle);
  Paykan_free(f);
}

PaykanShared *PaykanFile_toString(PaykanObject *self) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "File@%p", (void *)self);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanFile_equals(PaykanObject *self, PaykanShared *other) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_consume_other(other, result);
}

/// Whether @p f is closed, which a read or write reports and skips.
static int file_closed(const PaykanFile *f, const char *op) {
  if (f->handle)
    return 0;
  fprintf(stderr, "paykan: %s on closed File\n", op);
  return 1;
}

/// The `Str?` result of a read: the first @p used bytes of @p buf as a fresh
/// Str, or None when nothing was read.  Frees @p buf.
static PaykanShared *file_read_result(char *buf, size_t used) {
  PaykanShared *result =
      used ? PaykanShared_new(
                 (PaykanObject *)PaykanString_new(buf, (int64_t)used))
           : PaykanShared_new(&PaykanObject_None);
  Paykan_free(buf);
  return result;
}

void PaykanFile_write(PaykanObject *self, PaykanObject *strObj) {
  PaykanFile *f = (PaykanFile *)self;
  PaykanString *str = (PaykanString *)strObj;
  if (file_closed(f, "write"))
    return;
  fwrite(str->data, 1, (size_t)str->len, f->handle);
}

PaykanShared *PaykanFile_readln(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (file_closed(f, "readln"))
    return PaykanShared_new(&PaykanObject_None);
  // One line, including the trailing '\n' when there is one.
  size_t cap = 128;
  size_t used = 0;
  char *buf = (char *)Paykan_malloc(cap);
  int c;
  while ((c = fgetc(f->handle)) != EOF) {
    if (used + 1 >= cap) {
      cap *= 2;
      buf = (char *)Paykan_realloc(buf, cap);
    }
    buf[used++] = (char)c;
    if (c == '\n')
      break;
  }
  buf[used] = '\0';
  return file_read_result(buf, used);
}

PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n) {
  PaykanFile *f = (PaykanFile *)self;
  if (file_closed(f, "readbytes") || n <= 0)
    return PaykanShared_new(&PaykanObject_None);
  char *buf = (char *)Paykan_malloc((size_t)n);
  return file_read_result(buf, fread(buf, 1, (size_t)n, f->handle));
}

PaykanShared *PaykanFile_read(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (file_closed(f, "read"))
    return PaykanShared_new(&PaykanObject_None);
  size_t cap = 4096;
  size_t used = 0;
  char *buf = (char *)Paykan_malloc(cap);
  size_t got;
  while ((got = fread(buf + used, 1, cap - used, f->handle)) > 0) {
    used += got;
    if (used == cap) {
      cap *= 2;
      buf = (char *)Paykan_realloc(buf, cap);
    }
  }
  return file_read_result(buf, used);
}

// -- Stdin singleton: an immortal PaykanFile wrapping C's stdin.

static void PaykanStdin_destroy(PaykanObject *self) { (void)self; }

static PaykanMethod PaykanStdin_vtable[PAYKAN_FILE_SLOTS] = {
    [PAYKAN_SLOT_DESTROY] = (PaykanMethod)PaykanStdin_destroy,
    [PAYKAN_SLOT_TO_STRING] = (PaykanMethod)PaykanFile_toString,
    [PAYKAN_SLOT_EQUALS] = (PaykanMethod)PaykanFile_equals,
    [PAYKAN_SLOT_FILE_WRITE] = (PaykanMethod)PaykanFile_write,
    [PAYKAN_SLOT_FILE_READLN] = (PaykanMethod)PaykanFile_readln,
    [PAYKAN_SLOT_FILE_READBYTES] = (PaykanMethod)PaykanFile_readbytes,
    [PAYKAN_SLOT_FILE_READ] = (PaykanMethod)PaykanFile_read,
};

PaykanFile PaykanFile_Stdin = {
    .vtable = PaykanStdin_vtable,
    .shared = NULL, // boxed on demand; release re-clears it (immortal destroy)
    .handle = NULL, // stdin is not a constant expression: set at startup
};

__attribute__((constructor)) static void PaykanFile_Stdin_init(void) {
  PaykanFile_Stdin.handle = stdin;
}
