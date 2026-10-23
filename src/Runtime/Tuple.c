// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Paykan runtime — rendering and comparing a tuple, a struct or an enum,
// values held in place, through their layouts (Runtime.h, "Tuples",
// "Structs" and "Enums").

#include "Runtime.h"
#include "RuntimeInternal.h"

#include <stdio.h>
#include <string.h>

/// The element of type T at @p at (read with memcpy: elements are aligned,
/// but the bytes are not declared as a T).
#define TUPLE_LOAD(T, name)                                                    \
  static T load_##name(const char *at) {                                       \
    T v;                                                                       \
    memcpy(&v, at, sizeof(T));                                                 \
    return v;                                                                  \
  }
TUPLE_LOAD(int64_t, int)
TUPLE_LOAD(double, float)
TUPLE_LOAD(uint8_t, byte)
TUPLE_LOAD(PaykanShared *, box)
#undef TUPLE_LOAD

/// The offset, in an inline optional, of a payload of @p kind.
static int64_t payload_offset(int64_t kind) {
  return kind == PAYKAN_KIND_BOOL || kind == PAYKAN_KIND_CHAR ? 1 : 8;
}

/// The words after the name at @p p: its length word, then its bytes
/// packed eight to a word.
static const int64_t *skip_name(const int64_t *p) {
  return p + 1 + (p[0] + 7) / 8;
}

static const int64_t *next_entry(const int64_t *e);

/// The words after the struct layout at @p l: the field count, the
/// struct's name, then each field's name and entry.
static const int64_t *skip_struct_layout(const int64_t *l) {
  int64_t n = l[0];
  const int64_t *e = skip_name(l + 1);
  for (int64_t i = 0; i < n; ++i)
    e = next_entry(skip_name(e));
  return e;
}

/// The words of variant @p tag of the enum layout at @p l, at its name:
/// after each earlier variant's name, its fields' flag, and for one with
/// fields their offset and struct layout.
static const int64_t *enum_variant(const int64_t *l, int64_t tag) {
  const int64_t *p = l + 1;
  for (int64_t i = 0; i < tag; ++i) {
    p = skip_name(p);
    if (*p++)
      p = skip_struct_layout(p + 1);
  }
  return p;
}

/// Whether the entry at @p e is an optional tuple's, struct's or enum's: a
/// presence flag at its offset, then (at e + 3) a whole entry for the value.
static int optional_aggregate(const int64_t *e) {
  return e[0] == PAYKAN_KIND_OPTIONAL &&
         (e[2] == PAYKAN_KIND_TUPLE || e[2] == PAYKAN_KIND_STRUCT ||
          e[2] == PAYKAN_KIND_ENUM);
}

/// The words of the layout entry at @p e: its own and a nested tuple's or
/// struct's.
static const int64_t *next_entry(const int64_t *e) {
  if (optional_aggregate(e))
    return next_entry(e + 3);
  if (e[0] == PAYKAN_KIND_OPTIONAL)
    return e + 3;
  if (e[0] == PAYKAN_KIND_STRUCT)
    return skip_struct_layout(e + 2);
  if (e[0] == PAYKAN_KIND_ENUM)
    return enum_variant(e + 2, e[2]); // past its last variant
  if (e[0] != PAYKAN_KIND_TUPLE)
    return e + 2;
  int64_t n = e[2];
  e += 3;
  for (int64_t i = 0; i < n; ++i)
    e = next_entry(e);
  return e;
}

// -- toString

/// A byte buffer for the rendering.
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

