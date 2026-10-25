// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
// and the use-after-free family of reads without a reference of their own:
//
//   C1  reads through objects that a call may replace
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
  auto r = compileAndRun(R"(import ::io;
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
      io::println(Str<int>(c.x));
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
  auto r = compileAndRun(R"(import ::io;
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
      io::println(Str<int>(c.x));
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
  auto r = compileAndRun(R"(import ::io;
    class B {
      x: int;
      fn send() { probe(self); }
    }

    impl B {
      fn __init__() { self.x = 1; }
    }
    fn probe(b: B) { io::println(Str<int>(b.x)); }
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
  auto r = compileAndRun(R"(import ::io;
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
      io::println(Str<int>(n));
      io::println(Str<int>(a.x));
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
  auto r = compileAndRun(R"(import ::io;
    class A {
      x: int;
    }

    impl A {
      fn __init__() { self.x = 5; }
    }
    fn probe(a: A) { io::println(Str<int>(a.x)); }
    fn main() -> int {
      o: Obj = A();
      match o {
        b: A { probe(b); }
        _ { io::println("no"); }
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
  auto r = compileAndRun(R"(import ::io;
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
          io::println(Str<int>(c.x + d.x));
        }
        _ { io::println("no"); }
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
  auto r = compileAndRun(R"(import ::io;
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
          io::println(Str<int>(c.x));
        }
        _ { io::println("no"); }
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
  auto r = compileAndRun(R"(import ::io;
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
        _ { io::println("no"); }
      }
      io::println(Str<int>(h.a.x));
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
  auto r = compileAndRun(R"(import ::io;
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
          io::println(Str<int>(c.x));
        }
        _ { io::println("no"); }
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
  auto r = compileAndRun(R"(import ::io;
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
          io::println(Str<int>(c.x));
        }
        _ { io::println("no"); }
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
  auto r = compileAndRun(R"(import ::io;
    class H {
      arr: Str[3];
    }

    impl H {
      fn __init__(a: Str[3]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([Str(1), Str(2), Str(3)]);
      io::println(h.arr[0]);
      io::println(Str<int>(h.arr.len()));
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
  auto r = compileAndRun(R"(import ::io;
    class H {
      arr: Str[2];
    }

    impl H {
      fn __init__(a: Str[2]) { self.arr = a; }
    }
    fn main() -> int {
      h: H = H([Str(1), Str(2)]);
      h.arr = [Str(7), h.arr[0]];
      io::println(h.arr[0] + h.arr[1] + Str<int>(h.arr.len()));
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
  auto r = compileAndRun(R"(import ::io;
    fn makeArr() -> Str[3] { return [Str(1), Str(2), Str(3)]; }
    fn main() -> int {
      io::println(Str<int>(makeArr().len()));
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
  auto r = compileAndRun(R"(import ::io;
    class A {
      x: int;
    }

    impl A {
      fn __init__(n: int) { self.x = n; }
    }
    fn main() -> int {
      arr: A[2] = [A(1), A(2)];
      y = arr[1];
      io::println(Str<int>(y.x));
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
  auto r = compileAndRun(R"(import ::io;
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
      io::println(Str<int>(h.a.x));
      h.a.x = 9;
      io::println(Str<int>(h.a.x));
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
  auto r = compileAndRun(R"(import ::io;
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
      io::println(a.speak());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "woof\n");
  g.expectNoLeaks("AssignNarrowsBaseVar");
}

// -- C1 — reads through objects that a call may replace
//
// In each statement a call replaces the field a value read before it is in
// (or the field of the object that holds it), and makes objects of the same
// size, which take the freed memory over: what the statement uses is what
// it read, alive until it is used (docs/pir.md §7, "Reads a call may
// replace").

// A method's receiver, the operands of `+` and `==`, the object a field
// assignment writes to, the storage an element is read from or assigned in.
TEST(Ownership, ReadsThroughObjectsACallReplacesStayAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B {
      s: Str;
      a: int[3];
      names: Str[2];
      fn m(h: H) -> Str {
        swap(h);
        return self.s;
      }
    }
    class H { b: B; }
    fn swap(h: H) {
      h.b = B("other" + Str(9), [7, 8, 9], ["p" + Str(0), "q"]);
      y = B("reused" + Str(1), [4, 5, 6], ["r" + Str(1), "t"]);
    }
    fn g(h: H) -> Str {
      swap(h);
      return "!";
    }
    fn same(h: H) -> Str {
      swap(h);
      return "first" + Str(1);
    }
    fn gi(h: H) -> int {
      swap(h);
      return 1;
    }
    fn made(h: H) -> Str {
      s = "x" + Str(5);
      swap(h);
      return s;
    }
    fn mk() -> H { return H(B("first" + Str(1), [1, 2, 3], ["n" + Str(1), "o"])); }
    fn main() -> int {
      h = mk();
      io::println("receiver: " + h.b.m(h));
      h = mk();
      io::println("operand: " + (h.b.s + g(h)));
      h = mk();
      io::println("equal: " + Str(h.b.s == same(h)));
      h = mk();
      h.b.s = g(h);
      io::println("target: " + h.b.s);
      h = mk();
      h.b.s += g(h);
      io::println("compound: " + h.b.s);
      h = mk();
      io::println("element: " + Str(h.b.a[gi(h)]));
      h = mk();
      io::println("string element: " + (h.b.names[0] + g(h)));
      h = mk();
      h.b.names[1] = made(h);
      io::println("element target: " + h.b.names[1]);
      h = mk();
      h.b.names[gi(h)] = "x" + Str(2);
      io::println("element index: " + h.b.names[1]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "receiver: first1\noperand: first1!\nequal: True\n"
                      "target: other9\ncompound: other9\nelement: 2\n"
                      "string element: n1!\nelement target: q\n"
                      "element index: q\n");
  g.expectNoLeaks("ReadsThroughObjectsACallReplacesStayAlive");
}

// A method may replace the field its receiver was read from: through what
// the receiver reaches, or a closure it calls (a `view fn` too).
TEST(Ownership, AReceiverItsMethodReplacesStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node {
      name: Str;
      parent: Holder?;
      fire: () -> void;
      fn drop() -> Str {
        match self.parent {
          p: Holder { p.node = Node("other" + Str(0), None, fn() {}); }
          None { }
        }
        y = Node("y" + Str(9), None, fn() {});
        return self.name;
      }
      view fn call() -> Str {
        self.fire();
        return self.name + ".";
      }
    }
    class Holder { node: Node; }
    fn main() -> int {
      h = Holder(Node("first" + Str(1), None, fn() {}));
      h.node.parent = h;
      io::println("method: " + h.node.drop());
      h.node.parent = None;
      w = Holder(Node("one" + Str(1), None, fn() {}));
      v = w;
      w.node = Node("two" + Str(2), None, fn() {
        v.node = Node("three" + Str(3), None, fn() {});
        y = Node("four" + Str(4), None, fn() {});
      });
      io::println("closure: " + w.node.call());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "method: first1\nclosure: two2.\n");
  g.expectNoLeaks("AReceiverItsMethodReplacesStaysAlive");
}

// The storage of a value held in an object's field, used where it is while
// a call replaces the object: passed to a `view` slice or by value, a
// struct method's `self`, an `inout` argument, an `inout` result (a
// function's or a method's), an `inout` local of either.  A write lands in
// the object it was read from, which nothing else keeps.
TEST(Ownership, StorageInAnObjectACallReplacesStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    struct S { v: Str; }
    impl S {
      view fn take(h: H) -> Str {
        swap(h);
        return self.v + "";
      }
    }
    class A {
      arr: Str[2];
      s: S;
      k: Str;
      fn slot(h: H) -> inout Str {
        swap(h);
        return self.k;
      }
      fn key() -> inout Str { return self.k; }
    }
    class H { a: A; }
    fn swap(h: H) {
      h.a = A(["p" + Str(0), "q"], S("zero" + Str(0)), "zk" + Str(0));
      y = A(["r" + Str(1), "t"], S("three" + Str(3)), "tk" + Str(3));
    }
    fn first(xs: view Str[], h: H) -> Str {
      swap(h);
      return xs[0] + "";
    }
    fn copied(s: S, t: Str) -> Str { return s.v + t; }
    fn bang(h: H) -> Str {
      swap(h);
      return "!";
    }
    fn put(x: inout Str, h: H) {
      swap(h);
      x = "five" + Str(5);
    }
    fn pick(x: inout Str) -> inout Str { return x; }
    fn mk() -> H { return H(A(["one" + Str(1), "two"], S("one" + Str(1)), "k" + Str(1))); }
    fn main() -> int {
      h = mk();
      io::println("slice: " + first(h.a.arr, h));
      h = mk();
      io::println("by value: " + copied(h.a.s, bang(h)));
      h = mk();
      io::println("self: " + h.a.s.take(h));
      h = mk();
      put(h.a.s.v, h);
      io::println("inout field: " + h.a.s.v);
      h = mk();
      put(h.a.slot(h), h);
      io::println("inout result: " + h.a.k);
      h = mk();
      c = h;
      x: inout = h.a.s.v;
      swap(c);
      x = "six" + Str(6);
      io::println("local: " + x + " " + c.a.s.v);
      h = mk();
      c = h;
      y: inout = pick(h.a.k);
      swap(c);
      y = "seven" + Str(7);
      io::println("picked: " + y + " " + c.a.k);
      h = mk();
      c = h;
      z: inout = h.a.key();
      swap(c);
      z = "eight" + Str(8);
      io::println("bound result: " + z + " " + c.a.k);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "slice: one1\nby value: one1!\nself: one1\n"
                      "inout field: zero0\ninout result: zk0\n"
                      "local: six6 zero0\npicked: seven7 zk0\n"
                      "bound result: eight8 zk0\n");
  g.expectNoLeaks("StorageInAnObjectACallReplacesStaysAlive");
}

// A `match` arm's `n: inout` of a struct's field in an object, a tuple
// compared in place, the object an array field is assigned in.
TEST(Ownership, MatchedAndComparedStorageACallReplacesStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Box { v: Str; }
    struct T { b: Box; }
    class C {
      t: T;
      pair: (Str, int);
      arr: Str[2];
    }
    class D { c: C; }
    fn swap(d: D) -> (Str, int) {
      d.c = C(T(Box("zero" + Str(0))), ("zp" + Str(0), 0), ["za" + Str(0), "zb"]);
      y = C(T(Box("three" + Str(3))), ("tp" + Str(3), 3), ["ta" + Str(3), "tb"]);
      return ("one" + Str(1), 1);
    }
    fn fresh(d: D) -> Str[2] {
      xs = ["new" + Str(1), "x"];
      swap(d);
      return xs;
    }
    fn mk() -> D { return D(C(T(Box("one" + Str(1))), ("one" + Str(1), 1), ["a" + Str(1), "b"])); }
    fn main() -> int {
      d = mk();
      e = d;
      match d.c.t.b {
        n: inout Box {
          swap(e);
          n = Box("five" + Str(5));
          io::println("match: " + n.v + " " + e.c.t.b.v);
        }
      }
      d = mk();
      io::println("tuple equality: " + Str(d.c.pair == swap(d)));
      d = mk();
      d.c.arr = fresh(d);
      io::println("array field: " + d.c.arr[0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut,
            "match: five5 zero0\ntuple equality: True\narray field: za0\n");
  g.expectNoLeaks("MatchedAndComparedStorageACallReplacesStaysAlive");
}

// What an `inout` parameter, a slice or a struct method's `self` names is
// the caller's, here an object's field a call replaces in place; a native
// function's borrowed argument; an `inout` result read after its call; a
// field returned as `Str(self.s)`, a reference of the caller's own.
TEST(Ownership, CallersStorageACallReplacesStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    native fn charAt(s: view Str, i: int) -> char = "PaykanString_char_at";
    class A {
      k: Str;
      arr: Str[2];
      fn slot(h: H) -> inout Str {
        h.a = A("zk" + Str(0), ["p" + Str(0), "q"]);
        y = A("tk" + Str(3), ["r" + Str(1), "t"]);
        return self.k;
      }
    }
    class H {
      a: A;
      s: Str;
      fn show() -> Str { return Str(self.s); }
    }
    struct S { v: Str; }
    class G { s: S; }
    impl S {
      view fn both(g: G) -> Str { return self.v + reset(g); }
    }
    fn reset(g: G) -> Str {
      g.s = S("zero" + Str(0));
      y = S("three" + Str(3));
      return "!";
    }
    fn bump(h: H) -> Str {
      h.a.k = "zk" + Str(0);
      h.a.arr[0] = "p" + Str(0);
      h.s = "zs" + Str(0);
      y = "tk" + Str(3);
      return "!";
    }
    fn index(h: H) -> int {
      bump(h);
      return 1;
    }
    fn param(x: inout Str, h: H) -> Str { return x + bump(h); }
    fn slice(xs: view Str[], h: H) -> Str { return xs[0] + bump(h); }
    fn mk() -> H { return H(A("k" + Str(1), ["one" + Str(1), "two"]), "ab" + Str(1)); }
    fn main() -> int {
      h = mk();
      io::println("inout parameter: " + param(h.a.k, h));
      h = mk();
      io::println("slice: " + slice(h.a.arr, h));
      g = G(S("one" + Str(1)));
      io::println("struct self: " + g.s.both(g));
      h = mk();
      io::println("native: " + Str(charAt(h.s, index(h))));
      h = mk();
      io::println("inout result: " + (h.a.slot(h) + "!"));
      h = mk();
      io::println("Str of a field: " + h.show() + h.show());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "inout parameter: k1!\nslice: one1!\nstruct self: one1!\n"
                      "native: b\ninout result: k1!\n"
                      "Str of a field: ab1ab1\n");
  g.expectNoLeaks("CallersStorageACallReplacesStaysAlive");
}

// A function handed a variable's last reference reads a field of what it
// was given across a call that replaces it: the read holds a reference of
// its own, as code must for an object a temporary made.
TEST(Ownership, AFieldOfWhatALastUseHandedOnStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { name: Str; }
    class Holder { node: Node; }
    fn take(n: Node) -> Str { return "took"; }
    fn replace(h: Holder) -> Str {
      h.node = Node("other");
      return "!";
    }
    fn use(h: Holder) -> Str { return h.node.name + replace(h); }
    fn main() -> int {
      a = Node("first" + Str(1));
      h = Holder(a);
      t = take(a);
      io::println(t + " " + use(h));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "took first1!\n");
  g.expectNoLeaks("AFieldOfWhatALastUseHandedOnStaysAlive");
}

// An `inout` result in the object an `inout` argument of a class holds
// (`return b.k;` of `b: inout B`), used after its call while another
// reference to the holder replaces that object: bound to an `inout` local,
// passed on to an `inout` parameter, a method's, an `inout` parameter passed
// on, a field of a field.  The caller holds the object the argument held
// when the call returned; the write lands in it, and the object made next
// keeps its own value.
TEST(Ownership, AnInoutResultInAnInoutArgumentsObjectStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B { k: Str; }
    class X { b: B; }
    class H {
      b: B;
      x: X;
    }
    class P {
      n: int;
      fn pick(b: inout B) -> inout Str { return b.k; }
    }
    fn pick(b: inout B) -> inout Str { return b.k; }
    fn put(x: inout Str, h: H) -> Str {
      h.b = B("zero" + Str(0));
      y = B("three" + Str(3));
      x = "five" + Str(5);
      return y.k;
    }
    fn passOn(b: inout B, c: H) -> Str {
      x: inout = pick(b);
      c.b = B("zero" + Str(0));
      y = B("three" + Str(3));
      x = "five" + Str(5);
      return x + " " + y.k;
    }
    fn mk() -> H { return H(B("one" + Str(1)), X(B("two" + Str(2)))); }
    fn main() -> int {
      h = mk();
      c = h;
      x: inout = pick(h.b);
      c.b = B("zero" + Str(0));
      y = B("three" + Str(3));
      x = "five" + Str(5);
      io::println("bound: " + x + " " + y.k + " " + c.b.k);
      h = mk();
      io::println("passed on: " + put(pick(h.b), h) + " " + h.b.k);
      h = mk();
      c = h;
      p = P(1);
      m: inout = p.pick(h.b);
      c.b = B("zero" + Str(0));
      w = B("three" + Str(3));
      m = "five" + Str(5);
      io::println("method: " + m + " " + w.k + " " + c.b.k);
      h = mk();
      io::println("inout parameter: " + passOn(h.b, h) + " " + h.b.k);
      h = mk();
      c = h;
      d: inout = pick(h.x.b);
      c.x.b = B("zero" + Str(0));
      z = B("three" + Str(3));
      d = "five" + Str(5);
      io::println("field of a field: " + d + " " + z.k + " " + c.x.b.k);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "bound: five5 three3 zero0\npassed on: three3 zero0\n"
                      "method: five5 three3 zero0\n"
                      "inout parameter: five5 three3 zero0\n"
                      "field of a field: five5 three3 zero0\n");
  g.expectNoLeaks("AnInoutResultInAnInoutArgumentsObjectStaysAlive");
}

// The same for an object held in a value held in memory: in a struct
// argument's field, a struct receiver's field, a tuple argument's element,
// an array argument's element, and for an `inout` local of the result in a
// loop.  The caller holds a copy of the value, its references retained.
TEST(Ownership, AnInoutResultInAnObjectAValueHoldsStaysAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B { k: Str; }
    struct S {
      b: B;
      n: int;
    }
    impl S {
      fn slot() -> inout Str { return self.b.k; }
    }
    class H {
      s: S;
      t: (B, int);
      a: B[6];
    }
    fn pick(s: inout S) -> inout Str { return s.b.k; }
    fn first(t: inout (B, int)) -> inout Str { return t.0.k; }
    fn last(a: inout B[6]) -> inout Str { return a[5].k; }
    fn put(x: inout Str, h: H) -> Str {
      h.s = S(B("zero" + Str(0)), 0);
      y = B("three" + Str(3));
      x = "five" + Str(5);
      return y.k;
    }
    fn six(t: Str) -> B[6] { return [B("1"), B("2"), B("3"), B("4"), B("5"), B(t)]; }
    fn mk() -> H {
      return H(S(B("one" + Str(1)), 1), (B("one" + Str(1)), 1), six("one" + Str(1)));
    }
    fn main() -> int {
      h = mk();
      c = h;
      x: inout = pick(h.s);
      c.s = S(B("zero" + Str(0)), 0);
      y = B("three" + Str(3));
      x = "five" + Str(5);
      io::println("struct: " + x + " " + y.k + " " + c.s.b.k);
      h = mk();
      io::println("passed on: " + put(pick(h.s), h) + " " + h.s.b.k);
      h = mk();
      c = h;
      r: inout = h.s.slot();
      c.s = S(B("zero" + Str(0)), 0);
      w = B("three" + Str(3));
      r = "five" + Str(5);
      io::println("struct self: " + r + " " + w.k + " " + c.s.b.k);
      h = mk();
      c = h;
      t: inout = first(h.t);
      c.t = (B("zero" + Str(0)), 0);
      u = B("three" + Str(3));
      t = "five" + Str(5);
      io::println("tuple: " + t + " " + u.k + " " + c.t.0.k);
      h = mk();
      c = h;
      e: inout = last(h.a);
      c.a = six("zero" + Str(0));
      v = B("three" + Str(3));
      e = "five" + Str(5);
      io::println("array: " + e + " " + v.k + " " + c.a[5].k);
      h = mk();
      c = h;
      for i in 0..2 {
        l: inout = pick(h.s);
        c.s = S(B("zero" + Str(i)), i);
        q = B("three" + Str(3));
        l = "five" + Str(i);
        io::println("loop: " + l + " " + q.k + " " + c.s.b.k);
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "struct: five5 three3 zero0\npassed on: three3 zero0\n"
                      "struct self: five5 three3 zero0\n"
                      "tuple: five5 three3 zero0\narray: five5 three3 zero0\n"
                      "loop: five0 three3 zero0\nloop: five1 three3 zero1\n");
  g.expectNoLeaks("AnInoutResultInAnObjectAValueHoldsStaysAlive");
}

// An `inout` result in an object reached through a field of `self` was the
// one its caller could not keep alive: with another reference replacing
// `h.c` while the result was used, `put(h.slot(), h)` and an `inout` local
// of `h.slot()` wrote to freed memory.  Such a result, the place itself or
// a call's result there, is now a compile error.
TEST(Ownership, AnInoutResultInAnObjectReachedThroughAFieldIsAnError) {
  for (const char *ret : {"return self.c.k;", "return self.c.key();"}) {
    auto r = compileAndRun(std::string(R"(import ::io;
      class C {
        k: Str;
        fn key() -> inout Str { return self.k; }
      }
      class H {
        c: C;
        fn slot() -> inout Str { )") +
                           ret + R"( }
      }
      fn put(x: inout Str, h: H) {
        h.c = C("zero" + Str(0));
        y = C("three" + Str(3));
        x = "five" + Str(5);
      }
      fn main() -> int {
        h = H(C("one" + Str(1)));
        put(h.slot(), h);
        d = h;
        x: inout = h.slot();
        d.c = C("zero" + Str(0));
        y = C("three" + Str(3));
        x = "five" + Str(5);
        io::println(h.c.k);
        return 0;
      }
    )");
    EXPECT_FALSE(r.CompileOk) << ret;
    const std::string what =
        std::string(ret).find("key") == std::string::npos
            ? "'slot' returns an 'inout' reference into 'self.c', a separate "
              "object"
            : "'slot' returns the 'inout' result of 'key', which can point "
              "into 'self.c', a separate object";
    EXPECT_NE(r.StdErr.find(what + ": an 'inout' result can only point into "
                                   "'self' or into an 'inout' parameter's "
                                   "storage; return a copy, or call an "
                                   "accessor on 'self.c' directly"),
              std::string::npos)
        << r.StdErr;
  }
}

// The accessor of the object itself, called where its result is used
// (`h.c.slot()`), keeps that object alive while the result is: passed to an
// `inout` parameter, or bound to an `inout` local, while another reference
// replaces `h.c`, the write lands in the object the result was in, which
// nothing else keeps.  So do the `inout` results that may still return a
// call's: another method's in `self`, one in an `inout` parameter's object,
// one in an object a struct parameter holds.  A `view` result in an object
// reached through a field is a copy, which the replacement leaves as it was.
TEST(Ownership, AnAccessorOfTheObjectItselfKeepsItAlive) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class C {
      k: Str;
      fn slot() -> inout Str { return self.k; }
    }
    struct S {
      c: C;
      n: int;
    }
    impl S {
      fn slot() -> inout Str { return self.c.k; }
    }
    class H {
      c: C;
      k: Str;
      s: S;
      fn key() -> inout Str { return self.k; }
      fn viaKey() -> inout Str { return self.key(); }
      view fn peek() -> view Str { return self.c.k; }
    }
    fn pick(c: inout C) -> inout Str { return c.slot(); }
    fn viaS(s: inout S) -> inout Str { return s.slot(); }
    fn swap(h: H) -> Str {
      h.c = C("zero" + Str(0));
      h.s = S(C("zero" + Str(0)), 0);
      y = C("three" + Str(3));
      return y.k;
    }
    fn put(x: inout Str, h: H) -> Str {
      t = swap(h);
      x = "five" + Str(5);
      return t;
    }
    fn show(s: view Str, h: H) -> Str {
      t = swap(h);
      return s + " " + t;
    }
    fn mk() -> H { return H(C("one" + Str(1)), "k" + Str(1), S(C("one" + Str(1)), 1)); }
    fn main() -> int {
      h = mk();
      io::println("accessor: " + put(h.c.slot(), h) + " " + h.c.k);
      h = mk();
      c = h;
      x: inout = h.c.slot();
      t = swap(c);
      x = "five" + Str(5);
      io::println("bound accessor: " + x + " " + t + " " + c.c.k);
      h = mk();
      io::println("own accessor: " + put(h.viaKey(), h) + " " + h.k);
      h = mk();
      io::println("parameter's object: " + put(pick(h.c), h) + " " + h.c.k);
      h = mk();
      io::println("struct's object: " + put(viaS(h.s), h) + " " + h.s.c.k);
      h = mk();
      io::println("view: " + show(h.peek(), h) + " " + h.c.k);
      h = mk();
      c = h;
      v: view = h.peek();
      t = swap(c);
      io::println("view local: " + v + " " + t + " " + c.c.k);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "accessor: three3 zero0\n"
                      "bound accessor: five5 three3 zero0\n"
                      "own accessor: three3 five5\n"
                      "parameter's object: three3 zero0\n"
                      "struct's object: three3 zero0\n"
                      "view: one1 three3 zero0\n"
                      "view local: one1 three3 zero0\n");
  g.expectNoLeaks("AnAccessorOfTheObjectItselfKeepsItAlive");
}

// A call's `inout` result is one object past what it borrowed only when
// that object can hold a place of its type.  A `C` holds no `S` and no `C`,
// so `self.s.me()`, `self.s.ref()` and `self.w.get()` return storage in
// `self`, and `s.ref().k` a field of the object the struct parameter holds,
// as `s.c.k` does.  With another reference replacing that storage while the
// result is used, the write lands where the result points: in `h`, or in
// the object the argument held, which the caller keeps alive.
TEST(Ownership, AnInoutResultOfATypeNoObjectHoldsStaysInItsStorage) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class C {
      k: Str;
      n: int;
    }
    struct S {
      c: C;
      n: int;
    }
    impl S {
      fn me() -> inout S { return self; }
      fn ref() -> inout C { return self.c; }
    }
    struct W<T> {
      v: T;
    }
    impl W<T> {
      fn get() -> inout T { return self.v; }
    }
    class H {
      s: S;
      w: W<C>;
      fn num() -> inout int { return self.s.me().n; }
      fn whole() -> inout S { return self.s.me(); }
      fn ref() -> inout C { return self.s.ref(); }
      fn get() -> inout C { return self.w.get(); }
    }
    fn key(s: inout S) -> inout Str { return s.ref().k; }
    fn swap(h: H) -> Str {
      h.s = S(C("zero" + Str(0), 0), 0);
      h.w = W<C>(C("zero" + Str(0), 0));
      y = C("three" + Str(3), 3);
      return y.k;
    }
    fn putN(x: inout int, h: H) -> Str {
      t = swap(h);
      x = 5;
      return t;
    }
    fn putS(x: inout S, h: H) -> Str {
      t = swap(h);
      x = S(C("five" + Str(5), 5), 5);
      return t;
    }
    fn putC(x: inout C, h: H) -> Str {
      t = swap(h);
      x = C("five" + Str(5), 5);
      return t;
    }
    fn putK(x: inout Str, h: H) -> Str {
      t = swap(h);
      x = "five" + Str(5);
      return t;
    }
    fn mk() -> H {
      return H(S(C("one" + Str(1), 1), 1), W<C>(C("one" + Str(1), 1)));
    }
    fn main() -> int {
      h = mk();
      io::println("struct's field: " + putN(h.num(), h) + " " + Str(h.s.n));
      h = mk();
      io::println("struct: " + putS(h.whole(), h) + " " + h.s.c.k);
      h = mk();
      io::println("struct's slot: " + putC(h.ref(), h) + " " + h.s.c.k);
      h = mk();
      io::println("generic struct's slot: " + putC(h.get(), h) + " " + h.w.v.k);
      h = mk();
      io::println("struct parameter's object: " + putK(key(h.s), h) + " " + h.s.c.k);
      h = mk();
      c = h;
      x: inout = h.ref();
      t = swap(c);
      x = C("five" + Str(5), 5);
      io::println("bound: " + x.k + " " + t + " " + c.s.c.k);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "struct's field: three3 5\n"
                      "struct: three3 five5\n"
                      "struct's slot: three3 five5\n"
                      "generic struct's slot: three3 five5\n"
                      "struct parameter's object: three3 zero0\n"
                      "bound: five5 three3 five5\n");
  g.expectNoLeaks("AnInoutResultOfATypeNoObjectHoldsStaysInItsStorage");
}

// A call of a function whose own body makes no store that releases a
// reference still replaces what its callees, a recursion, a method a
// subclass overrides, its `inout` parameter or a struct method's `self`
// replace: the receiver read from a field stays alive through the call, and
// so does a field's value read before it.  A method that stores a number
// and calls only such methods, and a stdlib map's lookups, replace nothing.
TEST(Ownership, AReleaseInACalleesCalleeStillHoldsTheRead) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections;
    class B {
      s: Str;
      n: int;
      fn virt(h: H) -> Str { return self.s; }
    }
    class D : B {
      fn virt(h: H) -> Str { return clear(h) + self.s; }
    }
    impl B {
      view fn count() -> int { return self.n; }
      fn bump() -> int {
        self.n = self.n + 1;
        return self.count();
      }
      view fn viaFree(h: H) -> Str {
        t = clear(h);
        return t + self.s;
      }
      view fn ping(k: int, h: H) -> Str {
        if k == 0 {
          t = clear(h);
          return t + self.s;
        }
        return self.pong(k - 1, h);
      }
      view fn pong(k: int, h: H) -> Str { return self.ping(k, h); }
      view fn rec(k: int, h: H) -> Str {
        if k == 0 {
          h.b = B("other" + Str(9), 9);
          y = B("reuse" + Str(1), 1);
          return "!" + self.s;
        }
        return self.rec(k - 1, h);
      }
      view fn setIt(x: inout Str) -> Str {
        x = "new" + Str(222);
        y = B("QQQQQ" + Str(8), 8);
        return "!";
      }
      fn viaVirt(h: H) -> Str { return self.virt(h); }
    }
    struct S {
      b: B;
    }
    impl S {
      fn reset() -> Str {
        self.b = B("other" + Str(9), 9);
        y = B("reuse" + Str(1), 1);
        return "!";
      }
    }
    class H {
      b: B;
      c: B;
    }
    class W { s: S; }
    fn clear(h: H) -> Str {
      h.b = B("other" + Str(9), 9);
      y = B("reuse" + Str(1), 1);
      return "!";
    }
    fn mk() -> H { return H(B("first" + Str(1), 1), B("c", 3)); }
    fn main() -> int {
      h = mk();
      io::println("free: " + h.b.viaFree(h));
      h = mk();
      io::println("mutual: " + h.b.ping(3, h));
      h = mk();
      io::println("self: " + h.b.rec(3, h));
      h = mk();
      io::println("inout: " + h.b.s + h.c.setIt(h.b.s) + h.b.s);
      h = mk();
      h.b = D("first" + Str(1), 1);
      io::println("virtual: " + h.b.viaVirt(h));
      w = W(S(B("first" + Str(1), 1)));
      io::println("struct: " + w.s.b.s + w.s.reset() + w.s.b.s);
      h = mk();
      io::println("number: " + h.b.s + Str(h.b.bump()) + Str(h.b.count()));
      m = collections::HashMap<Str, int>();
      m.put("a" + Str(1), 1);
      io::println("map: " + Str(m.get("a1")) + Str(m.containsKey("a1")) + Str(m.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "free: !first1\nmutual: !first1\nself: !first1\n"
                      "inout: first1!new222\nvirtual: !first1\n"
                      "struct: first1!other9\nnumber: first122\n"
                      "map: 1True1\n");
  g.expectNoLeaks("AReleaseInACalleesCalleeStillHoldsTheRead");
}

// The counterexamples of the analysis that lets a call keep reads it cannot
// replace in place (docs/pir.md §7): a call that replaces a field through a
// narrowed `inout` arm (`d: inout Derived<int>` on a `Base` field) frees
// the object a read of the field holds, though the object's class is exact
// and holds no `Base`; one given only a Wrap replaces the field an `inout`
// parameter names (its slot is of the parameter's superclass); a call
// given a parameter the function may hand on replaces a field of its
// object.  Each method runs on, and each string is the one of, the object
// that was there.
TEST(Ownership, AReadACallReplacesThroughANarrowedPlaceLivesThroughIt) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Base {
      n: int;
      view fn m(x: int) -> int { return self.n + x; }
    }
    impl Base { fn __init__(n: int) { self.n = n; } }
    class Derived<X> : Base { v: X; }
    impl Derived<X> {
      fn __init__(n: int, v: X) { __super__(n); self.v = v; }
    }
    class Plain : Base {
      tag: Str;
      view fn m(x: int) -> int { return self.n + x + self.tag.len(); }
    }
    impl Plain {
      fn __init__(n: int, t: Str) { __super__(n); self.tag = t; }
    }
    class Holder { b: Base; weak w: Base?; }
    impl Holder {
      fn __init__() { self.b = Derived<int>(100, 9); self.w = self.b; }
      fn run() -> int { return self.b.m(reset(self)); }
      fn state() -> Str {
        match self.w {
          Base { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    fn reset(h: Holder) -> int {
      match h.b {
        d: inout Derived<int> { d = Derived<int>(1, 9); }
        _ { }
      }
      return 7;
    }
    class Wrap { h: Holder; }
    fn clobber(w: Wrap) -> int {
      w.h.b = Base(100);
      return 5;
    }
    fn use(x: inout Plain, w: Wrap) -> int { return x.m(clobber(w)); }
    class Node { name: Str; }
    class Pair { node: Node; }
    fn replace(p: Pair) -> Str {
      p.node = Node("o" + Str(2));
      return "!";
    }
    fn local(p: Pair) -> Str { return p.node.name + replace(p); }
    fn main() -> int {
      h = Holder();
      r = h.run();
      io::println("arm: " + Str(r) + " " + h.state());
      h2 = Holder();
      h2.b = Plain(1, "abc");
      w = Wrap(h2);
      match h2.b {
        s: inout Plain {
          s = Plain(2, "fresh");
          io::println("inout: " + Str(use(s, w)));
        }
        _ { }
      }
      io::println(local(Pair(Node("n" + Str(1)))));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "arm: 107 freed\ninout: 12\nn1!\n");
  g.expectNoLeaks("AReadACallReplacesThroughANarrowedPlaceLivesThroughIt");
}

// The standard library's containers read their state in place across the
// calls that let go only of their elements and nodes: growing a map past
// its rehash, replacing and removing keys, clearing it, and stacks, queues
// and heaps of strings run as before, with every value released.
TEST(Ownership, AContainersStateNeedsNoHoldAcrossItsOwnMethods) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as c;
    fn main() -> int {
      m = c::HashMap<Str, int>();
      i = 0;
      while i < 40 {
        m.put("k" + Str(i), i);
        i = i + 1;
      }
      m.put("k3", 300);
      m.remove("k4");
      io::println(Str(m.get("k3")) + " " + Str(m.len()) + " " +
                  Str(m.keys().len()));
      m.clear();
      s = c::Stack<Str>();
      q = c::Queue<Str>();
      p = c::MinPriorityQueue<Str>();
      for j in 0..5 {
        s.push("s" + Str(j));
        q.enqueue("q" + Str(j));
        p.push("p" + Str(j), 5 - j);
      }
      io::println(s.pop() + q.dequeue() + p.pop() + " " + Str(m.len()) +
                  Str(s.len()) + Str(q.len()) + Str(p.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "300 39 39\ns4q0p4 0444\n");
  g.expectNoLeaks("AContainersStateNeedsNoHoldAcrossItsOwnMethods");
}

// A read of `self`'s storage a call cannot let go of is read by a stable
// load, which keeps the hand-off its hold made: with the moves at last use
// made, `v` is handed on to `offer`, and the Pet dies when `offer` returns,
// before `push` asks, as it did while `self.sink` was held across the call
// (docs/pir.md §7, §12).
TEST(Ownership, AStableReadKeepsTheHandOffsTiming) {
  LeakGuard g;
  auto r = compileMoveAndRun(R"(import ::io;
    class Pet<X> { name: Str; }
    class Probe { weak p: Pet<int>?; }
    impl Probe {
      view fn state() -> Str {
        match self.p {
          Pet<int> { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    class Sink<T> { last: Str; }
    impl Sink<T> {
      fn offer(v: T) { self.last = "seen"; }
    }
    struct Pile { sink: Sink<Pet<int>>; }
    impl Pile {
      fn push(v: Pet<int>, probe: Probe) -> Str {
        self.sink.offer(v);
        return probe.state();
      }
    }
    fn main() -> int {
      p = Pile(Sink<Pet<int>>("none"));
      x = Pet<int>("rex");
      probe = Probe(x);
      io::println(p.push(x, probe) + " " + probe.state());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "freed freed\n");
  g.expectNoLeaks("AStableReadKeepsTheHandOffsTiming");
}

// The main module of a program knows every override of its classes'
// methods (docs/pir.md §7): `self.item.name` needs no hold across a call
// whose overrides let go of nothing (`Hush.run`, the pure `Shape.area`'s
// `Square` one), and keeps one across `Shout.run`, which replaces the Item
// it is in: the string read is the one of the Item that was there, which
// dies when the call replaces it.  A map of Dogs, whose death lets go of a
// string only, reads its table in place across put and remove, and every
// replaced Dog dies when it is let go of.
TEST(Ownership, AProgramsVirtualCallsHoldOnlyWhatTheirOverridesMayLetGo) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as c;
    class Item { name: Str; }
    class Probe { weak it: Item?; }
    impl Probe {
      view fn state() -> Str {
        match self.it {
          Item { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    class Quiet { fn run(h: Holder) -> int { return 1; } }
    class Hush : Quiet { fn run(h: Holder) -> int { return 2; } }
    class Loud { fn run(h: Holder) -> int; }
    class Shout : Loud {
      fn run(h: Holder) -> int {
        h.item = Item("new" + Str(1));
        return 3;
      }
    }
    class Holder {
      item: Item;
      quiet: Quiet;
      loud: Loud;
      fn viaQuiet() -> Str { return self.item.name + Str(self.quiet.run(self)); }
      fn viaLoud() -> Str { return self.item.name + Str(self.loud.run(self)); }
    }
    class Shape { view fn area() -> int; }
    class Square : Shape {
      s: int;
      view fn area() -> int { return self.s * self.s; }
    }
    fn total(shapes: view Shape[]) -> int {
      t = 0;
      for i in 0..shapes.len() { t += shapes[i].area(); }
      return t;
    }
    class Dog { name: Str; }
    class Watch { weak d: Dog?; }
    impl Watch {
      view fn state() -> Str {
        match self.d {
          Dog { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    fn main() -> int {
      h = Holder(Item("old" + Str(0)), Hush(), Shout());
      p = Probe(h.item);
      io::println(h.viaQuiet() + " " + p.state());
      io::println(h.viaLoud() + " " + p.state() + " " + h.item.name);
      shapes: Shape[2] = [Square(2), Square(3)];
      io::println(Str(total(shapes)));
      m = c::HashMap<Str, Dog>();
      rex = Dog("rex" + Str(1));
      w = Watch(rex);
      m.put("d", rex);
      rex = Dog("max");
      io::println(w.state());
      m.put("d", rex);
      io::println(w.state() + " " + m.get("d").name);
      m.remove("d");
      io::println(Str(m.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut,
            "old02 alive\nold03 freed new1\n13\nalive\nfreed max\n0\n");
  g.expectNoLeaks("AProgramsVirtualCallsHoldOnlyWhatTheirOverridesMayLetGo");
}

// A read of `self`'s storage across a virtual call of a program's class
// whose overrides let go of nothing of its class is read by a stable load,
// which keeps the hand-off its hold made: `v` is handed on to `offer`, and
// the Pet dies when `offer` returns, before `push` asks (docs/pir.md §7,
// §12).
TEST(Ownership, AProgramsStableReadKeepsTheHandOffsTiming) {
  LeakGuard g;
  auto r = compileMoveAndRun(R"(import ::io;
    class Pet { name: Str; }
    class Probe { weak p: Pet?; }
    impl Probe {
      view fn state() -> Str {
        match self.p {
          Pet { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    class Sink { last: Str; fn offer(v: Pet) { self.last = "seen"; } }
    class Loud : Sink { fn offer(v: Pet) { self.last = v.name; } }
    struct Pile { sink: Sink; }
    impl Pile {
      fn push(v: Pet, probe: Probe) -> Str {
        self.sink.offer(v);
        return probe.state();
      }
    }
    fn main() -> int {
      p = Pile(Loud("none"));
      x = Pet("rex" + Str(1));
      probe = Probe(x);
      io::println(p.push(x, probe) + " " + probe.state() + " " + p.sink.last);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "freed freed rex1\n");
  g.expectNoLeaks("AProgramsStableReadKeepsTheHandOffsTiming");
}

// A field no code stores into once its object is made keeps the reference
// its object's `__init__` gave it (docs/pir.md §7): `self.fixed.name` is
// read in place across a closure that replaces `moving` and, through an
// `inout` argument, `byArg`.  Their reads keep a reference of their own:
// each is the string of the Item that was there, which dies once the
// statement has used it.
TEST(Ownership, AFieldStoredOnlyAtConstructionIsReadInPlace) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Item { name: Str; }
    class Probe { weak it: Item?; }
    impl Probe {
      view fn state() -> Str {
        match self.it {
          Item { return "alive"; }
          None { return "freed"; }
        }
      }
    }
    fn bump(s: inout Item) { s = Item("arg" + Str(3)); }
    class Holder {
      fixed: Item;
      moving: Item;
      byArg: Item;
      view fn viaFixed(f: () -> int) -> Str { return self.fixed.name + Str(f()); }
      view fn viaMoving(f: () -> int) -> Str { return self.moving.name + Str(f()); }
      view fn viaArg(f: () -> int) -> Str { return self.byArg.name + Str(f()); }
    }
    impl Holder {
      fn __init__() {
        self.fixed = Item("fix" + Str(0));
        self.moving = Item("old" + Str(1));
        self.byArg = Item("arg" + Str(2));
      }
    }
    fn main() -> int {
      h = Holder();
      fixed = Probe(h.fixed);
      moving = Probe(h.moving);
      f = fn() -> int {
        h.moving = Item("new" + Str(4));
        bump(h.byArg);
        return 7;
      };
      io::println(h.viaFixed(f) + " " + fixed.state() + " " + moving.state());
      moving = Probe(h.moving);
      io::println(h.viaMoving(f) + " " + moving.state() + " " + h.moving.name);
      arg = Probe(h.byArg);
      io::println(h.viaArg(f) + " " + arg.state() + " " + h.byArg.name);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut,
            "fix07 alive freed\nnew47 freed new4\narg37 freed arg3\n");
  g.expectNoLeaks("AFieldStoredOnlyAtConstructionIsReadInPlace");
}

// An `__init__`'s store into another object of its class is no
// construction: `self.f` keeps a reference of its own across `remake`,
// whose `__init__` replaces the `f` of the object it is given, and `tag`
// runs on the Item that was there, which dies once the statement has used
// it.  (A binding cannot be named `self` there: Sema rejects it.)
TEST(Ownership, AnInitsStoreIntoAnotherObjectIsNoConstruction) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Item { s: Str; }
    impl Item {
      fn __init__(s: Str) { self.s = s; }
      view fn tag(x: int) -> Str { return self.s + Str(x); }
    }
    class A { f: Item; }
    impl A {
      fn __init__(other: A?) {
        self.f = Item("original" + Str(100));
        match other {
          o: A { o.f = Item("replaced" + Str(2)); }
          None { }
        }
      }
      fn run() -> Str { return self.f.tag(remake(self)); }
    }
    fn remake(a: A) -> int {
      b = A(a);
      return 1;
    }
    fn main() -> int {
      a = A(None);
      io::println(a.run());
      io::println(a.f.s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "original1001\nreplaced2\n");
  g.expectNoLeaks("AnInitsStoreIntoAnotherObjectIsNoConstruction");
}

// A string's character is read from the string as it was before its index
// was computed, as an array's element is, though the call computing it
// replaces the string (and makes a new one where it was).
TEST(Ownership, AStringIsReadBeforeItsIndex) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B {
      s: Str;
      a: Str[2];
    }
    class H { b: B; }
    fn g(h: H) -> int {
      h.b = B("XY" + Str(9), ["X" + Str(9), "Y"]);
      y = "QQ" + Str(8);
      return 0;
    }
    fn bump(s: inout Str) -> int {
      s = "XY" + Str(9);
      y = "QQ" + Str(8);
      return 0;
    }
    fn main() -> int {
      h = H(B("ab" + Str(1), ["a" + Str(1), "b"]));
      c1 = h.b.a[g(h)];
      h.b = B("ab" + Str(1), ["a" + Str(1), "b"]);
      c2 = h.b.s[g(h)];
      t = "ab" + Str(1);
      c3 = t[bump(t)];
      io::println(c1 + " " + Str(c2) + " " + Str(c3) + " " + h.b.s + " " + t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "a1 a a XY9 XY9\n");
  g.expectNoLeaks("AStringIsReadBeforeItsIndex");
}

// -- One reference count per object, at the runtime level

// Boxing the same object twice gives the same pointer (the object's own) with
// a bumped strong count -- what makes every raw-alias boxing site safe.
// A parameter lent to its callee (#98 part 2, docs/pir.md §12) is the
// caller's: the callee releases nothing of it, retains what it keeps, and
// hands it on with its bit.  Kept in a field, it lives as long as the field
// after the caller let go of it; handed on through a method to the field of
// another object, the same; handed over at the caller's last use, the
// object dies with its last keeper.  A read the callee's call replaces
// (`self.a.b.name` while `g` assigns `x.a`) stays alive for the read.
TEST(Ownership, ALentParameterTheCalleeKeepsLivesAsLongAsItsKeeper) {
  LeakGuard g;
  paykan::pir::Statistics stats;
  auto r = compilePassesAndRun(R"(import ::io;
    class Node { name: Str; }
    class Probe { weak p: Node?; }
    impl Probe {
      view fn state() -> Str {
        match self.p {
          Node { return "alive"; }
          None { return "gone"; }
        }
      }
    }
    class Holder { n: Node?; }
    impl Holder {
      fn take(n: Node) { self.n = n; }
      view fn relay(n: Node, other: Holder) { other.take(n); }
    }
    class B { name: Str; }
    class A { b: B; }
    class X {
      a: A;
      fn m() -> Str { return self.a.b.name + g(self); }
    }
    fn g(x: X) -> Str {
      x.a = A(B("new"));
      return "!";
    }
    fn main() -> int {
      h = Holder(None);
      k = Holder(None);
      n = Node("n");
      probe = Probe(n);
      h.take(n);
      h.relay(n, k);
      io::println(n.name + " " + probe.state());
      n = Node("m");
      io::println("dropped: " + probe.state());
      h.n = None;
      io::println("h let go: " + probe.state());
      k.n = None;
      io::println("k let go: " + probe.state());
      last = Node("last");
      lastProbe = Probe(last);
      h.take(last);
      h.n = None;
      io::println("handed over: " + lastProbe.state() + " " + n.name);
      x = X(A(B("old" + "name")));
      io::println(x.m() + " now " + x.a.b.name);
      return 0;
    }
  )",
                               stats);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.ExitCode, 0);
  EXPECT_EQ(r.StdOut, "n alive\n"
                      "dropped: alive\n"
                      "h let go: alive\n"
                      "k let go: gone\n"
                      "handed over: gone m\n"
                      "oldname! now new\n");
  EXPECT_GT(stats.get("lending", "parameters handed on"), 0u);
  EXPECT_GT(stats.get("lending", "retains at keeps"), 0u);
  g.expectNoLeaks("ALentParameterTheCalleeKeepsLivesAsLongAsItsKeeper");
}

TEST(Ownership, SharedNewRetainsTheObject) {
  PaykanObject *obj = PaykanObject_new();
  PaykanShared *first = PaykanShared_new(obj);
  PaykanShared *second = PaykanShared_new(obj);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, obj);
  EXPECT_EQ(obj->strong, 2);
  Paykan_release(second);
  EXPECT_EQ(obj->strong, 1);
  Paykan_release(first); // destroys and frees obj
}

// The immortal None singleton: its strong count starts at 1 and nothing
// releases that reference, so boxings and releases never destroy it.
TEST(Ownership, NoneSingletonStaysAlive) {
  ASSERT_EQ(PaykanObject_None.strong, 1);
  PaykanShared *s = PaykanShared_new(&PaykanObject_None);
  EXPECT_EQ(s, &PaykanObject_None);
  EXPECT_EQ(PaykanObject_None.strong, 2);
  Paykan_release(s);
  EXPECT_EQ(PaykanObject_None.strong, 1);
}
