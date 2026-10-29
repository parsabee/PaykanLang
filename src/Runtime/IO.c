// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Paykan runtime — I/O builtins and the program's arguments.

// unsetenv is POSIX, not C: declared on the systems that have it, by the
// feature-test macro their headers read.
#if (defined(__unix__) || defined(__APPLE__)) && !defined(_POSIX_C_SOURCE)
// NOLINTNEXTLINE(bugprone-reserved-identifier)
#define _POSIX_C_SOURCE 200809L
#endif

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
  PaykanString *s = (PaykanString *)PaykanShared_get_inline(shared);
  if (s && s->data)
    fwrite(s->data, 1, (size_t)s->len, stream);
  Paykan_release_inline(shared);
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

/// Set by the plugin host for a program it runs with no arguments, which it
/// starts with one empty argv[0] (paykan::names::kPaykanHostNoArgsEnv in
/// include/Names.h).
#define PAYKAN_HOST_NO_ARGS "__PAYKAN_HOST_NO_ARGS"

PaykanShared **Paykan_args_new(int argc, char **argv, int64_t *count) {
  int64_t n = argc;
  if (getenv(PAYKAN_HOST_NO_ARGS)) {
    // The empty argv[0] the host supplied is no argument.  The variable says
    // nothing about a program started with arguments, and is unset where
    // the system can, so the programs this one starts do not inherit it.
    if (argc == 1 && argv[0][0] == '\0')
      n = 0;
#if defined(__unix__) || defined(__APPLE__)
    unsetenv(PAYKAN_HOST_NO_ARGS);
#endif
  }
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
    Paykan_release_inline(args[i]);
  Paykan_free(args);
}