static void sb_append(StrBuf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t newCap = b->cap ? b->cap : 32;
    while (b->len + n + 1 > newCap)
      newCap *= 2;
    b->data = (char *)Paykan_realloc(b->data, newCap);
    b->cap = newCap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

/// Append the name at @p p (Runtime.h, "Structs") and return the words
/// after it.
static const int64_t *append_name(StrBuf *sb, const int64_t *p) {
  int64_t n = p[0];
  const int64_t *words = p + 1;
  for (int64_t i = 0; i < n; ++i) {
    char c = (char)(((uint64_t)words[i / 8] >> (8 * (i % 8))) & 0xff);
    sb_append(sb, &c, 1);
  }
  return skip_name(p);
}

static void render_tuple(StrBuf *sb, const char *value, const int64_t *layout);
static void render_struct(StrBuf *sb, const char *value, const int64_t *layout);
static void render_enum(StrBuf *sb, const char *value, const int64_t *layout);

/// Append the scalar of @p kind at @p at, or the object a box holds.
static void render_scalar(StrBuf *sb, int64_t kind, const char *at) {
  char buf[64];
  int n;
  switch (kind) {
  case PAYKAN_KIND_FLOAT:
    // As Str<float> renders it.
    n = Paykan_format_float(buf, sizeof(buf), load_float(at));
    sb_append(sb, buf, (size_t)n);
    break;
  case PAYKAN_KIND_BOOL:
    if (load_byte(at))
      sb_append(sb, "True", 4);
    else
      sb_append(sb, "False", 5);
    break;
  case PAYKAN_KIND_CHAR:
    sb_append(sb, at, 1);
    break;
  case PAYKAN_KIND_UINT:
    // As Str<unsigned> renders it.
    n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)load_int(at));
    sb_append(sb, buf, (size_t)n);
    break;
  case PAYKAN_KIND_OBJECT: {
    PaykanShared *box = load_box(at);
    if (!box) {
      sb_append(sb, "None", 4);
      break;
    }
    // The element's own toString (a +1 Str box, consumed here).
    PaykanShared *s = Paykan_vcall_toString(PaykanShared_get(box));
    PaykanString *str = (PaykanString *)PaykanShared_get(s);
    sb_append(sb, str->data, (size_t)str->len);
    Paykan_release(s);
    break;
  }
  case PAYKAN_KIND_INT:
  default:
    n = snprintf(buf, sizeof(buf), "%lld", (long long)load_int(at));
    sb_append(sb, buf, (size_t)n);
    break;
  }
}

/// Append the element described by the entry at @p e of the value at
/// @p value.
static void render_entry(StrBuf *sb, const char *value, const int64_t *e) {
  const char *at = value + e[1];
  if (optional_aggregate(e)) {
    if (load_byte(at))
      render_entry(sb, value, e + 3);
    else
      sb_append(sb, "None", 4);
  } else if (e[0] == PAYKAN_KIND_TUPLE) {
    render_tuple(sb, at, e + 2);
  } else if (e[0] == PAYKAN_KIND_STRUCT) {
    render_struct(sb, at, e + 2);
  } else if (e[0] == PAYKAN_KIND_ENUM) {
    render_enum(sb, at, e + 2);
  } else if (e[0] == PAYKAN_KIND_OPTIONAL && e[2] != PAYKAN_KIND_OBJECT) {
    if (load_byte(at))
      render_scalar(sb, e[2], at + payload_offset(e[2]));
    else
      sb_append(sb, "None", 4);
  } else {
    render_scalar(sb, e[0] == PAYKAN_KIND_OPTIONAL ? e[2] : e[0], at);
  }
}

static void render_tuple(StrBuf *sb, const char *value, const int64_t *layout) {
  sb_append(sb, "(", 1);
  const int64_t *e = layout + 1;
  for (int64_t i = 0; i < layout[0]; ++i, e = next_entry(e)) {
    if (i)
      sb_append(sb, ", ", 2);
    render_entry(sb, value, e);
  }
  sb_append(sb, ")", 1);
}

/// `Point(x: 1, y: 2)`.
static void render_struct(StrBuf *sb, const char *value,
                          const int64_t *layout) {
  const int64_t *e = append_name(sb, layout + 1);
  sb_append(sb, "(", 1);
  for (int64_t i = 0; i < layout[0]; ++i, e = next_entry(e)) {
    if (i)
      sb_append(sb, ", ", 2);
    e = append_name(sb, e);
    sb_append(sb, ": ", 2);
    render_entry(sb, value, e);
  }
  sb_append(sb, ")", 1);
}

/// `North`, or `Move(x: 1, y: 2)` for a variant with fields (their struct's
/// layout is named for the variant).
static void render_enum(StrBuf *sb, const char *value, const int64_t *layout) {
  const int64_t *v = enum_variant(layout, load_int(value));
  const int64_t *fields = skip_name(v);
  if (fields[0])
    render_struct(sb, value + fields[1], fields + 2);
  else
    append_name(sb, v);
}

PaykanString *PaykanTuple_toString(const void *value, const int64_t *layout) {
  StrBuf sb = {NULL, 0, 0};
  render_tuple(&sb, (const char *)value, layout);
  PaykanString *out = PaykanString_new(sb.data, (int64_t)sb.len);
  Paykan_free(sb.data);
  return out;
}

