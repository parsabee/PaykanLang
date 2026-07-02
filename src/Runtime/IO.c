// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — I/O builtins.

#include "Runtime.h"
#include <stdio.h>

// ---------------------------------------------------------------------------
// Common helper — print one object to a stream, converted to a string via its
// vtable's toString method.  (Paykan has no variadic functions; the print
// builtins take a single Obj — compose with `+` for multiple pieces.)
// ---------------------------------------------------------------------------
static void print_object(FILE *stream, PaykanObject *obj) {
  if (!obj)
    return;
  PaykanShared *shared = obj->vtable->toString(obj);
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
