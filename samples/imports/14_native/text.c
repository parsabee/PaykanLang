// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The native functions of text.pkn, written against the Paykan runtime ABI
// (Runtime.h): a `view` Str argument is a borrowed PaykanObject *, an
// `inout` one the address of the caller's PaykanShared *, and a Str result
// an owned PaykanShared * (NULL is None for a Str? result).

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

// Replace the caller's string with its trimmed copy: the new box is the
// caller's to own, and the old one is released.
void sample_text_trim(PaykanShared **s) {
  const PaykanString *str = (const PaykanString *)PaykanShared_get(*s);
  int64_t start = 0, end = str->len;
  while (start < end && isspace((unsigned char)str->data[start]))
    ++start;
  while (end > start && isspace((unsigned char)str->data[end - 1]))
    --end;
  PaykanShared *trimmed = newStr(str->data + start, end - start);
  Paykan_release(*s);
  *s = trimmed;
}
