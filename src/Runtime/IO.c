// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — I/O builtins and the program's arguments.

#include "RuntimeInternal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Print one object to @p stream through its toString.  (The print builtins
/// take a single Obj: pieces are composed with `+`.)
static void print_object(FILE *stream, PaykanObject *obj) {
  if (!obj)
    return;
  PaykanShared *shared = Paykan_vcall_toString(obj);
  PaykanString *s = (PaykanString *)PaykanShared_get(shared);
  if (s && s->data)
    fwrite(s->data, 1, (size_t)s->len, stream);
  Paykan_release(shared);
}

void Paykan_print(PaykanObject *obj) { print_object(stdout, obj); }

void Paykan_println(PaykanObject *obj) {
  print_object(stdout, obj);
  fputc('\n', stdout);
}

void Paykan_printerr(PaykanObject *obj) { print_object(stderr, obj); }

void Paykan_printerrln(PaykanObject *obj) {
  print_object(stderr, obj);
  fputc('\n', stderr);
}

void Paykan_flush(void) { fflush(stdout); }

// -- Program arguments

PaykanShared **Paykan_args_new(int argc, char **argv, int64_t *count) {
  int64_t n = getenv("PAYKAN_NO_ARGS") ? 0 : argc;
  PaykanShared **args = (PaykanShared **)Paykan_malloc((size_t)(n > 0 ? n : 1) *
                                                       sizeof(PaykanShared *));
  for (int64_t i = 0; i < n; ++i)
    args[i] = PaykanShared_new(
        (PaykanObject *)PaykanString_new(argv[i], (int64_t)strlen(argv[i])));
  *count = n;
  return args;
}

void Paykan_args_free(PaykanShared **args, int64_t count) {
  for (int64_t i = 0; i < count; ++i)
    Paykan_release(args[i]);
  Paykan_free(args);
}
