// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Ownership / ARC regression tests for the unique-box invariant (PAY-1).
//
// Every heap object has exactly one PaykanShared box, recoverable from the
// object-header backpointer.  These tests pin down the raw-alias double-free
// family that motivated the invariant:
//
//   A1  `self` used as a value (return / argument / assignment)
//   A2  match-arm bindings used with ownership-taking consumers
//   A4  array-typed class fields holding references to their elements
//   A5  len() on receivers that are not bare identifiers
//   B8  vtable-convention narrowing on assignment
//
// Each test asserts BOTH correct output/exit and zero live heap blocks — a
// double free typically crashes, but a "fixed" path that leaks instead would
// only be caught by the tracking allocator.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// -- A1 — `self` used as a value

// Returning `self` from a method must hand out a +1 on the object's existing
// box, not wrap the raw receiver in a second box (double free on scope exit).
TEST(Ownership, ReturnSelf) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class B {
      x: int;
      fn me() -> B { return self; }
    }

    impl B {
      fn __init__() { self.x = 1; }
    }
    fn main() -> int {
      b: B = B();
      c: B = b.me();
      println(Str<int>(c.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n");
  g.expectNoLeaks("ReturnSelf");
}

// Chained self-returns: each hop retains the same unique box.
TEST(Ownership, ReturnSelfChained) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class B {
      x: int;
      fn bump() -> B { self.x = self.x + 1; return self; }
    }

    impl B {
      fn __init__() { self.x = 0; }
    }
    fn main() -> int {
      b: B = B();
      c: B = b.bump().bump().bump();
      println(Str<int>(c.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n");
  g.expectNoLeaks("ReturnSelfChained");
}

// Passing `self` to a function whose class-typed parameter consumes a +1 box.
TEST(Ownership, PassSelfAsArgument) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class B {
      x: int;
      fn send() { probe(self); }
    }

    impl B {
      fn __init__() { self.x = 1; }
    }
    fn probe(b: B) { println(Str<int>(b.x)); }
    fn main() -> int {
      b: B = B();
      b.send();
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n");
  g.expectNoLeaks("PassSelfAsArgument");
}

// `y = self` inside a method: the local owner shares the caller's box.
TEST(Ownership, AssignSelfToLocal) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn grab() -> int {
        y = self;
        return y.x;
      }
    }

    impl A {
      fn __init__() { self.x = 7; }
    }
    fn main() -> int {
      a: A = A();
      n: int = a.grab();
      println(Str<int>(n));
      println(Str<int>(a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "7\n7\n");
  g.expectNoLeaks("AssignSelfToLocal");
}

// -- A2 — match-arm bindings with ownership-taking consumers

// Binding whose subject is a plain owned variable (no recorded backing box):
// passing it to a consuming callee must recover the subject's box.
TEST(Ownership, MatchBindingPassedOwnedSubject) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn probe(a: A) { println(Str<int>(a.x)); }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A { probe(b); }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingPassedOwnedSubject");
}

// Binding consumed by explicit and implicit variable declarations.
TEST(Ownership, MatchBindingVarDecls) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A {
          c: A = b;
          d = b;
          println(Str<int>(c.x + d.x));
        }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "10\n");
  g.expectNoLeaks("MatchBindingVarDecls");
}

// Backed binding (call-result subject) consumed via implicit declaration —
// emitImplicitVarDecl's fallback must recover the subject's box.
TEST(Ownership, MatchBindingCallSubjectImplicitDecl) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn make() -> Obj { return A(); }
    fn main() -> int {
      match make() {
        b: A {
          c = b;
          println(Str<int>(c.x));
        }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingCallSubjectImplicitDecl");
}

// Binding stored into a class field: the field slot must end up sharing the
// subject's box (retained), and the nested read h.a.x must unwrap it.
TEST(Ownership, MatchBindingMemberAssign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    class Holder {
      a: A;
    }

    impl Holder {
      fn __init__(a0: A) { self.a = a0; }
    }
    fn main() -> int {
      o: Obj = A();
      h: Holder = Holder(A());
      match o {
        b: A { h.a = b; }
        _ { println("no"); }
      }
      println(Str<int>(h.a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingMemberAssign");
}

// Copying an unowned binding retains the subject's box — never a fresh box
// around the raw alias.
TEST(Ownership, MatchBindingCopied) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A {
          c = b;
          println(Str<int>(c.x));
        }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingCopied");
}

// Control: the recorded-backing fast path (call-result subject + explicit
// declaration) must keep working alongside the backpointer recovery.
TEST(Ownership, MatchBindingBackedControl) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn make() -> Obj { return A(); }
    fn main() -> int {
      match make() {
        b: A {
          c: A = b;
          println(Str<int>(c.x));
        }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingBackedControl");
}

// -- A4 — array-typed class fields

// An array field holds its elements in the object: storing one must retain
// the strings it copies, as the destructor releases them, so a raw store would
// be a use-after-free / double free.
TEST(Ownership, ArrayFieldStoreAndRead) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H {
      arr: Str[3];
    }

    impl H {
      fn __init__(a: Str[3]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([Str(1), Str(2), Str(3)]);
      println(h.arr[0]);
      println(Str<int>(h.arr.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n3\n");
  g.expectNoLeaks("ArrayFieldStoreAndRead");
}

// Re-assigning an array field releases the elements it replaces and retains
// the new ones, also one the new value takes from the old.
TEST(Ownership, ArrayFieldReassign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H {
      arr: Str[2];
    }

    impl H {
      fn __init__(a: Str[2]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([Str(1), Str(2)]);
      h.arr = [Str(7), h.arr[0]];
      println(h.arr[0] + h.arr[1] + Str<int>(h.arr.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "712\n");
  g.expectNoLeaks("ArrayFieldReassign");
}

// -- A5 — len() on non-identifier receivers

// len() on a call-result array must be the array's length, not its
// elements' (it previously resolved through a coincidental Str slot).
TEST(Ownership, LenOnCallResult) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn makeArr() -> Str[3] { return [Str(1), Str(2), Str(3)]; }
    fn main() -> int {
      println(Str<int>(makeArr().len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n");
  g.expectNoLeaks("LenOnCallResult");
}

// Implicit declaration from an object-array element: `y = arr[i]` must type
// `y` from the subscript's resolved type (not fall back to Obj) and share the
// element's box instead of double-boxing the raw element pointer.
TEST(Ownership, ImplicitDeclFromObjectArrayElement) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__(n: int) { self.x = n; }
    }
    fn main() -> int {
      arr: A[2] = [A(1), A(2)];
      y = arr[1];
      println(Str<int>(y.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "2\n");
  g.expectNoLeaks("ImplicitDeclFromObjectArrayElement");
}

// -- Nested member access (surfaced by A2's member-assign repro)

// h.a.x must unwrap the intermediate field's box before GEPing — reading a
// "field" out of the PaykanShared box returns its refCount/object words.
TEST(Ownership, NestedMemberAccessUnwrapsBox) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    class H {
      a: A;
    }

    impl H {
      fn __init__(a0: A) { self.a = a0; }
    }
    fn main() -> int {
      h: H = H(A());
      println(Str<int>(h.a.x));
      h.a.x = 9;
      println(Str<int>(h.a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n9\n");
  g.expectNoLeaks("NestedMemberAccessUnwrapsBox");
}

// -- B8 — vtable-convention narrowing on assignment

// `y = derived` on a base-typed variable must narrow the scope type to
// the concrete class so later method calls dispatch with the derived
// vtable convention (same as the implicit-declaration path).
TEST(Ownership, AssignNarrowsBaseVar) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Animal {
      fn speak() -> Str { return "..."; }
    }

    impl Animal {
      fn __init__() {}
    }
    class Dog : Animal {
      fn speak() -> Str { return "woof"; }
    }

    impl Dog {
      fn __init__() { __super__(); }
    }
    fn main() -> int {
      d: Dog = Dog();
      a: Animal = Animal();
      a = d;
      println(a.speak());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "woof\n");
  g.expectNoLeaks("AssignNarrowsBaseVar");
}

// -- Unique-box invariant at the runtime level

// Boxing the same object twice yields the SAME box with a bumped refcount —
// the invariant that makes every raw-alias boxing site safe.
TEST(Ownership, SharedNewAcquiresExistingBox) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *first = PaykanShared_new(obj);
  PaykanShared *second = PaykanShared_new(obj);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first->refCount, 2);
  EXPECT_EQ(obj->shared, first);
  Paykan_release(second);
  EXPECT_EQ(first->refCount, 1);
  Paykan_release(first); // destroys obj, frees the box
}

// The immortal None singleton cycles cleanly: boxing installs a box, the
// final release clears the backpointer (destroy is a no-op), and a later
// boxing starts fresh.
TEST(Ownership, NoneSingletonBoxCycle) {
  ASSERT_EQ(PaykanObject_None.shared, nullptr);
  PaykanShared *s = PaykanShared_new(&PaykanObject_None);
  EXPECT_EQ(PaykanObject_None.shared, s);
  Paykan_release(s);
  EXPECT_EQ(PaykanObject_None.shared, nullptr);
}
