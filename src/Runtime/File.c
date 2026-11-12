// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — File type implementation.

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A file is no object: an io::File (stdlib/io.pkn) is a struct holding the
// address of one of these as an `int`, and closes it in its `__deinit__`
// (Paykan_io_close).  Nothing counts it; the File that holds it is the one
// owner, which moves.

/// A new file over the open stream @p handle.
static PaykanFile *file_new(FILE *handle) {
  PaykanFile *f = (PaykanFile *)Paykan_malloc(sizeof(PaykanFile));
  f->handle = handle;
  f->lastOp = PAYKAN_FILE_OP_NONE;
  return f;
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

/// The next line of @p f, with its newline when it has one, or None.
static PaykanShared *file_readln(PaykanFile *f) {
  if (file_start_read(f))
    return PaykanShared_new(&PaykanObject_None);
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

/// The most bytes file_readbytes allocates before any are read.
#define PAYKAN_READBYTES_FIRST ((size_t)64 * 1024)

/// Up to @p n bytes of @p f, n positive, or None.
static PaykanShared *file_readbytes(PaykanFile *f, int64_t n) {
  if (file_start_read(f))
    return PaykanShared_new(&PaykanObject_None);
  // @p n is a maximum: the buffer starts at 64 KiB at most and doubles each
  // time a read fills it, up to n, so a large n on a short file allocates
  // about what is there.  fread gives fewer bytes than asked for only at the
  // end of the file or on an error, and nothing more is read after it (on a
  // terminal another read would wait for more input).
  size_t want = (size_t)n;
  size_t cap = want < PAYKAN_READBYTES_FIRST ? want : PAYKAN_READBYTES_FIRST;
  size_t used = 0;
  char *buf = (char *)Paykan_tryMalloc(cap);
  if (!buf)
    return file_read_no_memory(buf);
  for (;;) {
    used += fread(buf + used, 1, cap - used, f->handle);
    if (used < cap || used == want)
      break;
    size_t grown = cap <= want / 2 ? cap * 2 : want;
    char *more = (char *)Paykan_tryRealloc(buf, grown);
    if (!more)
      return file_read_no_memory(buf);
    buf = more;
    cap = grown;
  }
  return file_read_result(buf, used);
}

/// The rest of @p f, or None.
static PaykanShared *file_read(PaykanFile *f) {
  if (file_start_read(f))
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

// -- Stdin: the file over C's stdin, which nothing closes.

static PaykanFile PaykanFile_Stdin = {
    .handle = NULL, // stdin is not a constant expression: set at startup
    .lastOp = PAYKAN_FILE_OP_NONE,
};

__attribute__((constructor)) static void PaykanFile_Stdin_init(void) {
  PaykanFile_Stdin.handle = stdin;
}

// Every File that `io::stdin()` makes names that one file, and a File is
// Sendable: two tasks may each hold one.  A read or a write of it takes
// this lock, so `lastOp` has one writer at a time and a line read on one
// thread is whole.  Any other file has one File, which one thread holds.
static pthread_mutex_t stdin_lock = PTHREAD_MUTEX_INITIALIZER;

static void file_enter(const PaykanFile *f) {
  if (f == &PaykanFile_Stdin)
    pthread_mutex_lock(&stdin_lock);
}

static void file_leave(const PaykanFile *f) {
  if (f == &PaykanFile_Stdin)
    pthread_mutex_unlock(&stdin_lock);
}

// -- The standard library's ::io (stdlib/io.pkn)
//
// An io::File holds the address of its file as an `int`, which the natives
// bound to these functions take.  Each reports a failure as C's errno,
// which ::error turns into an Error: a write returns it (0 when it
// succeeded), a read returns NULL with it in `*err` (an int64_t, the
// native's `inout int`), 0 there meaning the end of the file.

/// The file an io::File's handle @p file names.  Only ::io makes a handle
/// (File's initializer and its field are the module's), so one that is
/// zero is the runtime's own mistake.
static PaykanFile *io_file(int64_t file, const char *op) {
  if (!file)
    Paykan_runtime_panic("io::File.%s on no file", op);
  // The native ABI passes a struct's `int` field as an int64_t: the
  // address Paykan_io_open returned in one.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  return (PaykanFile *)(intptr_t)file;
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

int64_t Paykan_io_open(PaykanObject *pathObj, PaykanObject *modeObj,
                       void *err) {
  PaykanString *path = (PaykanString *)pathObj;
  PaykanString *mode = (PaykanString *)modeObj;
  // fopen reads each up to its first NUL byte: a path or a mode holding one
  // would open another file, or in another mode.
  if (memchr(path->data, '\0', (size_t)path->len) ||
      memchr(mode->data, '\0', (size_t)mode->len)) {
    *(int64_t *)err = EINVAL;
    return 0;
  }
  FILE *handle = fopen(path->data, mode->data);
  if (!handle) {
    *(int64_t *)err = errno;
    return 0;
  }
  return (int64_t)(intptr_t)file_new(handle);
}

void Paykan_io_close(int64_t file) {
  if (!file)
    return;
  PaykanFile *f = io_file(file, "close");
  if (f == &PaykanFile_Stdin)
    return;
  // What the writes left in C's buffer is written out here; a File has no
  // way to report that it could not be (`__deinit__` returns nothing).
  (void)fclose(f->handle);
  Paykan_free(f);
}

int64_t Paykan_io_write(int64_t file, PaykanObject *str) {
  PaykanFile *f = io_file(file, "write");
  PaykanString *s = (PaykanString *)str;
  int err = 0;
  file_enter(f);
  if (file_write(f, s) != (size_t)s->len) {
    err = errno ? errno : EIO;
    // The failure is this call's, reported now: the next read of the stream
    // is not to mistake the error flag for its own.
    clearerr(f->handle);
  }
  file_leave(f);
  return err;
}

PaykanShared *Paykan_io_readln(int64_t file, void *err) {
  PaykanFile *f = io_file(file, "readln");
  file_enter(f);
  errno = 0;
  PaykanShared *r = io_read_result(f, file_readln(f), (int64_t *)err);
  file_leave(f);
  return r;
}

PaykanShared *Paykan_io_readbytes(int64_t file, int64_t n, void *err) {
  PaykanFile *f = io_file(file, "readbytes");
  if (n <= 0) {
    *(int64_t *)err = EINVAL;
    return (PaykanShared *)0;
  }
  file_enter(f);
  errno = 0;
  PaykanShared *r = io_read_result(f, file_readbytes(f, n), (int64_t *)err);
  file_leave(f);
  return r;
}

PaykanShared *Paykan_io_read(int64_t file, void *err) {
  PaykanFile *f = io_file(file, "read");
  file_enter(f);
  errno = 0;
  PaykanShared *r = io_read_result(f, file_read(f), (int64_t *)err);
  file_leave(f);
  return r;
}

int64_t Paykan_io_stdin(void) { return (int64_t)(intptr_t)&PaykanFile_Stdin; }
