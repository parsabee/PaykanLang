// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Heap leak tests: verify every allocation site releases to zero live blocks.
// Uses the tracking allocator (Paykan_heap_set_tracking /
// Paykan_heap_live_blocks).

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

// -- String literal (samples/leak-check/01)

TEST(Leak, StringLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "hello";
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringLiteral");
}

// -- String concatenation (samples/leak-check/02)

TEST(Leak, StringConcat) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: Str = "foo";
      b: Str = "bar";
      c: Str = a + b;
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringConcat");
}

// -- Str<int> conversion (samples/leak-check/03)

TEST(Leak, StrOfInt) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      n: int = 42;
      s: Str = Str<int>(n);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StrOfInt");
}

// -- int[n] array literal (samples/leak-check/04)

TEST(Leak, IntArrayLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int[3] = [1, 2, 3];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("IntArrayLiteral");
}

// -- Object array (samples/leak-check/06)

TEST(Leak, ObjArray) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      arr: Obj[1] = ["x"];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ObjArray");
}

// -- Fieldless class instance (samples/leak-check/07)

TEST(Leak, ClassEmpty) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box {}

    impl Box {
      fn __init__() {}
    }
    fn main() -> int {
      b: Box = Box();
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassEmpty");
}

// -- Class with Str field (samples/leak-check/08)

TEST(Leak, ClassStrField) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Person {
      name: Str;
    }

    impl Person {
      fn __init__(n: Str) { self.name = n; }
    }
    fn main() -> int {
      p: Person = Person("Alice");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassStrField");
}

// -- Class inheritance (samples/leak-check/09)

TEST(Leak, ClassInherit) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Animal {
      name: Str;
    }

    impl Animal {
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {}

    impl Dog {
      fn __init__(n: Str) { __super__(n); }
    }
    fn main() -> int {
      d: Dog = Dog("Rex");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassInherit");
}

// -- String assignment rebind: no double-wrap leak (regression for #22)

TEST(Leak, StringRebind) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "first";
      s = "second";
      s = "third";
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringRebind");
}

// -- String returned from function

