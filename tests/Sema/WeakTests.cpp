// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Sema tests: weak references (docs/language/16-memory.md, "Weak
// references"): the type of a `weak` declaration, and the places a weak
// reference cannot be.

#include "TestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

void expectAll(const SemaResult &r, std::initializer_list<const char *> diags) {
  for (const char *d : diags)
    EXPECT_NE(r.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                        << r.Diagnostics;
}

} // namespace

// A `weak` local or field has the type `T?` of a class `T`: not a plain
// class (the object may go), not a value, a struct, an enum or a function.
TEST(Weak, TypeIsAnOptionalClass) {
  auto r = semaCheck(R"(
    class Point { x: int; }
    struct S { v: int; }
    enum Color { Red }
    class C {
      weak a: int?;
      weak b: Point;
    }
    fn main() -> int {
      p = Point(1);
      weak w: Point = p;
      weak n: int? = 1;
      weak m = 5;
      weak s = S(1);
      weak f: (() -> int)? = None;
      weak c = Color::Red;
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'weak' field 'a' of class 'C' has type 'int?'; a weak "
                "reference is an optional class type ('Node?')",
                "'weak' field 'b' of class 'C' must be optional: a weak "
                "reference reads as None once its object is destroyed; write "
                "'Point?'",
                "'weak' variable 'w' must be optional",
                "'weak' variable 'n' has type 'int?'",
                "'weak' variable 'm' has type 'int'",
                "'weak' variable 's' has type 'S'",
                "'weak' variable 'f' has type '(() -> int)?'",
                "'weak' variable 'c' has type 'Color'"});
}

// A weak reference is not a place a strong reference could be written to:
// no `inout` parameter, local or match arm names a weak local or field.
TEST(Weak, IsNotAnInoutPlace) {
  auto r = semaCheck(R"(
    class Point { x: int; }
    class Holder { weak peek: Point?; }
    fn f(p: inout Point?) { p = None; }
    fn main() -> int {
      x = Point(1);
      h = Holder(None);
      weak w = x;
      f(w);
      f(h.peek);
      v: inout Point? = w;
      u: inout Point? = h.peek;
      match w { n: inout Point { n = Point(2); } None { } }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'w' is a 'weak' reference and cannot be passed to 'inout' "
                "parameter 'p'",
                "field 'peek' of class 'Holder' is a 'weak' reference and "
                "cannot be passed to 'inout' parameter 'p'",
                "'w' is a 'weak' reference and cannot be named by 'inout' "
                "local 'v'",
                "field 'peek' of class 'Holder' is a 'weak' reference and "
                "cannot be named by 'inout' local 'u'",
                "'w' is a 'weak' reference and cannot be bound by 'inout' "
                "arm 'n'"});
}

// A `weak` local may name what a `view` names (a `view` parameter, part of
// `self` in a `view fn`): it takes no reference, so the view is not copied,
// and what it reads is a fresh reference, so a `view fn` can walk a chain of
// nodes through one and return what it finds.
TEST(Weak, LocalMayNameWhatAViewNames) {
  auto r = semaCheck(R"(
    class Node { v: int; next: Node?; }
    class Chain {
      head: Node?;
      view fn sum() -> int {
        total = 0;
        weak cur = self.head;
        while cur != None {
          match cur {
            n: Node { total = total + n.v; cur = n.next; }
            None { }
          }
        }
        return total;
      }
      view fn first() -> Node? {
        weak h = self.head;
        match h {
          n: Node { return n; }
          None { return None; }
        }
      }
    }
    fn peek(c: view Chain, n: view Node) -> int {
      weak w = c.head;
      weak m = n;
      match w { x: Node { return x.v; } None { return -1; } }
    }
    fn main() -> int {
      c = Chain(None);
      return c.sum() + peek(c, Node(1, None));
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Otherwise a weak reference is an ordinary `T?`: assigned, compared,
// matched, passed by value or as a `view`, returned, stored, captured and
// shadowed; `weak y = x` infers `T?` from a `T` or a `T?`.
TEST(Weak, IsOtherwiseAnOptional) {
  auto r = semaCheck(R"(
    class Point { x: int; }
    class Holder { item: Point?; weak peek: Point?; }
    fn take(p: Point?) -> int { return 0; }
    fn look(p: view Point?) -> int { return 0; }
    fn pass(p: Point?) -> Point? { return p; }
    fn main() -> int {
      x = Point(1);
      o: Point? = x;
      weak w = x;
      weak u = o;
      weak e: Point?;
      w = x;
      w = o;
      w = None;
      w = u;
      h = Holder(None, None);
      h.item = w;
      h.peek = x;
      h.peek = h.peek;
      u = h.peek;
      o = w;
      n = take(w) + look(w) + take(pass(w)) + take(h.peek);
      b = w == None;
      c = w == u;
      match w { p: Point { n = n + p.x; } None { } }
      match u { p: view Point { n = n + p.x; } None { } }
      f = fn() -> bool { return w == None; };
      { weak w = x; }
      w, n = (x, 1);
      v: view Point? = w;
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}
