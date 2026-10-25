// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for PaykanTuple_toString and PaykanTuple_equals: a tuple is a
// value held in place, so each test builds one in C++ memory laid out the
// way the compiler lays it out (a struct with natural alignment, a reference
// element as its PaykanShared* box, an `int?` element inline as
// {bool present; T value}, a nested tuple inline) and describes it with a
// layout (Runtime.h, "Tuples").  Each test runs on the tracking allocator
// and ends with no live blocks.

#include <cmath>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

extern "C" {
#include "Runtime.h"
}

namespace {

// RAII: the tracking allocator with zeroed counters; no live blocks at the
// end.
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

#define OFF(T, field) static_cast<int64_t>(offsetof(T, field))

PaykanShared *str(const char *s) {
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s, (int64_t)strlen(s)));
}

std::string render(const void *value, const int64_t *layout) {
  PaykanString *s = PaykanTuple_toString(value, layout);
  std::string out(s->data, (size_t)s->len);
  Paykan_dispose((PaykanObject *)s);
  return out;
}

int64_t eq(const void *a, const void *b, const int64_t *layout) {
  return PaykanTuple_equals(a, b, layout);
}

// -- A class element: Point(x) renders as P(x) and equals another Point of
// the same x (its toString and equals through the vtable, as a generated
// class's are).

struct Point {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
  int64_t x;
};

// Nothing to tear down: the runtime frees the object after the destroy.
void Point_destroy(PaykanObject *self) { (void)self; }

PaykanShared *Point_toString(PaykanObject *self) {
  std::string s = "P(" + std::to_string(((Point *)self)->x) + ")";
  return PaykanShared_new(
      (PaykanObject *)PaykanString_new(s.data(), (int64_t)s.size()));
}

int64_t Point_equals(PaykanObject *self, PaykanShared *other);

PaykanMethod Point_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Point_destroy),
    reinterpret_cast<PaykanMethod>(&Point_toString),
    reinterpret_cast<PaykanMethod>(&Point_equals),
};

int gPointEqualsCalls = 0;

int64_t Point_equals(PaykanObject *self, PaykanShared *other) {
  ++gPointEqualsCalls;
  PaykanObject *o = PaykanShared_get(other);
  int64_t result =
      o && o->vtable == Point_vtable && ((Point *)o)->x == ((Point *)self)->x;
  Paykan_release(other); // `other` is consumed
  return result;
}

PaykanShared *point(int64_t x) {
  auto *p = static_cast<Point *>(Paykan_malloc(sizeof(Point)));
  p->vtable = Point_vtable;
  p->strong = 0;
  p->weak = 0;
  p->x = x;
  return PaykanShared_new((PaykanObject *)p);
}

// -- An element whose toString and equals write the storage the tuple is in,
// as a Paykan override can (through a closure that assigns the field holding
// the tuple): Replacer releases the reference in gReplacedSlot and clears it,
// then records how many Replacers have been destroyed so far.

struct Replacer {
  PaykanMethod *vtable;
  int32_t strong;
  int32_t weak;
};

PaykanShared **gReplacedSlot = nullptr;
int gReplacersDestroyed = 0;
int gDestroyedDuringCall = -1;

void Replacer_destroy(PaykanObject *self) {
  (void)self;
  ++gReplacersDestroyed;
}

void replaceSlot() {
  Paykan_release(*gReplacedSlot);
  *gReplacedSlot = nullptr;
  gDestroyedDuringCall = gReplacersDestroyed;
}

PaykanShared *Replacer_toString(PaykanObject *self) {
  (void)self;
  replaceSlot();
  return str("R");
}

int64_t Replacer_equals(PaykanObject *self, PaykanShared *other) {
  (void)self;
  replaceSlot();
  Paykan_release(other); // `other` is consumed
  return 0;
}

PaykanMethod Replacer_vtable[PAYKAN_OBJECT_SLOTS] = {
    reinterpret_cast<PaykanMethod>(&Replacer_destroy),
    reinterpret_cast<PaykanMethod>(&Replacer_toString),
    reinterpret_cast<PaykanMethod>(&Replacer_equals),
};

