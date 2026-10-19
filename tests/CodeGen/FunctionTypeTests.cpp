// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen / E2E tests for function values (docs/language/15-function-types.md):
// lambdas and their captures (by copy: values copied, references retained),
// named functions as values, calls through variables, fields, elements and
// results, closures stored, returned, passed across modules and matched as
// optionals.  Every runtime test runs under the tracking allocator and
// asserts zero live heap blocks afterwards, so each case checks both the
// observable output and that every closure and every capture is released
// exactly once.

#include "CodeGenTestUtils.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace paykan::test;

namespace {

std::string writeTempFile(const std::filesystem::path &dir,
                          const std::string &relPath,
                          const std::string &content) {
  auto full = dir / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

} // namespace

TEST(FunctionType, LambdasNamedFunctionsAndCalls) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    fn square(x: int) -> int { return x * x; }
    fn apply(f: (int) -> int, x: int) -> int { return f(x); }
    fn twice(f: (int) -> int) -> (int) -> int { return fn(x: int) -> int { return f(f(x)); }; }
    class Button { label: Str; onClick: (Str) -> void; }
    struct Op { name: Str; run: (int, int) -> int; }
    fn main() -> int {
      n = 10;
      s = "s";
      add = fn(x: int) -> int { return x + n; };   // captures n
      g = square;
      h = twice(add);
      k: (int) -> int = fn(x: int) -> int { return x; };
      fs: ((int) -> int)[2] = [square, add];
      b = Button("ok", fn(l: Str) { println(l + s); });
      b.onClick("clicked ");
      o = Op("add", fn(a: int, b: int) -> int { return a + b; });
      v = fn(x: view Str, c: inout int) -> bool { c = c + x.len(); return c > 3; };
      m = 0;
      ok = v("abc", m);
      t: ((int) -> int, Str) = (g, "t");
      println(Str(apply(g, 3)) + " " + Str(h(1)) + " " + Str(fs[1](2)) + " " +
              Str(o.run(1, 2)) + " " + Str(t.0(4)) + " " + Str(m) + Str(ok));
      n = 20;                                      // the capture is a copy
      return add(1) + k(2);
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "clicked s\n9 21 12 3 16 3False\n");
  EXPECT_EQ(r.ExitCode, 13);
  g.expectNoLeaks("LambdasNamedFunctionsAndCalls");
}

TEST(FunctionType, CapturesAreCopiedAndReleased) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { n: int; view fn toString() -> Str { return "N" + Str(self.n); } }
    struct P { x: int; y: int; }
    class Counter {
      n: int;
      fn make() -> () -> int { return fn() -> int { return self.n + 1; }; }
      fn adder() -> (int) -> void { return fn(d: int) { self.n = self.n + d; }; }
    }
    fn makeAdders(base: int) -> ((int) -> int)[2] {
      return [fn(x: int) -> int { return x + base; }, fn(x: int) -> int { return x * base; }];
    }
    fn compose(f: (int) -> int, g: (int) -> int) -> (int) -> int {
      return fn(x: int) -> int { return g(f(x)); };
    }
    fn mapAll(xs: view int[], f: (int) -> int) -> int {
      total = 0;
      i = 0;
      while (i < xs.len()) { x = xs[i]; total = total + f(x); i = i + 1; }
      return total;
    }
    fn describe(n: Node) -> (Str) -> Str {
      p = P(1, 2);
      t = (n, "t");
      xs: int[2] = [3, 4];
      return fn(prefix: Str) -> Str {
        inner = fn() -> int { return p.x + t.1.len() + xs[1]; };
        return prefix + n.toString() + Str(inner()) + t.0.toString();
      };
    }
    fn main() -> int {
      c = Counter(5);
      get = c.make();                  // captures self: the same object
      add = c.adder();
      add(10);
      println(Str(get()) + " " + Str(c.n));
      fs = makeAdders(3);
      println(Str(fs[0](1)) + " " + Str(fs[1](2)));
      h = compose(fs[0], fs[1]);
      println(Str(h(1)) + " " + Str(mapAll([1, 2, 3], h)));
      d = describe(Node(7));           // a struct, a tuple, an array and an object, captured
      println(d(">"));
      opt: (() -> int)? = None;
      opt = get;
      match opt {
        f: () -> int { println("some " + Str(f())); }
        None { println("none"); }
      }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "16 15\n4 6\n12 45\n>N76N7\nsome 16\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("CapturesAreCopiedAndReleased");
}

