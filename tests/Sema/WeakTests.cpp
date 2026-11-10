// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: weak references (docs/language/16-memory.md, "Weak
// references"): the type of a `weak` declaration, and the places a weak
// reference cannot be.  A weak reference is a `shared` reference that owns
// nothing: `weak` names a `shared` object, `weak mut` a `shared mut` one.

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
      a: weak int?;
      b: weak Point;
    }
    fn main() -> int {
      p: shared = Point(1);
      w: weak Point = p;
      n: weak int? = 1;
      m: weak = 5;
      s: weak = S(1);
      f: weak (() -> int)? = None;
      c: weak = Color::Red;
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

// A `weak` local's type comes from its initializer's class, and `None` has
// none: `a: weak = None;` (or a conditional of two Nones) is an error, once,
// and the local is not reported again where it is used.  (It was a weak
// reference to the None value itself, which never read None.)  With its
// type written, it starts as None; a conditional of None and a `shared`
// object infers that object's class.
TEST(Weak, NoneInfersNoClass) {
  auto r = semaCheck(R"(
    class N { x: int; }
    fn main() -> int {
      c = True;
      s: shared = N(1);
      a: weak = None;
      b: weak = if c then None else None;
      d: weak N? = None;
      e: weak Obj? = None;
      f: weak = if c then None else s;
      a = N(1);
      if a == None && b == None && d == None && e == None && f == None {
        return 1;
      }
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"cannot infer the class of weak local 'a' from None; write "
                "'a: weak T? = None'",
                "cannot infer the class of weak local 'b' from None; write "
                "'b: weak T? = None'"});
  EXPECT_EQ(r.ErrorCount, 2u) << r.Diagnostics;
}

// A weak reference is not a place a strong reference could be written to:
// no `inout` parameter, local or match arm names a weak local or field (its
// object `shared`, as a weak reference's always is).
TEST(Weak, IsNotAnInoutPlace) {
  auto r = semaCheck(R"(
    class Point { x: int; }
    class Holder { peek: weak Point?; }
    fn f(p: inout Point?) { p = None; }
    fn main() -> int {
      x: shared = Point(1);
      h = Holder(None);
      w: weak = x;
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

// A lambda inside a lambda captures the outer one's weak capture, weakly,
// and it is read-only there as any capture is: it is not assigned, passed
// to an `inout` parameter, named by an `inout` local or bound by an `inout`
// arm.  It is read as any `T?` is (of a `shared` object here, so a read of
// it may be kept in a strong local).
TEST(Weak, NestedCaptureIsReadOnly) {
  auto r = semaCheck(R"(
    class B { n: int; }
    fn reset(p: inout B?) { p = None; }
    fn look(p: view B?) -> int { return 0; }
    fn main() -> int {
      let b: shared B = B(1);
      me: weak = b;
      let bad = fn() -> () -> int {
        return fn() -> int {
          me = None;
          reset(me);
          r: inout B? = me;
          match me { n: inout B { n.n = 2; } None { } }
          return 0;
        };
      };
      let ok = fn() -> () -> int {
        return fn() -> int {
          let n = look(me);
          let keep: shared B? = me;
          v: view B? = me;
          match me { x: B { let sum = x.n + n; } None { } }
          return if me == None then n else look(keep);
        };
      };
      return 0;
    }
  )");
  EXPECT_FALSE(r.Ok);
  expectAll(r, {"cannot assign to 'me': the function captures it by copy, and "
                "the copy is read-only",
                "'me' is captured by copy by the function, and the copy is "
                "read-only; it cannot be passed to 'inout' parameter 'p'",
                "'me' is captured by copy by the function, and the copy is "
                "read-only; it cannot be named by 'inout' local 'r'",
                "'me' is captured by copy by the function, and the copy is "
                "read-only; it cannot be bound by 'inout' arm 'n'"});
  EXPECT_EQ(r.ErrorCount, 4u) << r.Diagnostics;
}

// A `weak` local may name a `shared` object a `view` reaches (part of a
// `view` parameter, part of `self` in a `view fn`): it takes no reference,
// so the view is not copied, and a `view fn` can walk a chain of `shared`
// nodes through one, reading each node.  A `shared` parameter is named as
// any `shared` reference is.
TEST(Weak, LocalMayNameWhatAViewNames) {
  auto r = semaCheck(R"(
    class Node { v: int; next: shared Node?; }
    class Chain {
      head: shared Node?;
      view fn sum() -> int {
        total = 0;
        cur: weak = self.head;
        while cur != None {
          match cur {
            n: Node { total = total + n.v; cur = n.next; }
            None { }
          }
        }
        return total;
      }
      view fn first() -> int {
        h: weak = self.head;
        match h {
          n: Node { return n.v; }
          None { return -1; }
        }
      }
    }
    fn peek(c: view Chain, n: shared Node) -> int {
      w: weak = c.head;
      m: weak = n;
      match w { x: Node { return x.v; } None { return -1; } }
    }
    fn main() -> int {
      c = Chain(None);
      return c.sum() + c.first() + peek(c, Node(1, None));
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// What a `weak` local of a `view` reads is a `view` too: the object is
// reached through the view, so nothing changes through it (a `weak mut` one
// of a `shared mut` object neither), and it is never stored, returned or
// captured except as a `view`.  It may be assigned another `view` (the
// cursor moves on).
TEST(Weak, LocalOfAViewReadsAsAView) {
  auto r = semaCheck(R"(
    class Node {
      v: int;
      next: shared Node?;
      fn bump() { self.v = self.v + 1; }
    }
    class Keep { n: shared Node?; }
    fn take(n: shared Node) { }
    class Chain {
      head: shared Node?;
      view fn leak() -> shared Node? {
        cur: weak = self.head;
        match cur { n: Node { return n; } None { return None; } }
      }
      view fn poke() {
        cur: weak = self.head;
        match cur { n: Node { n.v = 9; n.bump(); take(n); } None { } }
      }
      view fn keep(k: mut Keep) {
        cur: weak = self.head;
        match cur { n: Node { k.n = n; } None { } }
      }
      view fn later() -> () -> int {
        cur: weak = self.head;
        return fn() -> int { match cur { n: Node { return n.v; } None { return 0; } } };
      }
    }
    class Ring {
      head: shared mut Node?;
      view fn poke() {
        cur: weak mut = self.head;
        match cur { n: Node { n.v = 9; n.bump(); } None { } }
      }
    }
    fn first(c: view Chain) -> shared Node {
      w: weak = c.head;
      match w { x: Node { return x; } None { return Node(0, None); } }
    }
    fn main() -> int { return 0; }
  )");
  EXPECT_FALSE(r.Ok);
  EXPECT_EQ(r.ErrorCount, 9u) << r.Diagnostics;
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
    class Node { v: Str; next: shared Node?; }
    class Chain {
      head: shared Node?;
      view fn front() -> view Str {
        match self.head {
          n: Node { return n.v; }
          None { return "none"; }
        }
      }
      view fn second() -> view Str {
        cur: weak = self.head;
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

// Otherwise a weak reference is an ordinary `shared T?`: assigned, compared,
// matched, passed by value or as a `view`, returned, stored, captured and
// shadowed; `y: weak = x` infers `T?` from a `shared T` or a `shared T?`.
// It reads as the `shared` reference it names, so what reads it may keep a
// strong reference.  A tuple holds no `shared` reference, so no
// destructuring assigns a weak variable.
TEST(Weak, IsOtherwiseAnOptional) {
  auto r = semaCheck(R"(
    class Point { x: int; }
    class Holder { item: shared Point?; peek: weak Point?; }
    fn take(p: shared Point?) -> int { return 0; }
    fn look(p: view Point?) -> int { return 0; }
    fn pass(p: shared Point?) -> shared Point? { return p; }
    fn main() -> int {
      x: shared Point = Point(1);
      o: shared Point? = x;
      w: weak = x;
      u: weak = o;
      e: weak Point?;
      w = x;
      w = o;
      w = None;
      w = u;
      h: mut = Holder(None, None);
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
      { w: weak = x; }
      w = None;
      v: view Point? = w;
      return n;
    }
  )");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  // A tuple's element holds no `shared` reference, so a destructuring
  // stores none in a weak variable.
  auto bad = semaCheck(R"(
    class Point { x: int; }
    fn pair() -> (Point?, int) { return (Point(1), 2); }
    fn main() -> int {
      x: shared Point = Point(1);
      w: weak = x;
      n = 0;
      w, n = (Point(2), 1);
      w, n = pair();
      return n;
    }
  )");
  EXPECT_FALSE(bad.Ok);
  for (const char *d :
       {":8:7: error: cannot store element 0 of the tuple (a 'Point') in "
        "variable 'w', a 'weak Point?': a 'weak' reference names a 'shared' "
        "object, which a tuple's element never is",
        ":9:7: error: cannot store element 0 of the tuple (a 'Point?')"})
    EXPECT_NE(bad.Diagnostics.find(d), std::string::npos) << d << "\n"
                                                          << bad.Diagnostics;
  EXPECT_EQ(bad.ErrorCount, 2u) << bad.Diagnostics;
}

// A local starts with a value, and only a `weak` one may leave out its
// initializer (it starts as None).  No parser builds another, but a frontend
// could hand one over: a `Str` local was then a null reference at run time.
TEST(Weak, OnlyAWeakLocalHasNoInitialValue) {
  namespace ast = paykan::ast;
  ast::ASTContext ctx;
  auto *s = ctx.make<ast::VarDecl>(ast::SourceLocation(3, 3, 3, 10),
                                   ctx.intern("s"), ctx.getStrTy(), nullptr);
  // `w: weak Obj?;`: a weak local's type is the `shared` form it reads as.
  auto *wTy =
      ctx.make<ast::QualifiedType>(ast::SourceLocation(4, 6, 4, 16),
                                   ast::Qualifier{ast::QualForm::Shared, false},
                                   ctx.getOptionalType(ctx.getObjTy()));
  auto *w = ctx.make<ast::VarDecl>(ast::SourceLocation(4, 3, 4, 16),
                                   ctx.intern("w"), wTy, nullptr);
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