PaykanShared *replacer() {
  auto *r = static_cast<Replacer *>(Paykan_malloc(sizeof(Replacer)));
  r->vtable = Replacer_vtable;
  r->strong = 0;
  r->weak = 0;
  return PaykanShared_new((PaykanObject *)r);
}

/// Replacers start counting their deaths from zero; @p slot is the one the
/// next toString or equals clears.
void replacing(PaykanShared **slot) {
  gReplacedSlot = slot;
  gReplacersDestroyed = 0;
  gDestroyedDuringCall = -1;
}

// -- Tuple types as the compiler lays them out

// (int, float, bool, char, Str)
struct Scalars {
  int64_t i;
  double f;
  bool b;
  int8_t c;
  PaykanShared *s;
};
const int64_t kScalarsLayout[] = {5,                        // elements
                                  kInt,   OFF(Scalars, i),  // int
                                  kFloat, OFF(Scalars, f),  // float
                                  kBool,  OFF(Scalars, b),  // bool
                                  kChar,  OFF(Scalars, c),  // char
                                  kObj,   OFF(Scalars, s)}; // Str

template <typename T> struct Inline {
  bool present;
  T value;
};

// (int?, float?, bool?, char?, Str?)
struct Optionals {
  Inline<int64_t> i;
  Inline<double> f;
  Inline<bool> b;
  Inline<int8_t> c;
  PaykanShared *s; // NULL for None
};
const int64_t kOptionalsLayout[] = {5,                               // elements
                                    kOpt, OFF(Optionals, i), kInt,   // int?
                                    kOpt, OFF(Optionals, f), kFloat, // float?
                                    kOpt, OFF(Optionals, b), kBool,  // bool?
                                    kOpt, OFF(Optionals, c), kChar,  // char?
                                    kOpt, OFF(Optionals, s), kObj};  // Str?

// The payload offsets the layout comment promises.
static_assert(offsetof(Inline<int64_t>, value) == 8, "int? payload at 8");
static_assert(offsetof(Inline<double>, value) == 8, "float? payload at 8");
static_assert(offsetof(Inline<bool>, value) == 1, "bool? payload at 1");
static_assert(offsetof(Inline<int8_t>, value) == 1, "char? payload at 1");

// (int, Str)
struct IntStr {
  int64_t n;
  PaykanShared *s;
};
const int64_t kIntStrLayout[] = {2, kInt, OFF(IntStr, n), kObj, OFF(IntStr, s)};

// ((int, Str), (char, (bool, float)), int)
struct Inner2 {
  bool b;
  double f;
};
struct Inner1 {
  int8_t c;
  Inner2 t;
};
struct Nested {
  IntStr a;
  Inner1 b;
  int64_t n;
};
const int64_t kNestedLayout[] = {
    3,                                              // elements
    kTuple, OFF(Nested, a), 2,                      // (int, Str)
    kInt,   OFF(IntStr, n), kObj,   OFF(IntStr, s), //
    kTuple, OFF(Nested, b), 2,                      // (char, (bool, float))
    kChar,  OFF(Inner1, c),                         //
    kTuple, OFF(Inner1, t), 2,                      // (bool, float)
    kBool,  OFF(Inner2, b), kFloat, OFF(Inner2, f), //
    kInt,   OFF(Nested, n)};                        // int

// (int, float)
struct IntFloat {
  int64_t n;
  double f;
};
const int64_t kIntFloatLayout[] = {2, kInt, OFF(IntFloat, n), kFloat,
                                   OFF(IntFloat, f)};

// (Obj, Obj?): two references, the second one nullable.
struct Refs {
  PaykanShared *a;
  PaykanShared *b;
};
const int64_t kRefsLayout[] = {2, kObj, OFF(Refs, a), kOpt, OFF(Refs, b), kObj};

void release(Refs &r) {
  Paykan_release(r.a);
  Paykan_release(r.b);
}

} // namespace

// -- toString

TEST(TupleToString, RendersEachKind) {
  TrackingHeap heap;
  Scalars t = {1, 2.5, true, 'c', str("a")};
  EXPECT_EQ(render(&t, kScalarsLayout), "(1, 2.5, True, c, a)");
  Scalars u = {-7, -0.125, false, 'Z', t.s};
  EXPECT_EQ(render(&u, kScalarsLayout), "(-7, -0.125, False, Z, a)");
  Paykan_release(t.s);
}

