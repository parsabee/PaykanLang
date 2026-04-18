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
    PaykanString *s = obj->vtable->toString(obj);
    if (s && s->data)
      fwrite(s->data, 1, (size_t)s->len, stream);
  }
  fputc('\n', stream);
}

void Paykan_out(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stdout, argc, ap);
  va_end(ap);
}

void Paykan_err(int64_t argc, ...) {
  va_list ap;
  va_start(ap, argc);
  print_objects(stderr, argc, ap);
  va_end(ap);
}
