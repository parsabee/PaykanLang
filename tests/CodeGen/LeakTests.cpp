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

// ============================================================================
// String literal (samples/leak-check/01)
// ============================================================================

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

// ============================================================================
// String concatenation (samples/leak-check/02)
// ============================================================================

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

// ============================================================================
// Str<int> conversion (samples/leak-check/03)
// ============================================================================

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

// ============================================================================
// int[] array literal (samples/leak-check/04)
// ============================================================================

TEST(Leak, IntArrayLiteral) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      a: int[] = [1, 2, 3];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("IntArrayLiteral");
}

// ============================================================================
// Str[] array with push (samples/leak-check/05)
// ============================================================================

TEST(Leak, StrArrayPush) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      arr: Str[] = [];
      arr.push("one");
      arr.push("two");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("StrArrayPush");
}

// ============================================================================
// Object array (samples/leak-check/06)
// ============================================================================

TEST(Leak, ObjArray) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      arr: Obj[] = [];
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("ObjArray");
}

// ============================================================================
// Fieldless class instance (samples/leak-check/07)
// ============================================================================

TEST(Leak, ClassEmpty) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box {
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

// ============================================================================
// Class with Str field (samples/leak-check/08)
// ============================================================================

TEST(Leak, ClassStrField) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Person {
      name: Str;
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

// ============================================================================
// Class inheritance (samples/leak-check/09)
// ============================================================================

TEST(Leak, ClassInherit) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Animal {
      name: Str;
      fn __init__(n: Str) { self.name = n; }
    }
    class Dog : Animal {
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

// ============================================================================
// String assignment rebind (ensures no double-wrap leak — regression for #22)
// ============================================================================

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

// ============================================================================
// String returned from function
// ============================================================================

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

// ============================================================================
// Early return from function with string locals
// ============================================================================

TEST(Leak, EarlyReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn maybe(flag: bool) -> int {
      s: Str = "allocated";
      if (flag) { return 1; }
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

// ============================================================================
// mov: ownership transfer must neither leak nor double-free
// ============================================================================

TEST(Leak, MovStringTransfer) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "Hello world";
      t = mov s;
      println(t);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MovStringTransfer");
}

TEST(Leak, MovClassObject) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box { v: Str;
      fn __init__(s: Str) { self.v = s; }
    }
    fn main() -> int {
      b: Box = Box("payload");
      c = mov b;
      println(c.v);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MovClassObject");
}

TEST(Leak, MovThenRevive) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "first";
      t = mov s;
      s = "second";
      println(t);
      println(s);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MovThenRevive");
}

TEST(Leak, MovConditional) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "cond";
      if (True) {
        t = mov s;
        println(t);
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MovConditional");
}

TEST(Leak, MovIntoReturn) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn passthrough(s: Str) -> Str { return mov s; }
    fn main() -> int {
      out: Str = passthrough("relayed");
      println(out);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MovIntoReturn");
}

// ============================================================================
// Method call on a freshly-owned temporary receiver must not leak the receiver
// (regression: the receiver teardown was missing; `mov` surfaced it).
// ============================================================================

TEST(Leak, MethodCallOnCallResultReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn make() -> Str { return "xyz"; }
    fn main() -> int {
      n: int = make().len();
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnCallResultReceiver");
}

TEST(Leak, MethodCallOnConcatReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      n: int = ("a" + "b").len();
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnConcatReceiver");
}

TEST(Leak, MethodCallOnMovedReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      s: Str = "receiver";
      n: int = (mov s).len();
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnMovedReceiver");
}

TEST(Leak, MethodCallOnStringLiteralReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn main() -> int {
      n: int = "abc".len();
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MethodCallOnStringLiteralReceiver");
}

// ============================================================================
// A ref-typed call temporary used as an equality operand (not bound to a
// variable) must be destroyed at the comparison, not leaked.
// ============================================================================

TEST(Leak, CallTemporaryInEquality) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Thing { n: int;
      fn __init__(x: int) { self.n = x; }
    }
    fn process() -> Obj { return Thing(5); }
    fn main() -> int {
      if (process() == None) { println("none"); }
      else { println("something"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallTemporaryInEquality");
}

TEST(Leak, TwoCallTemporariesInEquality) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Thing {}
    fn make() -> Obj { return Thing(); }
    fn main() -> int {
      if (make() == make()) { println("same"); } else { println("diff"); }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("TwoCallTemporariesInEquality");
}

// ============================================================================
// Member chains rooted in a call (samples/leak-check/16): the member access
// owns the fresh receiver box (`makeH()` below) and must tear it down after
// reading the field — retaining a ref-typed field value first so it survives
// the receiver teardown.
// ============================================================================

TEST(Leak, CallRootedMemberChainAsValue) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Str;
      fn __init__() { self.a = "hello"; }
    }
    fn makeH() -> H { return H(); }
    fn main() -> int {
      x = makeH().a;
      println(x);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedMemberChainAsValue");
}

TEST(Leak, CallRootedMemberChainAsReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Str;
      fn __init__() { self.a = "hello"; }
    }
    fn makeH() -> H { return H(); }
    fn main() -> int {
      n: int = makeH().a.len();
      println(Str<int>(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedMemberChainAsReceiver");
}

TEST(Leak, CallRootedNestedMemberChain) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Str;
      fn __init__() { self.a = "deep"; }
    }
    class O { h: H;
      fn __init__() { self.h = H(); }
    }
    fn makeO() -> O { return O(); }
    fn main() -> int {
      x = makeO().h.a;
      println(x);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedNestedMemberChain");
}

// ============================================================================
// Acquiring a BORROWED field box for a new owner must retain it, not steal
// it: the field slot keeps its own reference, and stealing used to release
// the same box one time too many (heap corruption).
// ============================================================================

TEST(Leak, MemberReadIntoImplicitVarRetains) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Str;
      fn __init__() { self.a = "hello"; }
    }
    fn main() -> int {
      h: H = H();
      x = h.a;
      println(x);
      println(h.a);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MemberReadIntoImplicitVarRetains");
}

TEST(Leak, MemberReadRebindAndFieldToField) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Str;
      fn __init__(s: Str) { self.a = s; }
    }
    fn main() -> int {
      h: H = H("one");
      h2: H = H("two");
      x: Str = "seed";
      x = h.a;      // rebind an owned var to a borrowed field box
      h2.a = h.a;   // field-to-field assignment (borrowed RHS)
      println(x);
      println(h2.a);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MemberReadRebindAndFieldToField");
}

// ============================================================================
// A match subject that is a borrowed field box must be retained for the
// match's duration (releasing the field slot's own reference at match.end
// freed the field under the object), and a call-rooted subscript receiver's
// fresh box must be torn down once the element is copied out.
// ============================================================================

TEST(Leak, MatchOnBorrowedFieldBox) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class H { a: Obj;
      fn __init__() { self.a = "hello"; }
    }
    fn main() -> int {
      h: H = H();
      match h.a {
        Str { println("is str"); }
        _ { println("other"); }
      }
      println("done");
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("MatchOnBorrowedFieldBox");
}

TEST(Leak, CallRootedSubscriptReceiver) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn makeArr() -> int[] { return [1, 2, 3]; }
    fn makeStr() -> Str { return "a" + "b"; }
    fn main() -> int {
      x: int = makeArr()[0];
      c: char = makeStr()[1];
      println(Str<int>(x));
      println(Str<char>(c));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("CallRootedSubscriptReceiver");
}

// ============================================================================
// Array-typed ternary: both branches must yield a boxed PaykanShared* (the
// ternary is classified as a fresh +1 box), on the taken and not-taken paths,
// for primitive and object element arrays, with no leaks.
// ============================================================================

TEST(Leak, TernaryArrayBranches) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn pick(c: bool) -> int {
      a: int[] = [1, 2, 3];
      b: int[] = [4, 5];
      xs: int[] = if c then a else b;
      xs.push(9);
      return xs.len();
    }
    fn names(c: bool) -> Str {
      s: Str[] = ["a"];
      t: Str[] = ["b", "c"];
      zs: Str[] = if c then s else t;
      zs = if c then t else s;   // rebind through the assignment path
      return zs[0];
    }
    fn main() -> int {
      println(Str<int>(pick(True)));    // a: 3 + 1 pushed
      println(Str<int>(pick(False)));   // b: 2 + 1 pushed
      println(names(True));           // t[0]
      println(names(False));          // s[0]
      lit: int[] = if False then [1] else [2, 3];
      println(Str<int>(lit.len()));
      println(Str<int>((if True then lit else [7]).len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "4\n3\nb\na\n2\n2\n");
  g.expectNoLeaks("TernaryArrayBranches");
}

// ============================================================================
// Empty array literal at every typed sink (samples/leak-check/19).  Sema types
// `[]` as ArrayType(void) and must adopt the destination's element type at
// EVERY sink, not just declarations; otherwise CodeGen emits a primitive
// array whose destructor never releases the objects pushed into it.
// ============================================================================

TEST(Leak, EmptyArrayLiteralSinks) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      items: Str[];
      grid: Str[][];
      fn __init__(seed: Str[]) {
        self.items = [];          // field store in the constructor
        self.grid = [[], []];     // nested empty literals
        self.items.push("ctor");
        self.grid[0].push("g0");
        self.grid[1] = [];        // subscript store
        self.grid[1].push("g1");
        seed.push("seed");
      }
      fn reset() { self.items = []; self.items.push("reset"); }
      fn take(xs: Str[]) -> int { xs.push("arg"); return xs.len(); }
    }
    fn fresh() -> Str[] { return []; }
    fn fill(xs: Str[]) -> int { xs.push("fn"); return xs.len(); }
    fn main() -> int {
      b: Bag = Bag([]);                      // constructor argument
      println(b.items[0] + b.grid[0][0] + b.grid[1][0]);
      b.reset();                             // field store in a method
      println(b.items[0]);
      b.items = [];                          // field store from outside
      b.items.push("outside");
      println(b.items[0]);
      println(Str<int>(fill([])));             // free-function argument
      println(Str<int>(b.take([])));           // method argument
      f: Str[] = fresh();                    // returned literal
      f.push("ret");
      println(f[0]);
      rows: Str[][] = [];
      rows.push([]);                         // push argument
      rows[0].push("pushed");
      println(rows[0][0]);
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ctorg0g1\nreset\noutside\n1\n1\nret\npushed\n");
  g.expectNoLeaks("EmptyArrayLiteralSinks");
}

// ============================================================================
// Array / tuple arguments to user-defined METHODS.  The method prologue owns
// every reference-typed parameter and releases it on exit, so the call site
// must pass a +1 box for arrays and tuples exactly as it does for classes.
// Before, they took the borrowed-temporary path and were released twice.
// ============================================================================

TEST(Leak, RefTypedArgsToUserMethods) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Bag {
      xs: int[];
      fn __init__() { self.xs = [1, 2]; }
      fn sum(ys: int[]) -> int { return ys[0] + ys[1]; }
      fn names(ns: Str[]) -> int { ns.push("m"); return ns.len(); }
      fn first(t: (int, Str)) -> Str { return t.1; }
      fn own() -> int { return self.sum(self.xs); }     // field argument
    }
    fn mk() -> int[] { return [10, 20]; }
    fn main() -> int {
      b: Bag = Bag();
      v: int[] = [3, 4];
      println(Str<int>(b.sum(v)));                         // local argument
      println(Str<int>(v.len()));                          // local still alive
      println(Str<int>(b.sum([5, 6])));                    // literal argument
      println(Str<int>(b.sum(mk())));                      // call-result argument
      println(Str<int>(b.own()));
      println(Str<int>(b.names([])));                      // empty literal
      println(Str<int>(b.names(["a", "b"])));
      println(b.first((1, "t")));                        // tuple literal
      t: (int, Str) = (2, "u");
      println(b.first(t));                               // tuple local
      println(b.first(t));                               // still alive
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "7\n2\n11\n30\n3\n1\n3\nt\nu\nu\n");
  g.expectNoLeaks("RefTypedArgsToUserMethods");
}
