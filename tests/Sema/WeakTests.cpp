// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
// and a `view fn` can walk a chain of nodes through one, reading each node.
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
      view fn first() -> int {
        weak h = self.head;
        match h {
          n: Node { return n.v; }
          None { return -1; }
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
      return c.sum() + c.first() + peek(c, Node(1, None));
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// What a `weak` local of a `view` reads is a `view` too: the object is the
// view's, so nothing changes through it, and it is never stored, returned
// or captured except as a `view`.  It may be assigned another `view` (the
// cursor moves on).
TEST(Weak, LocalOfAViewReadsAsAView) {
  auto r = semaCheck(R"(
    class Node {
      v: int;
      next: Node?;
      fn bump() { self.v = self.v + 1; }
    }
    class Keep { n: Node?; }
    fn take(n: Node) { }
    class Chain {
      head: Node?;
      view fn leak() -> Node? {
        weak cur = self.head;
        match cur { n: Node { return n; } None { return None; } }
      }
      view fn poke() {
        weak cur = self.head;
        match cur { n: Node { n.v = 9; n.bump(); take(n); } None { } }
      }
      view fn keep(k: Keep) {
        weak cur = self.head;
        match cur { n: Node { k.n = n; } None { } }
      }
      view fn later() -> () -> int {
        weak cur = self.head;
        return fn() -> int { match cur { n: Node { return n.v; } None { return 0; } } };
      }
    }
    fn first(n: view Node) -> Node {
      weak w = n;
      match w { x: Node { return x; } None { return Node(0, None); } }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"'n' is bound to a 'view'; it cannot be returned",
                "'n' is bound to a 'view'; cannot assign to its field 'v'",
                "'n' is bound to a 'view'; 'bump' is not a 'view fn'",
                "'n' is bound to a 'view'; it can only be passed to a 'view' "
                "parameter",
                "'n' is bound to a 'view'; it cannot be stored",
                "'cur' is a 'weak' local of a 'view'; a function cannot "
                "capture a borrow",
                "'x' is bound to a 'view'; it cannot be returned"});
}

// A `view fn` returns a `view` of an element it reached by `match`ing part
// of `self`: the arm's name is a `view` of the subject.
TEST(Weak, MatchedPartOfSelfReturnsAsAView) {
  auto r = semaCheck(R"(
    class Node { v: Str; next: Node?; }
    class Chain {
      head: Node?;
      view fn front() -> view Str {
        match self.head {
          n: Node { return n.v; }
          None { return "none"; }
        }
      }
      view fn second() -> view Str {
        weak cur = self.head;
        match cur { n: Node { return n.v; } None { return "none"; } }
      }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  // The match of `self.head` lends its node; a new value and a cursor do not.
  EXPECT_EQ(r.Diagnostics.find(":7:"), std::string::npos) << r.Diagnostics;
  expectAll(r, {"a 'view' result must be part of 'self' or of a 'view' or "
                "'inout' parameter, not a new value",
                "'cur' is a local: a 'view' result must be part of 'self'"});
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

// A local starts with a value, and only a `weak` one may leave out its
// initializer (it starts as None).  No parser builds another, but a frontend
// could hand one over: a `Str` local was then a null reference at run time.
TEST(Weak, OnlyAWeakLocalHasNoInitialValue) {
  namespace ast = paykan::ast;
  ast::ASTContext ctx;
  auto *s = ctx.make<ast::VarDecl>(ast::SourceLocation(3, 3, 3, 10),
                                   ctx.intern("s"), ctx.getStrTy(), nullptr);
  auto *w =
      ctx.make<ast::VarDecl>(ast::SourceLocation(4, 3, 4, 16), ctx.intern("w"),
                             ctx.getOptionalType(ctx.getObjTy()), nullptr);
  w->setWeak(true);
  auto r = semaCheckAST(
      ctx,
      mainOfStatements(ctx, {ctx.make<ast::DeclStmt>(s->getLocation(), s),
                             ctx.make<ast::DeclStmt>(w->getLocation(), w)}));
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 1u) << r.Diagnostics;
  EXPECT_NE(r.Diagnostics.find("3:3: error: variable 's' needs an initial "
                               "value (only a 'weak' local starts without "
                               "one, as None)"),
            std::string::npos)
      << r.Diagnostics;
}