TEST(Leak, StringReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn greet(name: Str) -> Str {
      return "Hello " + name;
    }
    fn main() -> int {
      s: Str = greet("World");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringReturn");
}

// -- Early return from function with string locals

TEST(Leak, EarlyReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn maybe(flag: bool) -> int {
      s: Str = "allocated";
      if flag { return 1; }
      return 0;
    }
    fn main() -> int {
      maybe(True);
      maybe(False);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("EarlyReturn");
}

// -- Ownership transfer between locals, parameters and returns: no leak, no
// double free

TEST(Leak, StringTransfer) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s: Str = "Hello world";
      t = s;
      io::println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StringTransfer");
}

TEST(Leak, ClassObjectTransfer) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Box { v: Str;
    }

    impl Box {
      fn __init__(s: Str) { self.v = s; }
    }
    fn main() -> int {
      b: Box = Box("payload");
      c = b;
      io::println(c.v);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ClassObjectTransfer");
}

TEST(Leak, TransferThenReassign) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s: Str = "first";
      t = s;
      s = "second";
      io::println(t);
      io::println(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("TransferThenReassign");
}

TEST(Leak, ConditionalTransfer) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s: Str = "cond";
      if True {
        t = s;
        io::println(t);
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ConditionalTransfer");
}

TEST(Leak, ParameterIntoReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn passthrough(s: Str) -> Str { return s; }
    fn main() -> int {
      out: Str = passthrough("relayed");
      io::println(out);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ParameterIntoReturn");
}

// Method call on a freshly-owned temporary receiver must not leak the receiver
// (regression: the receiver teardown was missing).

TEST(Leak, MethodCallOnCallResultReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn make() -> Str { return "xyz"; }
    fn main() -> int {
      n: int = make().len();
      io::println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnCallResultReceiver");
}

TEST(Leak, MethodCallOnConcatReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      n: int = ("a" + "b").len();
      io::println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnConcatReceiver");
}

TEST(Leak, MethodCallOnParenthesizedReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s: Str = "receiver";
      n: int = (s).len();
      io::println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnParenthesizedReceiver");
}

TEST(Leak, MethodCallOnStringLiteralReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      n: int = "abc".len();
      io::println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnStringLiteralReceiver");
}

// A ref-typed call temporary used as an equality operand (not bound to a
// variable) must be destroyed at the comparison, not leaked.

TEST(Leak, CallTemporaryInEquality) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Thing { n: int;
    }

    impl Thing {
      fn __init__(x: int) { self.n = x; }
    }
    fn process() -> Obj { return Thing(5); }
    fn main() -> int {
      if process() == None { io::println("none"); }
      else { io::println("something"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallTemporaryInEquality");
}

TEST(Leak, TwoCallTemporariesInEquality) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Thing {}
    fn make() -> Obj { return Thing(); }
    fn main() -> int {
      if make() == make() { io::println("same"); } else { io::println("diff"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("TwoCallTemporariesInEquality");
}

// Member chains rooted in a call (samples/leak-check/16): the member access
// owns the fresh receiver box (`makeH()` below) and must tear it down after
// reading the field — retaining a ref-typed field value first so it survives
// the receiver teardown.

TEST(Leak, CallRootedMemberChainAsValue) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Str;
    }

    impl H {
      fn __init__() { self.a = "hello"; }
    }
    fn makeH() -> H { return H(); }
    fn main() -> int {
      x = makeH().a;
      io::println(x);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedMemberChainAsValue");
}

TEST(Leak, CallRootedMemberChainAsReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Str;
    }

    impl H {
      fn __init__() { self.a = "hello"; }
    }
    fn makeH() -> H { return H(); }
    fn main() -> int {
      n: int = makeH().a.len();
      io::println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedMemberChainAsReceiver");
}

TEST(Leak, CallRootedNestedMemberChain) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Str;
    }

    impl H {
      fn __init__() { self.a = "deep"; }
    }
    class O { h: H;
    }

    impl O {
      fn __init__() { self.h = H(); }
    }
    fn makeO() -> O { return O(); }
    fn main() -> int {
      x = makeO().h.a;
      io::println(x);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedNestedMemberChain");
}

// Acquiring a BORROWED field box for a new owner must retain it, not steal
// it: the field slot keeps its own reference, and stealing used to release
// the same box one time too many (heap corruption).

TEST(Leak, MemberReadIntoImplicitVarRetains) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Str;
    }

    impl H {
      fn __init__() { self.a = "hello"; }
    }
    fn main() -> int {
      h: H = H();
      x = h.a;
      io::println(x);
      io::println(h.a);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MemberReadIntoImplicitVarRetains");
}

TEST(Leak, MemberReadRebindAndFieldToField) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Str;
    }

    impl H {
      fn __init__(s: Str) { self.a = s; }
    }
    fn main() -> int {
      h: H = H("one");
      h2: H = H("two");
      x: Str = "seed";
      x = h.a;      // rebind an owned var to a borrowed field box
      h2.a = h.a;   // field-to-field assignment (borrowed RHS)
      io::println(x);
      io::println(h2.a);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MemberReadRebindAndFieldToField");
}

// A match subject that is a borrowed field box must be retained for the
// match's duration (releasing the field slot's own reference at match.end
// freed the field under the object), and a call-rooted subscript receiver's
// fresh box must be torn down once the element is copied out.

TEST(Leak, MatchOnBorrowedFieldBox) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class H { a: Obj;
    }

    impl H {
      fn __init__() { self.a = "hello"; }
    }
    fn main() -> int {
      h: H = H();
      match h.a {
        Str { io::println("is str"); }
        _ { io::println("other"); }
      }
      io::println("done");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MatchOnBorrowedFieldBox");
}

TEST(Leak, CallRootedSubscriptReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn makeArr() -> Str[3] { return [Str(1), Str(2), Str(3)]; }
    fn makeStr() -> Str { return "a" + "b"; }
    fn main() -> int {
      x: Str = makeArr()[0];
      c: char = makeStr()[1];
      io::println(x);
      io::println(Str<char>(c));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedSubscriptReceiver");
}

// Array / tuple arguments to user-defined METHODS.  A by-value parameter
// holding references (a tuple, a `Str[2]`) is the method's own copy, whose
// references it releases on exit; a slice parameter borrows the caller's
// array.  Every argument form (local, literal, call result, field) is
// released exactly once.

TEST(Leak, RefTypedArgsToUserMethods) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Bag {
      xs: int[2];
      view fn sum(ys: view int[]) -> int { return ys[0] + ys[1]; }
      fn names(ns: Str[2]) -> int { ns[1] = "m"; return ns.len(); }
      fn first(t: (int, Str)) -> Str { return t.1; }
      fn own() -> int { return self.sum(self.xs); }     // field argument
    }

    impl Bag {
      fn __init__() { self.xs = [1, 2]; }
    }
    fn mk() -> int[2] { return [10, 20]; }
    fn main() -> int {
      b: Bag = Bag();
      v: int[2] = [3, 4];
      io::println(Str<int>(b.sum(v)));                         // local argument
      io::println(Str<int>(v.len()));                          // local still alive
      io::println(Str<int>(b.sum([5, 6])));                    // literal argument
      io::println(Str<int>(b.sum(mk())));                      // call-result argument
      io::println(Str<int>(b.own()));
      io::println(Str<int>(b.names(["a", "b"])));
      io::println(b.first((1, "t")));                        // tuple literal
      t: (int, Str) = (2, "u");
      io::println(b.first(t));                               // tuple local
      io::println(b.first(t));                               // still alive
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n2\n11\n30\n3\n2\nt\nu\nu\n");
  g.expectNoLeaks("RefTypedArgsToUserMethods");
}

// -- for loops: the body's objects go at the end of each iteration and at a
// `continue` or `break`, and the loop's own bookkeeping holds no object.

TEST(Leak, ForLoopBodyLocals) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    import ::io;
    class Dog { name: Str; }
    fn main() -> int {
      kept: Dog? = None;
      for i in 0..10 step 2 {
        d = Dog("dog " + Str(i));
        if i == 2 { continue; }
        if i == 6 { kept = d; break; }
        s = d.name + "!";
        io::println(s);
      }
      for i in 3..=0 step -1 {
        label = "n" + Str(i);
        if i == 1 { break; }
      }
      match kept {
        d: Dog { io::println("kept " + d.name); }
        None { }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog 0!\ndog 4!\nkept dog 6\n");
  g.expectNoLeaks("ForLoopBodyLocals");
}
