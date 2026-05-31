// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — I/O builtins.

#include "Runtime.h"
#include <stdio.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Common helper — print variadic PaykanObject arguments to a stream.
// Each argument is converted to a string via its vtable's toString method.
// ---------------------------------------------------------------------------
static void print_objects(FILE *stream, int64_t argc, va_list ap) {
  for (int64_t i = 0; i < argc; ++i) {
    PaykanObject *obj = va_arg(ap, PaykanObject *);
    if (!obj)
      continue;
    PaykanShared *shared = obj->vtable->toString(obj);
    PaykanString *s = (PaykanString *)PaykanShared_get(shared);
    if (s && s->data)
      fwrite(s->data, 1, (size_t)s->len, stream);
    Paykan_release(shared);
  }
}

void Paykan_print(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stdout, argc, ap);
  va_end(ap);
}

void Paykan_println(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stdout, argc, ap);
  va_end(ap);
  fputc('\n', stdout);
}

void Paykan_printerr(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stderr, argc, ap);
  va_end(ap);
}

void Paykan_printerrln(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stderr, argc, ap);
  va_end(ap);
  fputc('\n', stderr);
}