// A float element renders as Str<float> does.
TEST(TupleToString, FloatsRenderLikeStrFloat) {
  TrackingHeap heap;
  IntFloat t = {0, 2.0};
  EXPECT_EQ(render(&t, kIntFloatLayout), "(0, 2)");
  t.f = std::nan("");
  EXPECT_EQ(render(&t, kIntFloatLayout), "(0, nan)");
  t.f = -INFINITY;
  EXPECT_EQ(render(&t, kIntFloatLayout), "(0, -inf)");
}

TEST(TupleToString, OptionalsRenderTheirValueOrNone) {
  TrackingHeap heap;
  Optionals some = {
      {true, 7}, {true, 2.5}, {true, false}, {true, 'z'}, str("s")};
  EXPECT_EQ(render(&some, kOptionalsLayout), "(7, 2.5, False, z, s)");
  // An absent one is None whatever its payload bytes hold.
  Optionals none = {
      {false, 7}, {false, 2.5}, {false, true}, {false, 'z'}, nullptr};
  EXPECT_EQ(render(&none, kOptionalsLayout), "(None, None, None, None, None)");
  Paykan_release(some.s);
}

// A NULL box renders as None even where the element is not optional.
TEST(TupleToString, NullReferenceRendersNone) {
  TrackingHeap heap;
  IntStr t = {3, nullptr};
  EXPECT_EQ(render(&t, kIntStrLayout), "(3, None)");
}

TEST(TupleToString, NestedTuplesRenderInPlace) {
  TrackingHeap heap;
  Nested t = {{1, str("in")}, {'x', {true, 0.5}}, 2};
  EXPECT_EQ(render(&t, kNestedLayout), "((1, in), (x, (True, 0.5)), 2)");
  Paykan_release(t.a.s);
}

TEST(TupleToString, ClassElementRendersThroughItsToString) {
  TrackingHeap heap;
  Refs t = {point(3),
            PaykanShared_new((PaykanObject *)PaykanString_new("oops", 4))};
  EXPECT_EQ(render(&t, kRefsLayout), "(P(3), oops)");
  // Rendering borrows the elements.
  EXPECT_EQ(t.a->strong, 1);
  EXPECT_EQ(t.b->strong, 1);
  release(t);
}

// An element's toString may release the tuple's own reference to it (the
// tuple is in storage the toString can write): the element is held while
// its toString runs, and dies when it returns.
TEST(TupleToString, ElementIsHeldWhileItsToStringRuns) {
  TrackingHeap heap;
  Refs t = {replacer(), nullptr};
  replacing(&t.a);
  EXPECT_EQ(render(&t, kRefsLayout), "(R, None)");
  EXPECT_EQ(gDestroyedDuringCall, 0);
  EXPECT_EQ(gReplacersDestroyed, 1);
  EXPECT_EQ(t.a, nullptr);
}

TEST(TupleToString, LongRenderingGrowsBuffer) {
  TrackingHeap heap;
  std::string big(200, 'x');
  PaykanShared *s = str(big.c_str());
  Refs t = {s, s};
  EXPECT_EQ(render(&t, kRefsLayout), "(" + big + ", " + big + ")");
  Paykan_release(s);
}

// -- equals

TEST(TupleEquals, EachScalarKind) {
  TrackingHeap heap;
  PaykanShared *a = str("a");
  Scalars t = {1, 2.5, true, 'c', a};
  Scalars u = t;
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 1);
  u.i = 2;
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 0);
  u = t;
  u.f = 2.25;
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 0);
  u = t;
  u.b = false;
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 0);
  u = t;
  u.c = 'd';
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 0);
  // A Str compares by content: another string of the same text is equal.
  u = t;
  u.s = str("a");
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 1);
  Paykan_release(u.s);
  u.s = str("b");
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 0);
  Paykan_release(u.s);
  EXPECT_EQ(a->strong, 1);
  Paykan_release(a);
}

