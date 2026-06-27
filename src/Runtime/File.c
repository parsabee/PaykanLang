// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — File type implementation.
//
// File is a builtin class that inherits Obj.  Its vtable mirrors
// PaykanObjectVTable (destroy / toString / equals); no File-specific
// methods exist yet.

#include "Runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Forward declarations
// ============================================================================

void PaykanFile_destroy(PaykanObject *self);
PaykanShared *PaykanFile_toString(PaykanObject *self);
int64_t PaykanFile_equals(PaykanObject *self, PaykanObject *other);
void PaykanFile_write(PaykanObject *self, PaykanObject *str);
PaykanShared *PaykanFile_readln(PaykanObject *self);
PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n);
PaykanShared *PaykanFile_read(PaykanObject *self);
PaykanShared *PaykanFile_open(PaykanObject *path, PaykanObject *mode);

// ============================================================================
// VTable
// ============================================================================

PaykanFileVTable PaykanFile_vtable = {
    .destroy = PaykanFile_destroy,
    .toString = PaykanFile_toString,
    .equals = PaykanFile_equals,
    .write = PaykanFile_write,
    .readln = PaykanFile_readln,
    .readbytes = PaykanFile_readbytes,
    .read = PaykanFile_read,
};

// ============================================================================
// Constructor
// ============================================================================

PaykanFile *PaykanFile_new(void) {
  PaykanFile *f = (PaykanFile *)Paykan_malloc(sizeof(PaykanFile));
  f->vtable = (PaykanObjectVTable *)&PaykanFile_vtable;
  f->handle = NULL;
  return f;
}

// ============================================================================
// Method implementations
// ============================================================================

PaykanShared *PaykanFile_open(PaykanObject *pathObj, PaykanObject *modeObj) {
  PaykanString *path = (PaykanString *)pathObj;
  PaykanString *mode = (PaykanString *)modeObj;
  FILE *handle = fopen(path->data, mode->data);
  if (!handle) {
    // Build a human-readable error message and wrap it in an Error object.
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

int64_t PaykanFile_equals(PaykanObject *self, PaykanObject *other) {
  return self == other;
}

void PaykanFile_write(PaykanObject *self, PaykanObject *strObj) {
  PaykanFile *f = (PaykanFile *)self;
  PaykanString *str = (PaykanString *)strObj;
  if (!f->handle) {
    fprintf(stderr, "paykan: write on closed File\n");
    return;
  }
  fwrite(str->data, 1, (size_t)str->len, f->handle);
}

PaykanShared *PaykanFile_readln(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (!f->handle) {
    fprintf(stderr, "paykan: readln on closed File\n");
    return PaykanShared_new(&PaykanObject_None);
  }
  // Read one line (including the trailing '\n' if present).
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
  // EOF with no bytes read — return None to signal end-of-file.
  if (used == 0) {
    Paykan_free(buf);
    return PaykanShared_new(&PaykanObject_None);
  }
  PaykanShared *result =
      PaykanShared_new((PaykanObject *)PaykanString_new(buf, (int64_t)used));
  Paykan_free(buf);
  return result;
}

PaykanShared *PaykanFile_readbytes(PaykanObject *self, int64_t n) {
  PaykanFile *f = (PaykanFile *)self;
  if (!f->handle || n <= 0) {
    if (!f->handle)
      fprintf(stderr, "paykan: readbytes on closed File\n");
    return PaykanShared_new(&PaykanObject_None);
  }
  char *buf = (char *)Paykan_malloc((size_t)n);
  int64_t got = (int64_t)fread(buf, 1, (size_t)n, f->handle);
  if (got == 0) {
    Paykan_free(buf);
    return PaykanShared_new(&PaykanObject_None);
  }
  PaykanShared *result =
      PaykanShared_new((PaykanObject *)PaykanString_new(buf, got));
  Paykan_free(buf);
  return result;
}

PaykanShared *PaykanFile_read(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (!f->handle) {
    fprintf(stderr, "paykan: read on closed File\n");
    return PaykanShared_new(&PaykanObject_None);
  }
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
  if (used == 0) {
    Paykan_free(buf);
    return PaykanShared_new(&PaykanObject_None);
  }
  PaykanShared *result =
      PaykanShared_new((PaykanObject *)PaykanString_new(buf, (int64_t)used));
  Paykan_free(buf);
  return result;
}

// ============================================================================
// Stdin singleton
// ============================================================================
//
// PaykanFile_Stdin is an immortal PaykanFile wrapping C's stdin.
// Its destroy is a no-op so it is never freed.

static void PaykanStdin_destroy(PaykanObject *self) { (void)self; }

static PaykanFileVTable PaykanStdin_vtable = {
    .destroy = PaykanStdin_destroy,
    .toString = PaykanFile_toString,
    .equals = PaykanFile_equals,
    .write = PaykanFile_write,
    .readln = PaykanFile_readln,
    .readbytes = PaykanFile_readbytes,
    .read = PaykanFile_read,
};

PaykanFile PaykanFile_Stdin = {
    .vtable = (PaykanObjectVTable *)&PaykanStdin_vtable,
    .handle = NULL, // set to stdin at startup via __attribute__((constructor))
};

__attribute__((constructor)) static void PaykanFile_Stdin_init(void) {
  PaykanFile_Stdin.handle = stdin;
}