TEST(FunctionType, ValuesHeldInPlaceCrossTheCall) {
  // Structs, tuples and fixed-size arrays as parameters (by value, `view`
  // and `inout`) and results of function values, and objects by value and
  // `view`: each passed and returned as a function's are.
  LeakGuard g;
  auto r = compileAndRun(R"(
    struct P { x: int; name: Str; }
    class Node { n: int; }
    fn main() -> int {
      byVal = fn(p: P) -> P { return P(p.x + 1, p.name + "!"); };
      byView = fn(p: view P) -> int { return p.x + p.name.len(); };
      byInout = fn(p: inout P) { p.x = p.x * 10; p.name = "changed"; };
      tup = fn(t: (int, Str)) -> (Str, int) { return (t.1, t.0); };
      arr = fn(a: int[2], s: view Str[]) -> int[2] { return [a[1] + s.len(), a[0]]; };
      objs = fn(n: Node, m: view Node) -> Node { return Node(n.n + m.n); };
      p = P(1, "a");
      q = byVal(p);
      byInout(q);
      t = tup((7, "seven"));
      xs = arr([1, 2], ["x", "y", "z"]);
      nd = objs(Node(1), Node(2));
      println(Str(byView(p)) + " " + Str(q.x) + " " + q.name + " " + t.0 + Str(t.1) + " " +
              Str(xs[0]) + Str(xs[1]) + " " + Str(nd.n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2 20 changed seven7 51 3\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("ValuesHeldInPlaceCrossTheCall");
}

TEST(FunctionType, CallsOfValuesWithReferenceAndMemoryResults) {
  // A call through a function value that is not a name (an element, a
  // tuple's field, another call's result) returning a reference, an object
  // or a value held in memory; a named function in a variable without an
  // annotation, called and copied.
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Node { v: int; view fn get() -> int { return self.v; } }
    struct P { x: int; name: Str; }
    fn sq(x: int) -> int { return x * x; }
    fn mkS() -> Str { return "s"; }
    fn mkNode(v: int) -> Node { return Node(v); }
    fn mkP(x: int) -> P { return P(x, "p" + Str(x)); }
    fn mkF() -> (int) -> int { return sq; }
    fn main() -> int {
      fs: (() -> Str)[1] = [mkS];
      a = fs[0]();
      println(fs[0]());
      println(fs[0]() + "!" + a);
      ns: ((int) -> Node)[1] = [mkNode];
      println(Str(ns[0](3).get()) + " " + Str(ns[0](4).v));
      ps: ((int) -> P)[1] = [mkP];
      p = ps[0](7);
      t: ((int) -> Str, int) = (fn(x: int) -> Str { return "t" + Str(x); }, 0);
      println(p.name + " " + Str(p.x) + " " + t.0(8) + " " + Str(mkF()(5)) + " " + ps[0](1).name);
      g = sq;
      h = g;
      g = fn(x: int) -> int { return x + 1; };
      return h(3) + g(3);
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "s\ns!s\n3 4\np7 7 t8 25 p1\n");
  EXPECT_EQ(r.ExitCode, 13);
  g.expectNoLeaks("CallsOfValuesWithReferenceAndMemoryResults");
}

TEST(FunctionType, TheCalleeOutlivesItsArguments) {
  // An argument that rebinds the variable or the field holding the closure
  // being called: the call keeps its own reference to the closure.
  LeakGuard g;
  auto r = compileAndRun(R"(
    class O { cb: (int) -> int; n: int; }
    fn reset(o: O) -> int {
      o.cb = fn(x: int) -> int { return x * 100; };
      o.n = o.n + 1;
      return o.n;
    }
    fn swap(f: inout (int) -> int) -> int {
      f = fn(x: int) -> int { return x - 1; };
      return 10;
    }
    fn main() -> int {
      k = 5;
      o = O(fn(x: int) -> int { return x + k; }, 0);
      a = o.cb(reset(o));              // the closure held before, with 1
      b = o.cb(1);                     // the replacement
      f = fn(x: int) -> int { return x + k; };
      c = f(swap(f));                  // the closure held before, with 10
      d = (f)(swap(f));                // x - 1, with 10
      println(Str(a) + " " + Str(b) + " " + Str(c) + " " + Str(d));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6 100 15 9\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("TheCalleeOutlivesItsArguments");
}

TEST(FunctionType, ClosuresAreReferencesWithIdentity) {
  LeakGuard g;
  auto r = compileAndRun(R"(
    class Box<T> { v: T; view fn get() -> view T { return self.v; } fn set(v: T) { self.v = v; } }
    impl Box<T> { fn __init__(v: T) { self.v = v; } }
    fn main() -> int {
      f = fn(x: int) -> int { return x + 1; };
      g = f;                           // the same closure
      h = fn(x: int) -> int { return x + 1; };
      b = Box(f);
      b.set(h);
      k = b.get() == h;                // a view of a function value: compared, not stored
      fs: ((int) -> int)[2] = [f, h];
      fs[0] = fs[1];
      println(Str(f == g) + " " + Str(f == h) + " " + Str(k) + " " + Str(fs[0] == h) + " " + Str(f != g));
      i = 0;
      total = 0;
      while (i < 3) {
        mk = fn() -> int { return i * 2; };   // a closure per iteration
        total = total + mk();
        i = i + 1;
      }
      return total + fs[0](1);
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True False True True False\n");
  EXPECT_EQ(r.ExitCode, 8);
  g.expectNoLeaks("ClosuresAreReferencesWithIdentity");
}

TEST(FunctionType, AcrossModules) {
  LeakGuard g;
  auto tmpDir = paykan::test::tempDir() / "pkn_function_type_module_test";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);
  writeTempFile(tmpDir, "lib/ops.pkn", R"(
fn square(x: int) -> int { return x * x; }
fn twice(f: (int) -> int) -> (int) -> int { return fn(x: int) -> int { return f(f(x)); }; }
class Button {
  label: Str;
  onClick: (Str) -> Str;
  fn click() -> Str { return self.onClick(self.label); }
}
struct Op { name: Str; run: (int, int) -> int; }
fn makeCounter(start: int) -> () -> int { return fn() -> int { return start + 1; }; }
fn applyAll(xs: view int[], f: (int) -> int) -> int {
  t = 0; i = 0;
  while (i < xs.len()) { x = xs[i]; t = t + f(x); i = i + 1; }
  return t;
}
)");
  auto mainPath = writeTempFile(tmpDir, "main.pkn", R"(
import lib::ops;
fn plus(a: int, b: int) -> int { return a + b; }
fn main() -> int {
  sq = ops::square;                    // an imported function as a value
  t = ops::twice(sq);                  // a closure made in the other module
  b = ops::Button("b", fn(l: Str) -> Str { return l + "!"; });
  c = ops::makeCounter(41);
  o = ops::Op("plus", plus);
  k: (int) -> int = ops::twice(fn(x: int) -> int { return x + 1; });
  println(Str(t(3)) + " " + b.click() + " " + Str(c()) + " " + Str(o.run(2, 3)) + " " + Str(k(0)) + " " + Str(ops::applyAll([1, 2], sq)));
  return 0;
}
)");
  auto r = compileAndRunFile(mainPath);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "81 b! 42 5 2 5\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("AcrossModules");
  std::filesystem::remove_all(tmpDir);
}
