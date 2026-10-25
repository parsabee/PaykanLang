// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
  f->strong = 0; // unowned until boxed
  f->weak = 0;
  f->handle = NULL;
  f->lastOp = PAYKAN_FILE_OP_NONE;
  return f;
}

// -- Methods

PaykanShared *PaykanFile_open(PaykanObject *pathObj, PaykanObject *modeObj,
                              int64_t *err) {
  PaykanString *path = (PaykanString *)pathObj;
  PaykanString *mode = (PaykanString *)modeObj;
  // fopen reads each up to its first NUL byte: a path or a mode holding one
  // would open another file, or in another mode.
  if (memchr(path->data, '\0', (size_t)path->len) ||
      memchr(mode->data, '\0', (size_t)mode->len)) {
    *err = EINVAL;
    return (PaykanShared *)0;
  }
  FILE *handle = fopen(path->data, mode->data);
  if (!handle) {
    *err = errno;
    return (PaykanShared *)0;
  }
  PaykanFile *f = PaykanFile_new();
  f->handle = handle;
  return PaykanShared_new((PaykanObject *)f);
}

void PaykanFile_destroy(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (f->handle)
    fclose(f->handle);
}

PaykanShared *PaykanFile_toString(PaykanObject *self) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "File@%p", (void *)self);
  return PaykanShared_new((PaykanObject *)PaykanString_new(buf, n));
}

int64_t PaykanFile_equals(PaykanObject *self, PaykanShared *other,
                          uint64_t owned) {
  PaykanObject *o = Paykan_equals_unbox_other(other);
  int64_t result = o && self == o;
  return Paykan_equals_drop_other(other, owned, result);
}

/// Whether @p f is closed, which a read or write reports and skips.
static int file_closed(const PaykanFile *f, const char *op) {
  if (f->handle)
    return 0;
  fprintf(stderr, "paykan: %s on closed File\n", op);
  return 1;
}

// C lets a read follow a write on a stream opened for update ("r+", "w+",
// "a+") only after an fflush or a file positioning call, and a write follow
// a read only after a positioning call, unless the read met the end of the
// file (C11 7.21.5.3).  Without them a C library may drop what the write
// left in its buffer (glibc does, under a read longer than the buffer).
// Every read and write of a PaykanFile goes through these two, which make
// the call when the stream switches.

/// Readies @p f's open stream for a read: 0, or EOF when what the writes
/// before it left in C's buffer cannot be written out.  That sets the
/// stream's error indicator and errno, for the read to report, and keeps
/// the stream a writing one: the next read tries again.
static int file_start_read(PaykanFile *f) {
  if (f->lastOp == PAYKAN_FILE_OP_WRITE && fflush(f->handle) == EOF)
    return EOF;
  f->lastOp = PAYKAN_FILE_OP_READ;
  return 0;
}

/// Writes @p s to @p f's open stream, with errno zeroed first: the bytes C
/// took.  `fseek(0, SEEK_CUR)` positions the stream where its reads left
/// off; a stream that cannot be positioned (a pipe, a terminal) fails that
/// with ESPIPE and is written where it is.
static size_t file_write(PaykanFile *f, const PaykanString *s) {
  if (f->lastOp == PAYKAN_FILE_OP_READ) {
    (void)fseek(f->handle, 0, SEEK_CUR);
    f->lastOp = PAYKAN_FILE_OP_NONE;
  }
  errno = 0;
  size_t n = fwrite(s->data, 1, (size_t)s->len, f->handle);
  // A write that took nothing (an empty one, or one the stream refuses)
  // leaves nothing buffered.
  if (n)
    f->lastOp = PAYKAN_FILE_OP_WRITE;
  return n;
}

/// None for a read whose buffer could not be allocated, with errno ENOMEM
/// (a read sets errno to 0 first, so its caller can tell).  Frees @p buf.
static PaykanShared *file_read_no_memory(char *buf) {
  Paykan_free(buf);
  errno = ENOMEM;
  return PaykanShared_new(&PaykanObject_None);
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
  (void)file_write(f, str);
}

