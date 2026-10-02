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
//   A4  array-typed class fields stored as boxes, not raw pointers
//   A5  push/pop and equality on receivers that are not bare identifiers
//   B8  vtable-convention narrowing through `mov` on assignment
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

// ============================================================================
// A1 — `self` used as a value
// ============================================================================

// Returning `self` from a method must hand out a +1 on the object's existing
// box, not wrap the raw receiver in a second box (double free on scope exit).
TEST(Ownership, ReturnSelf) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class B {
      x: int;
      fn __init__() { self.x = 1; }
      fn me() -> B { return self; }
    }
    fn main() -> int {
      b: B = B();
      c: B = b.me();
      println(StrInt(c.x));
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
      fn __init__() { self.x = 0; }
      fn bump() -> B { self.x = self.x + 1; return self; }
    }
    fn main() -> int {
      b: B = B();
      c: B = b.bump().bump().bump();
      println(StrInt(c.x));
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
      fn __init__() { self.x = 1; }
      fn send() { probe(self); }
    }
    fn probe(b: B) { println(StrInt(b.x)); }
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
      fn __init__() { self.x = 7; }
      fn grab() -> int {
        y = self;
        return y.x;
      }
    }
    fn main() -> int {
      a: A = A();
      n: int = a.grab();
      println(StrInt(n));
      println(StrInt(a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "7\n7\n");
  g.expectNoLeaks("AssignSelfToLocal");
}

// ============================================================================
// A2 — match-arm bindings with ownership-taking consumers
// ============================================================================

// Binding whose subject is a plain owned variable (no recorded backing box):
// passing it to a consuming callee must recover the subject's box.
TEST(Ownership, MatchBindingPassedOwnedSubject) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn __init__() { self.x = 5; }
    }
    fn probe(a: A) { println(StrInt(a.x)); }
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
      fn __init__() { self.x = 5; }
    }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A {
          c: A = b;
          d = b;
          println(StrInt(c.x + d.x));
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
      fn __init__() { self.x = 5; }
    }
    fn make() -> Obj { return A(); }
    fn main() -> int {
      match make() {
        b: A {
          c = b;
          println(StrInt(c.x));
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
      fn __init__() { self.x = 5; }
    }
    class Holder {
      a: A;
      fn __init__(a0: A) { self.a = a0; }
    }
    fn main() -> int {
      o: Obj = A();
      h: Holder = Holder(A());
      match o {
        b: A { h.a = b; }
        _ { println("no"); }
      }
      println(StrInt(h.a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingMemberAssign");
}

// `mov` of an unowned binding degenerates to a retain of the subject's box —
// never a fresh box around the raw alias.
TEST(Ownership, MatchBindingMov) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn __init__() { self.x = 5; }
    }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A {
          c = mov b;
          println(StrInt(c.x));
        }
        _ { println("no"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n");
  g.expectNoLeaks("MatchBindingMov");
}

// Control: the recorded-backing fast path (call-result subject + explicit
// declaration) must keep working alongside the backpointer recovery.
TEST(Ownership, MatchBindingBackedControl) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn __init__() { self.x = 5; }
    }
    fn make() -> Obj { return A(); }
    fn main() -> int {
      match make() {
        b: A {
          c: A = b;
          println(StrInt(c.x));
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

// ============================================================================
// A4 — array-typed class fields
// ============================================================================

// An array field must be stored as a retained box: the destructor releases
// the slot as a box, so a raw store would be a use-after-free / double free.
TEST(Ownership, ArrayFieldStoreAndRead) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H {
      arr: int[];
      fn __init__(a: int[]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([1, 2, 3]);
      println(StrInt(h.arr[0]));
      println(StrInt(h.arr.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "1\n3\n");
  g.expectNoLeaks("ArrayFieldStoreAndRead");
}

// Re-assigning an array field releases the old box and retains the new one.
TEST(Ownership, ArrayFieldReassign) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H {
      arr: int[];
      fn __init__(a: int[]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([1, 2, 3]);
      h.arr = [7, 8];
      println(StrInt(h.arr[0] + h.arr.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "9\n");
  g.expectNoLeaks("ArrayFieldReassign");
}

// ============================================================================
// A5 — push/pop/length/equality on non-identifier receivers
// ============================================================================

// push on a call-result receiver: the receiver type comes from the resolved
// expression type, and the fresh receiver box is released after the call.
TEST(Ownership, PushOnCallResult) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn makeArr() -> int[] { return [1, 2]; }
    fn main() -> int {
      makeArr().push(3);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("PushOnCallResult");
}

// pop on a call-result receiver returns the element and frees the temporary.
TEST(Ownership, PopOnCallResult) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn makeArr() -> int[] { return [1, 2]; }
    fn main() -> int {
      n: int = makeArr().pop();
      println(StrInt(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "2\n");
  g.expectNoLeaks("PopOnCallResult");
}

// push through a nested subscript receiver (m[0] is an ArrayType expression,
// not a bare identifier).
TEST(Ownership, PushOnNestedSubscript) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      m: int[][] = [[1, 2], [3, 4]];
      m[0].push(9);
      println(StrInt(m[0].len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n");
  g.expectNoLeaks("PushOnNestedSubscript");
}

// len() on a call-result array must dispatch through the Array vtable (it
// previously resolved through a coincidental Str slot).
TEST(Ownership, LenOnCallResult) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn makeArr() -> int[] { return [1, 2, 3]; }
    fn main() -> int {
      println(StrInt(makeArr().len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "3\n");
  g.expectNoLeaks("LenOnCallResult");
}

// Array equality on literal temporaries: identity semantics (two distinct
// temporaries are not equal), single receiver unwrap, no leaks.
TEST(Ownership, ArrayEqualityTemporaries) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      if ([1, 2] == [1, 2]) { println("True"); } else { println("False"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "False\n");
  g.expectNoLeaks("ArrayEqualityTemporaries");
}

// Array equality on the same owned variable: identity holds, and the equals
// ABI (consumed boxed `other`) balances against the variable's ownership.
TEST(Ownership, ArrayEqualitySameVariable) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int[] = [1, 2];
      if (a == a) { println("True"); } else { println("False"); }
      if (a != [1, 2]) { println("ne"); } else { println("eq"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "True\nne\n");
  g.expectNoLeaks("ArrayEqualitySameVariable");
}

// Implicit declaration from an object-array element: `y = arr[i]` must type
// `y` from the subscript's resolved type (not fall back to Obj) and share the
// element's box instead of double-boxing the raw element pointer.
TEST(Ownership, ImplicitDeclFromObjectArrayElement) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn __init__(n: int) { self.x = n; }
    }
    fn main() -> int {
      arr: A[] = [A(1), A(2)];
      y = arr[1];
      println(StrInt(y.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "2\n");
  g.expectNoLeaks("ImplicitDeclFromObjectArrayElement");
}

// ============================================================================
// Nested member access (surfaced by A2's member-assign repro)
// ============================================================================

// h.a.x must unwrap the intermediate field's box before GEPing — reading a
// "field" out of the PaykanShared box returns its refCount/object words.
TEST(Ownership, NestedMemberAccessUnwrapsBox) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class A {
      x: int;
      fn __init__() { self.x = 5; }
    }
    class H {
      a: A;
      fn __init__(a0: A) { self.a = a0; }
    }
    fn main() -> int {
      h: H = H(A());
      println(StrInt(h.a.x));
      h.a.x = 9;
      println(StrInt(h.a.x));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "5\n9\n");
  g.expectNoLeaks("NestedMemberAccessUnwrapsBox");
}

// ============================================================================
// B8 — vtable-convention narrowing through `mov` on assignment
// ============================================================================

// `y = mov derived` on a base-typed variable must narrow the scope type to
// the concrete class so later method calls dispatch with the derived
// vtable convention (same as the implicit-declaration path).
TEST(Ownership, MovNarrowsAssignedBaseVar) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Animal {
      fn __init__() {}
      fn speak() -> Str { return "..."; }
    }
    class Dog : Animal {
      fn __init__() { __super__(); }
      fn speak() -> Str { return "woof"; }
    }
    fn main() -> int {
      d: Dog = Dog();
      a: Animal = Animal();
      a = mov d;
      println(a.speak());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "woof\n");
  g.expectNoLeaks("MovNarrowsAssignedBaseVar");
}

// ============================================================================
// Unique-box invariant at the runtime level
// ============================================================================

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
