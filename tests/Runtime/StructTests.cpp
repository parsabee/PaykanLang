// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Unit tests for PaykanStruct_toString and PaykanStruct_equals: a struct is a
// value held in place like a tuple, with a name and named fields, so each
// test builds one in C++ memory laid out the way the compiler lays it out
// and describes it with a struct layout (Runtime.h, "Structs"): the field
// count, the struct's name, then each field's name and tuple entry.  A name
// is its byte length in one word, then its bytes packed eight to a word, the
// first byte in the lowest eight bits.  Each test runs on the tracking
// allocator and ends with no live blocks.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

extern "C" {
#include "Runtime.h"
}

namespace {

struct TrackingHeap {
  TrackingHeap() {
    Paykan_heap_set_tracking(1);
    Paykan_heap_reset();
  }
  ~TrackingHeap() {
    EXPECT_EQ(Paykan_heap_live_blocks(), 0);
    Paykan_heap_set_tracking(0);
  }
};

constexpr int64_t kInt = PAYKAN_KIND_INT;
constexpr int64_t kFloat = PAYKAN_KIND_FLOAT;
constexpr int64_t kBool = PAYKAN_KIND_BOOL;
constexpr int64_t kChar = PAYKAN_KIND_CHAR;
constexpr int64_t kObj = PAYKAN_KIND_OBJECT;
constexpr int64_t kOpt = PAYKAN_KIND_OPTIONAL;
constexpr int64_t kTuple = PAYKAN_KIND_TUPLE;
constexpr int64_t kStruct = PAYKAN_KIND_STRUCT;

#define OFF(T, field) static_cast<int64_t>(offsetof(T, field))

/// A layout under construction: words appended in order.
struct Layout {
  std::vector<int64_t> Words;
  Layout &word(int64_t w) {
    Words.push_back(w);
    return *this;
  }
  Layout &words(std::initializer_list<int64_t> ws) {
    for (int64_t w : ws)
      Words.push_back(w);
    return *this;
  }
  /// A name as the compiler packs it.
  Layout &name(const std::string &s) {
    Words.push_back(static_cast<int64_t>(s.size()));
    for (size_t i = 0; i < s.size(); i += 8) {
      uint64_t w = 0;
      for (size_t k = 0; k < 8 && i + k < s.size(); ++k)
        w |= static_cast<uint64_t>(static_cast<unsigned char>(s[i + k]))
             << (8 * k);
      Words.push_back(static_cast<int64_t>(w));
    }
    return *this;
  }
  /// Another layout, inline (a nested struct's or tuple's).
  Layout &nested(const Layout &l) {
    Words.insert(Words.end(), l.Words.begin(), l.Words.end());
    return *this;
  }
  const int64_t *data() const { return Words.data(); }
};

PaykanShared *str(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

std::string render(const void *value, const Layout &layout) {
  PaykanString *s = PaykanStruct_toString(value, layout.data());
  std::string out(s->data, (size_t)s->len);
  PaykanString_destroy((PaykanObject *)s);
  return out;
}

std::string renderTuple(const void *value, const Layout &layout) {
  PaykanString *s = PaykanTuple_toString(value, layout.data());
  std::string out(s->data, (size_t)s->len);
  PaykanString_destroy((PaykanObject *)s);
  return out;
}

int64_t eq(const void *a, const void *b, const Layout &layout) {
  return PaykanStruct_equals(a, b, layout.data());
}

// -- The structs as the compiler lays them out

// struct Point { x: int; y: int; }
struct Point {
  int64_t x;
  int64_t y;
};
const Layout kPoint = Layout()
                          .word(2)
                          .name("Point")
                          .name("x")
                          .words({kInt, OFF(Point, x)})
                          .name("y")
                          .words({kInt, OFF(Point, y)});

// struct Rec { name: Str; f: float; b: bool; c: char; maybe: Str?; oi: int?; }
template <typename T> struct Inline {
  bool present;
  T value;
};
struct Rec {
  PaykanShared *name;
  double f;
  bool b;
  int8_t c;
  PaykanShared *maybe;
  Inline<int64_t> oi;
};
const Layout kRec = Layout()
                        .word(6)
                        .name("Rec")
                        .name("name")
                        .words({kObj, OFF(Rec, name)})
                        .name("f")
                        .words({kFloat, OFF(Rec, f)})
                        .name("b")
                        .words({kBool, OFF(Rec, b)})
                        .name("c")
                        .words({kChar, OFF(Rec, c)})
                        .name("maybe")
                        .words({kOpt, OFF(Rec, maybe), kObj})
                        .name("oi")
                        .words({kOpt, OFF(Rec, oi), kInt});

// struct Line { a: Point; b: Point; t: (int, Point); }: nested structs, and
// one inside a tuple.  The names are longer than a word and exactly one.
struct IntPoint {
  int64_t n;
  Point p;
};
struct Line {
  Point a;
  Point b;
  IntPoint t;
};
const Layout kIntPointTuple = Layout()
                                  .word(2)
                                  .words({kInt, OFF(IntPoint, n)})
                                  .words({kStruct, OFF(IntPoint, p)})
                                  .nested(kPoint);
const Layout kLine = Layout()
                         .word(3)
                         .name("LineSegment") // 11 bytes: two words
                         .name("startAtA")    // 8 bytes: one full word
                         .words({kStruct, OFF(Line, a)})
                         .nested(kPoint)
                         .name("b")
                         .words({kStruct, OFF(Line, b)})
                         .nested(kPoint)
                         .name("t")
                         .words({kTuple, OFF(Line, t)})
                         .nested(kIntPointTuple);

// (Point, int): a struct as a tuple element.
struct PointInt {
  Point p;
  int64_t n;
};
const Layout kPointInt = Layout()
                             .word(2)
                             .words({kStruct, OFF(PointInt, p)})
                             .nested(kPoint)
                             .words({kInt, OFF(PointInt, n)});

} // namespace

// -- toString

TEST(StructToString, NameThenEachFieldNamed) {
  TrackingHeap heap;
  Point p = {1, -2};
  EXPECT_EQ(render(&p, kPoint), "Point(x: 1, y: -2)");
}

TEST(StructToString, EveryKindRendersAsPrintShowsIt) {
  TrackingHeap heap;
  Rec r = {str("a b"), 2.5, true, 'z', nullptr, {false, 0}};
  EXPECT_EQ(render(&r, kRec),
            "Rec(name: a b, f: 2.5, b: True, c: z, maybe: None, oi: None)");
  r.maybe = str("m");
  r.oi = {true, 7};
  r.b = false;
  r.f = -0.125;
  EXPECT_EQ(render(&r, kRec),
            "Rec(name: a b, f: -0.125, b: False, c: z, maybe: m, oi: 7)");
  // Rendering borrows the references.
  EXPECT_EQ(r.name->refCount, 1);
  EXPECT_EQ(r.maybe->refCount, 1);
  Paykan_release(r.name);
  Paykan_release(r.maybe);
}

TEST(StructToString, NestedStructsAndTuplesRenderInPlace) {
  TrackingHeap heap;
  Line l = {{1, 2}, {3, 4}, {5, {6, 7}}};
  EXPECT_EQ(render(&l, kLine),
            "LineSegment(startAtA: Point(x: 1, y: 2), b: Point(x: 3, y: 4), "
            "t: (5, Point(x: 6, y: 7)))");
}

TEST(StructToString, AStructInATupleRendersInPlace) {
  TrackingHeap heap;
  PointInt t = {{8, 9}, 10};
  EXPECT_EQ(renderTuple(&t, kPointInt), "(Point(x: 8, y: 9), 10)");
}

TEST(StructToString, LongRenderingGrowsBuffer) {
  TrackingHeap heap;
  Rec r = {
      str(std::string(300, 'x').c_str()), 0.0, false, 'c', nullptr, {false, 0}};
  std::string out = render(&r, kRec);
  EXPECT_EQ(out.size(), std::string("Rec(name: , f: 0, b: False, c: c, "
                                    "maybe: None, oi: None)")
                                .size() +
                            300);
  EXPECT_EQ(out.substr(0, 10 + 300), "Rec(name: " + std::string(300, 'x'));
  Paykan_release(r.name);
}

// -- equals

TEST(StructEquals, FieldByField) {
  TrackingHeap heap;
  Point a = {1, 2};
  Point b = {1, 2};
  EXPECT_EQ(eq(&a, &b, kPoint), 1);
  EXPECT_EQ(eq(&a, &a, kPoint), 1);
  b.y = 3;
  EXPECT_EQ(eq(&a, &b, kPoint), 0);
  b.y = 2;
  b.x = 0;
  EXPECT_EQ(eq(&a, &b, kPoint), 0);
}

TEST(StructEquals, EveryKindComparesAsATupleElementDoes) {
  TrackingHeap heap;
  Rec a = {str("s"), 2.5, true, 'z', str("m"), {true, 7}};
  Rec b = {str("s"), 2.5, true, 'z', str("m"), {true, 7}};
  EXPECT_EQ(eq(&a, &b, kRec), 1); // Str by content
  b.c = 'y';
  EXPECT_EQ(eq(&a, &b, kRec), 0);
  b.c = 'z';
  b.oi.present = false;
  EXPECT_EQ(eq(&a, &b, kRec), 0); // presence
  b.oi.present = true;
  Paykan_release(b.maybe);
  b.maybe = nullptr;
  EXPECT_EQ(eq(&a, &b, kRec), 0); // a box against None
  Paykan_release(a.maybe);
  a.maybe = nullptr;
  EXPECT_EQ(eq(&a, &b, kRec), 1); // two Nones
  b.f = std::nan("");
  EXPECT_EQ(eq(&b, &b, kRec), 0); // IEEE 754: a NaN is unequal to itself
  // Comparing borrows the references.
  EXPECT_EQ(a.name->refCount, 1);
  Paykan_release(a.name);
  Paykan_release(b.name);
}

TEST(StructEquals, NestedStructsAndTuplesCompareInPlace) {
  TrackingHeap heap;
  Line a = {{1, 2}, {3, 4}, {5, {6, 7}}};
  Line b = a;
  EXPECT_EQ(eq(&a, &b, kLine), 1);
  b.t.p.y = 0; // a struct inside the tuple inside the struct
  EXPECT_EQ(eq(&a, &b, kLine), 0);
  b.t.p.y = 7;
  b.b.x = 0;
  EXPECT_EQ(eq(&a, &b, kLine), 0);
  b.b.x = 3;
  EXPECT_EQ(eq(&a, &b, kLine), 1);
  PointInt t = {{8, 9}, 10};
  PointInt u = {{8, 9}, 10};
  EXPECT_EQ(PaykanTuple_equals(&t, &u, kPointInt.data()), 1);
  u.p.x = 0;
  EXPECT_EQ(PaykanTuple_equals(&t, &u, kPointInt.data()), 0);
}