// Only the elements are compared, not the padding between them.
TEST(TupleEquals, PaddingIsIgnored) {
  TrackingHeap heap;
  Scalars t, u;
  memset(&t, 0xAA, sizeof t);
  memset(&u, 0x55, sizeof u);
  t.i = u.i = 4;
  t.f = u.f = 1.5;
  t.b = u.b = true;
  t.c = u.c = 'q';
  t.s = u.s = str("p");
  EXPECT_EQ(eq(&t, &u, kScalarsLayout), 1);
  Paykan_release(t.s);
}

// A tuple holding a NaN is unequal to itself, exactly like the NaN (#111);
// -0.0 == 0.0 under IEEE 754, so those tuples are equal.
TEST(TupleEquals, FloatsCompareByIEEE754) {
  TrackingHeap heap;
  IntFloat a = {1, std::nan("")};
  IntFloat b = a;
  EXPECT_EQ(eq(&a, &a, kIntFloatLayout), 0); // the same bytes
  EXPECT_EQ(eq(&a, &b, kIntFloatLayout), 0);
  a.f = -0.0;
  b.f = 0.0;
  EXPECT_EQ(eq(&a, &b, kIntFloatLayout), 1);
  EXPECT_EQ(eq(&b, &a, kIntFloatLayout), 1);
  EXPECT_EQ(eq(&a, &a, kIntFloatLayout), 1);
}

TEST(TupleEquals, OptionalsComparePresenceThenValue) {
  TrackingHeap heap;
  PaykanShared *s = str("s");
  Optionals a = {{true, 7}, {true, 2.5}, {true, false}, {true, 'z'}, s};
  Optionals b = a;
  EXPECT_EQ(eq(&a, &b, kOptionalsLayout), 1);
  // Present against absent, either way, for each element.
  for (int k = 0; k < 5; ++k) {
    b = a;
    switch (k) {
    case 0:
      b.i.present = false;
      break;
    case 1:
      b.f.present = false;
      break;
    case 2:
      b.b.present = false;
      break;
    case 3:
      b.c.present = false;
      break;
    default:
      b.s = nullptr;
      break;
    }
    EXPECT_EQ(eq(&a, &b, kOptionalsLayout), 0) << k;
    EXPECT_EQ(eq(&b, &a, kOptionalsLayout), 0) << k;
  }
  // Both present, different payloads.
  b = a;
  b.i.value = 8;
  EXPECT_EQ(eq(&a, &b, kOptionalsLayout), 0);
  b = a;
  b.b.value = true;
  EXPECT_EQ(eq(&a, &b, kOptionalsLayout), 0);
  b = a;
  b.f.value = std::nan("");
  EXPECT_EQ(eq(&b, &b, kOptionalsLayout), 0);
  // Two None are equal, whatever the payload bytes of an absent one hold.
  Optionals n1 = {
      {false, 1}, {false, 1.0}, {false, true}, {false, 'a'}, nullptr};
  Optionals n2 = {
      {false, 2}, {false, std::nan("")}, {false, false}, {false, 'b'}, nullptr};
  EXPECT_EQ(eq(&n1, &n2, kOptionalsLayout), 1);
  EXPECT_EQ(eq(&n2, &n1, kOptionalsLayout), 1);
  EXPECT_EQ(s->strong, 1);
  Paykan_release(s);
}

TEST(TupleEquals, NullReferences) {
  TrackingHeap heap;
  IntStr a = {1, nullptr};
  IntStr b = {1, nullptr};
  EXPECT_EQ(eq(&a, &b, kIntStrLayout), 1); // two None
  b.s = str("s");
  EXPECT_EQ(eq(&a, &b, kIntStrLayout), 0); // None against an object
  EXPECT_EQ(eq(&b, &a, kIntStrLayout), 0);
  Paykan_release(b.s);
}