PaykanShared *PaykanFile_readln(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (file_closed(f, "readln") || file_start_read(f))
    return PaykanShared_new(&PaykanObject_None);
  // One line, including the trailing '\n' when there is one.
  size_t cap = 128;
  size_t used = 0;
  char *buf = (char *)Paykan_tryMalloc(cap);
  if (!buf)
    return file_read_no_memory(buf);
  int c;
  while ((c = fgetc(f->handle)) != EOF) {
    if (used + 1 >= cap) {
      cap *= 2;
      char *grown = (char *)Paykan_tryRealloc(buf, cap);
      if (!grown)
        return file_read_no_memory(buf);
      buf = grown;
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
  if (file_closed(f, "readbytes") || n <= 0 || file_start_read(f))
    return PaykanShared_new(&PaykanObject_None);
  char *buf = (char *)Paykan_tryMalloc((size_t)n);
  if (!buf)
    return file_read_no_memory(buf);
  return file_read_result(buf, fread(buf, 1, (size_t)n, f->handle));
}

PaykanShared *PaykanFile_read(PaykanObject *self) {
  PaykanFile *f = (PaykanFile *)self;
  if (file_closed(f, "read") || file_start_read(f))
    return PaykanShared_new(&PaykanObject_None);
  size_t cap = 4096;
  size_t used = 0;
  char *buf = (char *)Paykan_tryMalloc(cap);
  if (!buf)
    return file_read_no_memory(buf);
  size_t got;
  while ((got = fread(buf + used, 1, cap - used, f->handle)) > 0) {
    used += got;
    if (used == cap) {
      cap *= 2;
      char *grown = (char *)Paykan_tryRealloc(buf, cap);
      if (!grown)
        return file_read_no_memory(buf);
      buf = grown;
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
    .strong = 1, // immortal: nothing releases this reference
    .weak = 0,
    .handle = NULL, // stdin is not a constant expression: set at startup
    .lastOp = PAYKAN_FILE_OP_NONE,
};

__attribute__((constructor)) static void PaykanFile_Stdin_init(void) {
  PaykanFile_Stdin.handle = stdin;
}

// -- The standard library's ::io (stdlib/io.pkn)
//
// An io::File holds one of these objects as its handle, which the natives
// bound to these functions take borrowed (`view Obj`).  Each reports a
// failure as C's errno, which ::error turns into an Error: a write returns
// it (0 when it succeeded), a read returns NULL with it in `*err` (an
// int64_t, the native's `inout int`), 0 there meaning the end of the file.

/// @p h as a file, or a panic naming @p op: an io::File made from an object
/// that is not one.
static PaykanFile *io_file(PaykanObject *h, const char *op) {
  if (!h || (Paykan_vtable_of(h) != PaykanFile_vtable &&
             Paykan_vtable_of(h) != PaykanStdin_vtable))
    Paykan_runtime_panic("io::File.%s on an object that is not a file", op);
  return (PaykanFile *)h;
}

/// A read's result as a `Str?`: the text, or NULL with the errno of the
/// failure in @p err (0 at the end of the file).  The caller zeroed errno:
/// ENOMEM there with no stream error is the read's own buffer failing.
static PaykanShared *io_read_result(PaykanFile *f, PaykanShared *r,
                                    int64_t *err) {
  if (r != &PaykanObject_None)
    return r;
  Paykan_release_inline(r);
  if (ferror(f->handle))
    *err = errno ? errno : EIO;
  else
    *err = errno == ENOMEM ? ENOMEM : 0;
  clearerr(f->handle);
  return (PaykanShared *)0;
}

PaykanShared *Paykan_io_open(PaykanObject *path, PaykanObject *mode,
                             void *err) {
  return PaykanFile_open(path, mode, (int64_t *)err);
}

int64_t Paykan_io_write(PaykanObject *file, PaykanObject *str) {
  PaykanFile *f = io_file(file, "write");
  PaykanString *s = (PaykanString *)str;
  if (!f->handle)
    return EBADF;
  if (file_write(f, s) != (size_t)s->len) {
    int err = errno ? errno : EIO;
    // The failure is this call's, reported now: the next read of the stream
    // is not to mistake the error flag for its own.
    clearerr(f->handle);
    return err;
  }
  return 0;
}

PaykanShared *Paykan_io_readln(PaykanObject *file, void *err) {
  PaykanFile *f = io_file(file, "readln");
  if (!f->handle) {
    *(int64_t *)err = EBADF;
    return (PaykanShared *)0;
  }
  errno = 0;
  return io_read_result(f, PaykanFile_readln((PaykanObject *)f),
                        (int64_t *)err);
}

PaykanShared *Paykan_io_readbytes(PaykanObject *file, int64_t n, void *err) {
  PaykanFile *f = io_file(file, "readbytes");
  if (!f->handle || n <= 0) {
    *(int64_t *)err = f->handle ? EINVAL : EBADF;
    return (PaykanShared *)0;
  }
  errno = 0;
  return io_read_result(f, PaykanFile_readbytes((PaykanObject *)f, n),
                        (int64_t *)err);
}

PaykanShared *Paykan_io_read(PaykanObject *file, void *err) {
  PaykanFile *f = io_file(file, "read");
  if (!f->handle) {
    *(int64_t *)err = EBADF;
    return (PaykanShared *)0;
  }
  errno = 0;
  return io_read_result(f, PaykanFile_read((PaykanObject *)f), (int64_t *)err);
}

PaykanShared *Paykan_io_stdin(void) {
  return PaykanShared_new((PaykanObject *)&PaykanFile_Stdin);
}