PaykanString *PaykanStruct_toString(const void *value, const int64_t *layout) {
  StrBuf sb = {NULL, 0, 0};
  render_struct(&sb, (const char *)value, layout);
  PaykanString *out = PaykanString_new(sb.data, (int64_t)sb.len);
  Paykan_free(sb.data);
  return out;
}

// -- equals

/// Whether the scalars of @p kind at @p a and @p b are equal.
static int scalar_equals(int64_t kind, const char *a, const char *b) {
  switch (kind) {
  case PAYKAN_KIND_FLOAT:
    // IEEE 754: a NaN is unequal to everything, itself included (#111).
    return load_float(a) == load_float(b);
  case PAYKAN_KIND_BOOL:
  case PAYKAN_KIND_CHAR:
    return load_byte(a) == load_byte(b);
  case PAYKAN_KIND_OBJECT: {
    PaykanShared *sa = load_box(a);
    PaykanShared *sb = load_box(b);
    if (!sa || !sb)
      return sa == sb; // two None are equal; None and an object are not
    // The virtual `equals` consumes its argument box: hand it a +1.
    Paykan_retain(sb);
    return Paykan_vcall_equals(PaykanShared_get(sa), sb) != 0;
  }
  case PAYKAN_KIND_INT:
  case PAYKAN_KIND_UINT:
  default:
    return load_int(a) == load_int(b);
  }
}

static int tuple_equals(const char *a, const char *b, const int64_t *layout);
static int struct_equals(const char *a, const char *b, const int64_t *layout);
static int enum_equals(const char *a, const char *b, const int64_t *layout);

/// Whether the elements described by the entry at @p e of the values at
/// @p a and @p b are equal.
static int entry_equals(const char *a, const char *b, const int64_t *e) {
  const char *ea = a + e[1];
  const char *eb = b + e[1];
  if (optional_aggregate(e))
    return load_byte(ea) == load_byte(eb) &&
           (!load_byte(ea) || entry_equals(a, b, e + 3));
  if (e[0] == PAYKAN_KIND_TUPLE)
    return tuple_equals(ea, eb, e + 2);
  if (e[0] == PAYKAN_KIND_STRUCT)
    return struct_equals(ea, eb, e + 2);
  if (e[0] == PAYKAN_KIND_ENUM)
    return enum_equals(ea, eb, e + 2);
  if (e[0] == PAYKAN_KIND_OPTIONAL && e[2] != PAYKAN_KIND_OBJECT) {
    // Both None, or both present with equal payloads.
    int64_t off = payload_offset(e[2]);
    return load_byte(ea) == load_byte(eb) &&
           (!load_byte(ea) || scalar_equals(e[2], ea + off, eb + off));
  }
  return scalar_equals(e[0] == PAYKAN_KIND_OPTIONAL ? e[2] : e[0], ea, eb);
}

static int tuple_equals(const char *a, const char *b, const int64_t *layout) {
  const int64_t *e = layout + 1;
  for (int64_t i = 0; i < layout[0]; ++i, e = next_entry(e))
    if (!entry_equals(a, b, e))
      return 0;
  return 1;
}

static int struct_equals(const char *a, const char *b, const int64_t *layout) {
  const int64_t *e = skip_name(layout + 1);
  for (int64_t i = 0; i < layout[0]; ++i, e = next_entry(e)) {
    e = skip_name(e);
    if (!entry_equals(a, b, e))
      return 0;
  }
  return 1;
}

/// The same variant, and for one with fields equal fields.
static int enum_equals(const char *a, const char *b, const int64_t *layout) {
  int64_t tag = load_int(a);
  if (tag != load_int(b))
    return 0;
  const int64_t *fields = skip_name(enum_variant(layout, tag));
  return !fields[0] || struct_equals(a + fields[1], b + fields[1], fields + 2);
}

int64_t PaykanTuple_equals(const void *a, const void *b,
                           const int64_t *layout) {
  return tuple_equals((const char *)a, (const char *)b, layout);
}

int64_t PaykanStruct_equals(const void *a, const void *b,
                            const int64_t *layout) {
  return struct_equals((const char *)a, (const char *)b, layout);
}

PaykanString *PaykanEnum_toString(const void *value, const int64_t *layout) {
  StrBuf sb = {NULL, 0, 0};
  render_enum(&sb, (const char *)value, layout);
  PaykanString *out = PaykanString_new(sb.data, (int64_t)sb.len);
  Paykan_free(sb.data);
  return out;
}

int64_t PaykanEnum_equals(const void *a, const void *b, const int64_t *layout) {
  return enum_equals((const char *)a, (const char *)b, layout);
}