// A reference element is compared by its `equals`, which is handed its own
// reference to the other element (and consumes it): the tuples' references
// are untouched.
TEST(TupleEquals, ReferencesCompareThroughEquals) {
  TrackingHeap heap;
  gPointEqualsCalls = 0;
  PaykanShared *err = PaykanShared_new((PaykanObject *)PaykanFile_new());
  Refs a = {point(3), err};
  Refs b = {point(3), err};
  EXPECT_EQ(eq(&a, &b, kRefsLayout), 1); // distinct Points, the same x
  EXPECT_EQ(gPointEqualsCalls, 1);
  EXPECT_EQ(a.a->strong, 1);
  EXPECT_EQ(b.a->strong, 1);
  EXPECT_EQ(err->strong, 1);
  Paykan_release(b.a);
  b.a = point(4);
  EXPECT_EQ(eq(&a, &b, kRefsLayout), 0);
  EXPECT_EQ(b.a->strong, 1);
  // File's equals is identity: another File is not equal.
  Paykan_release(b.a);
  b.a = point(3);
  b.b = PaykanShared_new((PaykanObject *)PaykanFile_new());
  EXPECT_EQ(eq(&a, &b, kRefsLayout), 0);
  release(a);
  release(b);
}

// An element's equals may release either tuple's reference to the elements
// it compares (the tuples are in storage the equals can write): the
// receiver is held while it runs, and `other` is the equals' own reference.
TEST(TupleEquals, ReceiverIsHeldWhileItsEqualsRuns) {
  TrackingHeap heap;
  Refs a = {replacer(), nullptr};
  Refs b = {replacer(), nullptr};
  replacing(&a.a);
  EXPECT_EQ(eq(&a, &b, kRefsLayout), 0);
  EXPECT_EQ(gDestroyedDuringCall, 0);
  EXPECT_EQ(gReplacersDestroyed, 1); // the receiver, once its equals returned
  EXPECT_EQ(a.a, nullptr);
  EXPECT_EQ(b.a->strong, 1);
  release(b);
}

TEST(TupleEquals, OtherIsHeldWhileTheEqualsRuns) {
  TrackingHeap heap;
  Refs a = {replacer(), nullptr};
  Refs b = {replacer(), nullptr};
  replacing(&b.a);
  EXPECT_EQ(eq(&a, &b, kRefsLayout), 0);
  EXPECT_EQ(gDestroyedDuringCall, 0);
  EXPECT_EQ(gReplacersDestroyed, 1); // `other`, released by the equals
  EXPECT_EQ(b.a, nullptr);
  EXPECT_EQ(a.a->strong, 1);
  release(a);
}

// The first unequal element decides: the elements after it are not compared.
TEST(TupleEquals, StopsAtTheFirstUnequalElement) {
  TrackingHeap heap;
  gPointEqualsCalls = 0;
  struct IntPoint {
    int64_t n;
    PaykanShared *p;
  };
  const int64_t layout[] = {2, kInt, OFF(IntPoint, n), kObj, OFF(IntPoint, p)};
  IntPoint a = {1, point(5)};
  IntPoint b = {2, point(5)};
  EXPECT_EQ(eq(&a, &b, layout), 0);
  EXPECT_EQ(gPointEqualsCalls, 0);
  b.n = 1;
  EXPECT_EQ(eq(&a, &b, layout), 1);
  EXPECT_EQ(gPointEqualsCalls, 1);
  Paykan_release(a.p);
  Paykan_release(b.p);
}

TEST(TupleEquals, NestedTuplesCompareElementWise) {
  TrackingHeap heap;
  Nested a = {{1, str("in")}, {'x', {true, 0.5}}, 2};
  Nested b = {{1, str("in")}, {'x', {true, 0.5}}, 2};
  EXPECT_EQ(eq(&a, &b, kNestedLayout), 1);
  b.b.t.b = false; // two levels down
  EXPECT_EQ(eq(&a, &b, kNestedLayout), 0);
  b.b.t.b = true;
  b.a.n = 9;
  EXPECT_EQ(eq(&a, &b, kNestedLayout), 0);
  b.a.n = 1;
  b.n = 3; // the element after the nested ones
  EXPECT_EQ(eq(&a, &b, kNestedLayout), 0);
  b.n = 2;
  // A NaN inside a nested tuple makes the outer one unequal to itself.
  b.b.t.f = std::nan("");
  EXPECT_EQ(eq(&b, &b, kNestedLayout), 0);
  Paykan_release(a.a.s);
  Paykan_release(b.a.s);
}
