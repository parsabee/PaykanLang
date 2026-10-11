// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The native functions of text.pkn, written against the Paykan runtime ABI
// (Runtime.h): a Str argument is a borrowed PaykanObject *, a Str result an
// owned PaykanShared * (NULL is None for a Str? result).

#include "Runtime.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static PaykanShared *newStr(const char *data, int64_t len) {
  return PaykanShared_new((PaykanObject *)PaykanString_new(data, len));
}

PaykanShared *sample_text_upper(PaykanObject *s) {
  const PaykanString *str = (const PaykanString *)s;
  char *buf = malloc((size_t)str->len + 1);
  for (int64_t i = 0; i < str->len; ++i)
    buf[i] = (char)toupper((unsigned char)str->data[i]);
  PaykanShared *r = newStr(buf, str->len);
  free(buf);
  return r;
}

int64_t sample_text_count(PaykanObject *s, int8_t c) {
  const PaykanString *str = (const PaykanString *)s;
  int64_t n = 0;
  for (int64_t i = 0; i < str->len; ++i)
    n += str->data[i] == c;
  return n;
}

PaykanShared *sample_text_find(PaykanObject *s, PaykanObject *needle) {
  const char *hit = strstr(((const PaykanString *)s)->data,
                           ((const PaykanString *)needle)->data);
  if (!hit)
    return NULL; // None
  return newStr(hit, (int64_t)strlen(hit));
}
