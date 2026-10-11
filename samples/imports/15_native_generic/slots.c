// 15_native_generic — the C side of main.pkn's generic native functions.
//
// Each gets the kind of its type argument first (PaykanTypeKind), then every
// argument as a 64-bit slot: a value's bits, or a reference's box, borrowed
// for the call.  A reference result is returned owned (+1).

#include "Runtime.h"

#include <stdio.h>
#include <string.h>

static int64_t new_str(const char *text) {
  PaykanString *s = PaykanString_new(text, (int64_t)strlen(text));
  return (int64_t)(intptr_t)PaykanShared_new((PaykanObject *)s);
}

int64_t sample_describe(int64_t kind, int64_t x) {
  char buf[64];
  switch (kind) {
  case PAYKAN_KIND_INT:
    snprintf(buf, sizeof buf, "int %lld", (long long)x);
    break;
  case PAYKAN_KIND_FLOAT: {
    double d;
    memcpy(&d, &x, sizeof d);
    snprintf(buf, sizeof buf, "float %g", d);
    break;
  }
  case PAYKAN_KIND_BOOL:
    snprintf(buf, sizeof buf, "bool %s", x ? "True" : "False");
    break;
  case PAYKAN_KIND_CHAR:
    snprintf(buf, sizeof buf, "char %c", (char)x);
    break;
  case PAYKAN_KIND_OBJECT:
    snprintf(buf, sizeof buf, "an object");
    break;
  default: // PAYKAN_KIND_OPTIONAL
    snprintf(buf, sizeof buf, "%s", x ? "an object" : "None");
    break;
  }
  return new_str(buf);
}

int64_t sample_pick(int64_t kind, int64_t first, int64_t a, int64_t b) {
  int64_t r = first ? a : b;
  if ((kind == PAYKAN_KIND_OBJECT || kind == PAYKAN_KIND_OPTIONAL) && r)
    Paykan_retain((PaykanShared *)(intptr_t)r); // the result is owned
  return r;
}
