// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// CodeGen / E2E tests for optional types `T?` (issue #5).  Every
// runtime test runs under the tracking allocator and asserts zero live heap
// blocks: a `T?` is a possibly-NULL PaykanShared* box, so each path that
// retains / releases / unwraps it must be NULL-safe and balanced.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

extern "C" {
#include "Runtime.h"
}

using namespace paykan::test;

static const char *const kNode = R"pkn(
    class Node {
      v: int;
      next: Node?;
      fn __init__(x: int) { self.v = x; }
    }
    class Leaf : Node {
      fn __init__(x: int) { __super__(x); }
    }
)pkn";

// -- Str? — Some / None through match, println, reassignment

TEST(Optional, StrSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn describe(s: Str?) -> Str {
      match s {
        v: Str { return "some(" + v + ")"; }
        None   { return "none"; }
      }
    }
    fn main() -> int {
      a: Str? = "hello";
      b: Str? = None;
      println(describe(a));
      println(describe(b));
      println(a);
      println(b);
      b = "later";
      println(describe(b));
      b = None;
      println(describe(b));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "some(hello)\nnone\nhello\nNone\nsome(later)\nnone\n");
  g.expectNoLeaks("StrSomeAndNone");
}

TEST(Optional, ClassSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn main() -> int {
      a: Node? = Node(1);
      b: Node? = None;
      match a {
        n: Node { println(Str<int>(n.v)); }
        None    { println("none"); }
      }
      match b {
        n: Node { println(Str<int>(n.v)); }
        None    { println("none"); }
      }
      b = a;                 // share
      a = None;
      match b {
        n: Node { println(Str<int>(n.v)); }
        _       { println("none"); }
      }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\nnone\n1\n");
  g.expectNoLeaks("ClassSomeAndNone");
}

// -- Linked list: optional field, insertion, traversal via match

TEST(Optional, LinkedListInsertAndTraverse) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn push_front(head: Node?, v: int) -> Node {
      n: Node = Node(v);
      n.next = head;
      return n;
    }
    fn sum(head: Node?) -> int {
      total: int = 0;
      cur: Node? = head;
      while (cur != None) {
        match cur {
          n: Node { total = total + n.v; cur = n.next; }
          None    { }
        }
      }
      return total;
    }
    fn find(head: Node?, key: int) -> Node? {
      cur: Node? = head;
      while (cur != None) {
        match cur {
          n: Node {
            if (n.v == key) { return n; }
            cur = n.next;
          }
          None { }
        }
      }
      return None;
    }
    fn main() -> int {
      head: Node? = None;
      head = push_front(head, 3);
      head = push_front(head, 2);
      head = push_front(head, 1);
      println(Str<int>(sum(head)));
      match find(head, 2) {
        n: Node { println("found " + Str<int>(n.v)); }
        None    { println("not found"); }
      }
      match find(head, 9) {
        n: Node { println("found " + Str<int>(n.v)); }
        None    { println("not found"); }
      }
      // Field left implicitly None by __init__.
      match Node(7).next {
        Node { println("has next"); }
        _    { println("tail"); }
      }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6\nfound 2\nnot found\ntail\n");
  g.expectNoLeaks("LinkedListInsertAndTraverse");
}

TEST(Optional, OptionalFieldReleasedByDestructor) {
  // Destroying the head releases the chain; a None field costs nothing.
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn main() -> int {
      a: Node = Node(1);
      b: Node = Node(2);
      a.next = b;
      a.next = None;      // releases b's reference held by the field
      a.next = Node(3);   // field owns the temporary
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  g.expectNoLeaks("OptionalFieldReleasedByDestructor");
}

// -- Equality: x == None / x != None (null check), two optionals

TEST(Optional, CompareAgainstNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn make(c: bool) -> Node? { return if c then Node(1) else None; }
    fn main() -> int {
      a: Str? = "x";
      b: Str? = None;
      println(Str<bool>(a == None));
      println(Str<bool>(a != None));
      println(Str<bool>(b == None));
      println(Str<bool>(None != b));
      println(Str<bool>(make(True) == None));    // call temporary released
      println(Str<bool>(make(False) == None));
      n: Node = Node(5);
      println(Str<bool>(n.next == None));        // field read
      n.next = Node(6);
      println(Str<bool>(n.next != None));
      xs: Node?[] = [];
      xs.push(None);
      println(Str<bool>(xs[0] == None));         // array element
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "False\nTrue\nTrue\nFalse\nFalse\nTrue\nTrue\nTrue\nTrue\n");
  g.expectNoLeaks("CompareAgainstNone");
}

TEST(Optional, CompareTwoOptionals) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    class Pt {
      x: int;
      fn __init__(v: int) { self.x = v; }
      fn equals(other: Obj) -> bool {
        match other {
          p: Pt { return p.x == self.x; }
          _     { return False; }
        }
      }
    }
    fn make(c: bool) -> Str? { return if c then "a" + "b" else None; }
    fn main() -> int {
      a: Str? = "ab";
      b: Str? = "ab";
      c: Str? = None;
      d: Str? = None;
      println(Str<bool>(a == b));      // content equality via Str.equals
      println(Str<bool>(a == c));      // one None
      println(Str<bool>(c == d));      // both None
      println(Str<bool>(a != c));
      println(Str<bool>(c != d));
      println(Str<bool>(make(True) == make(True)));   // temporaries
      println(Str<bool>(make(False) == make(True)));
      p: Pt? = Pt(3);
      q: Pt? = Pt(3);
      n: Pt? = None;
      println(Str<bool>(p == q));      // user equals override honoured
      println(Str<bool>(p == n));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "True\nFalse\nTrue\nTrue\nFalse\nTrue\nFalse\nTrue\nFalse\n");
  g.expectNoLeaks("CompareTwoOptionals");
}

// -- mov of an optional (present and None)

TEST(Optional, MovOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn describe(s: Str?) -> Str {
      match s {
        v: Str { return v; }
        None   { return "none"; }
      }
    }
    fn relay(s: Str?) -> Str? { return mov s; }
    fn main() -> int {
      a: Str? = "moved";
      b = mov a;
      println(describe(b));
      a = "revived";
      println(describe(a));
      n: Str? = None;
      m = mov n;                  // moving None leaves a NULL slot behind
      println(describe(m));
      println(describe(relay(mov b)));
      println(describe(relay(None)));
      c: Str? = mov a;
      println(describe(c));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "moved\nrevived\nnone\nmoved\nnone\nrevived\n");
  g.expectNoLeaks("MovOptional");
}

// -- Optional parameters / returns through free functions and methods

TEST(Optional, ParamAndReturnThroughFunctions) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn wrap(n: Node) -> Node? { return n; }
    fn pass(n: Node?) -> Node? { return n; }
    fn unwrapOr(n: Node?, d: int) -> int {
      match n {
        x: Node { return x.v; }
        None    { return d; }
      }
    }
    fn main() -> int {
      println(Str<int>(unwrapOr(wrap(Node(1)), 0)));
      println(Str<int>(unwrapOr(pass(None), 9)));
      println(Str<int>(unwrapOr(pass(Node(2)), 0)));
      println(Str<int>(unwrapOr(None, 4)));
      n: Node = Node(5);
      println(Str<int>(unwrapOr(n, 0)));    // T widened to T?
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n9\n2\n4\n5\n");
  g.expectNoLeaks("ParamAndReturnThroughFunctions");
}

TEST(Optional, MethodWithOptionalParamAndReturn) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    class List {
      head: Node?;
      fn __init__() {}
      fn link(n: Node?) { self.head = n; }
      fn first() -> Node? { return self.head; }
      fn firstOr(d: int) -> int {
        match self.head {
          n: Node { return n.v; }
          None    { return d; }
        }
      }
    }
    fn main() -> int {
      l: List = List();
      println(Str<int>(l.firstOr(-1)));
      l.link(Node(3));
      println(Str<int>(l.firstOr(-1)));
      match l.first() {
        n: Node { println(Str<int>(n.v)); }
        None    { println("none"); }
      }
      l.link(None);
      println(Str<int>(l.firstOr(-1)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-1\n3\n3\n-1\n");
  g.expectNoLeaks("MethodWithOptionalParamAndReturn");
}

// -- T? -> Obj coercion: an Obj slot never holds a NULL box

TEST(Optional, OptionalIntoObjMaterialisesNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn make(c: bool) -> Node? { return if c then Node(1) else None; }
    fn show(o: Obj) { println(o); }
    fn main() -> int {
      a: Node? = None;
      o: Obj = a;             // declaration
      println(o);
      match o {
        Node { println("node"); }
        _    { println("other"); }
      }
      p: Obj = None;
      p = make(False);        // assignment into a None-initialised Obj
      println(p);
      p = make(True);
      match p {
        n: Node { println(Str<int>(n.v)); }
        _       { println("other"); }
      }
      show(a);                // argument
      show(make(False));
      xs: Obj[] = [];
      xs.push(a);             // array element
      println(xs[0]);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\nother\nNone\n1\nNone\nNone\nNone\n");
  g.expectNoLeaks("OptionalIntoObjMaterialisesNone");
}

// -- Subclass arm on an optional subject; array subject

TEST(Optional, SubclassArmThenInnerArm) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn kind(n: Node?) -> Str {
      match n {
        l: Leaf { return "leaf " + Str<int>(l.v); }
        x: Node { return "node " + Str<int>(x.v); }
        None    { return "none"; }
      }
    }
    fn main() -> int {
      println(kind(Leaf(1)));
      println(kind(Node(2)));
      println(kind(None));
      a: Node? = Leaf(3);
      match a {
        n: Node { println("inner arm takes a subclass instance"); }
        None    { }
      }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "leaf 1\nnode 2\nnone\ninner arm takes a subclass instance\n");
  g.expectNoLeaks("SubclassArmThenInnerArm");
}

TEST(Optional, OptionalArraySubject) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn total(xs: int[]?) -> int {
      match xs {
        a: int[] {
          a.push(10);
          s: int = 0;
          i: int = 0;
          while (i < a.len()) { s = s + a[i]; i = i + 1; }
          return s;
        }
        None { return -1; }
      }
    }
    fn main() -> int {
      println(Str<int>(total([1, 2, 3])));
      println(Str<int>(total(None)));
      ys: Str[]? = [];
      match ys {
        a: Str[] { a.push("z"); println(a[0]); }
        None     { println("none"); }
      }
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "16\n-1\nz\n");
  g.expectNoLeaks("OptionalArraySubject");
}

// -- T?[] — arrays with None slots

TEST(Optional, ArrayOfOptionals) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn show(n: Node?) -> Str {
      match n {
        x: Node { return Str<int>(x.v); }
        None    { return "-"; }
      }
    }
    fn main() -> int {
      xs: Node?[] = [];
      xs.push(None);
      xs.push(Node(1));
      xs.push(Node(2));
      xs[2] = None;           // releases Node(2)
      xs[0] = Node(0);        // fills a None slot
      i: int = 0;
      while (i < xs.len()) { print(show(xs[i]) + " "); i = i + 1; }
      println("");
      p: Node? = xs.pop();    // None
      q: Node? = xs.pop();    // Node(1)
      println(show(p) + show(q));
      e: Node? = xs[0];       // retains the element
      xs[0] = None;
      println(show(e));
      ss: Str?[] = [];
      ss.push("a");
      ss.push(None);
      println(Str<int>(ss.len()));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0 1 - \n-1\n0\n2\n");
  g.expectNoLeaks("ArrayOfOptionals");
}

// -- Ternary producing an optional; control flow out of match arms

TEST(Optional, TernaryToOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn pick(c: bool) -> Str? { return if c then "yes" else None; }
    fn main() -> int {
      a: Str? = pick(True);
      b: Str? = pick(False);
      c: bool = False;
      d: Str? = if c then "x" else None;
      e: Str? = if c then a else b;
      println(Str<bool>(a != None));
      println(Str<bool>(b == None));
      println(Str<bool>(d == None));
      println(Str<bool>(e == None));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nTrue\n");
  g.expectNoLeaks("TernaryToOptional");
}

TEST(Optional, BreakContinueReturnInsideOptionalMatch) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(
    fn firstOver(head: Node?, limit: int) -> int {
      cur: Node? = head;
      while (True) {
        match cur {
          n: Node {
            cur = n.next;
            if (n.v <= limit) { continue; }
            return n.v;
          }
          None { break; }
        }
      }
      return -1;
    }
    fn main() -> int {
      a: Node = Node(1);
      b: Node = Node(5);
      c: Node = Node(9);
      a.next = b;
      b.next = c;
      println(Str<int>(firstOver(a, 4)));
      println(Str<int>(firstOver(a, 20)));
      println(Str<int>(firstOver(None, 0)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5\n-1\n-1\n");
  g.expectNoLeaks("BreakContinueReturnInsideOptionalMatch");
}

// -- The existing Obj-subject idiom keeps working and keeps its semantics

TEST(Optional, ObjSubjectMatchUnchanged) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    fn main() -> int {
      o: Obj = None;
      match o {
        Str { println("str"); }
        _   { println("none"); }
      }
      o = "text";
      match o {
        s: Str { println(s); }
        _      { println("none"); }
      }
      println(Str<bool>(o == None));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "none\ntext\nFalse\n");
  g.expectNoLeaks("ObjSubjectMatchUnchanged");
}

// -- Modules: optional signatures across an import (codegen + link + run)

static std::string writeFile(const std::string &dir, const std::string &relPath,
                             const std::string &content) {
  auto full = std::filesystem::path(dir) / relPath;
  std::filesystem::create_directories(full.parent_path());
  std::ofstream ofs(full);
  ofs << content;
  return full.string();
}

TEST(Optional, ImportedOptionalSignatures) {
  LeakGuard g;
  auto tmp = (paykan::test::tempDir() / "pkn_opt_cg").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "list.pkn", R"pkn(
    class Node {
      v: int;
      next: Node?;
      fn __init__(x: int) { self.v = x; }
      fn tail() -> Node? { return self.next; }
    }
    fn find(head: Node?, key: int) -> Node? {
      cur: Node? = head;
      while (cur != None) {
        match cur {
          n: Node { if (n.v == key) { return n; } cur = n.next; }
          None    { }
        }
      }
      return None;
    }
    fn count(head: Node?) -> int {
      match head {
        n: Node { return 1 + count(n.next); }
        None    { return 0; }
      }
    }
  )pkn");
  auto main = writeFile(tmp, "main.pkn", R"pkn(
import list;
fn main() -> int {
  a: list::Node = list::Node(1);
  b: list::Node = list::Node(2);
  a.next = b;
  println(Str<int>(list::count(a)));
  println(Str<int>(list::count(None)));
  match list::find(a, 2) {
    n: list::Node { println("found " + Str<int>(n.v)); }
    None          { println("not found"); }
  }
  match list::find(None, 2) {
    n: list::Node { println("found " + Str<int>(n.v)); }
    None          { println("not found"); }
  }
  match b.tail() {
    list::Node { println("has tail"); }
    _          { println("no tail"); }
  }
  return 0;
}
)pkn");
  auto r = compileAndRunFile(main);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2\n0\nfound 2\nnot found\nno tail\n");
  std::filesystem::remove_all(tmp);
  g.expectNoLeaks("ImportedOptionalSignatures");
}

// Interaction with tuples (#4): optional elements are REF slots that may hold
// NULL; destructuring, equality, printing and arrays must stay leak-free.

TEST(OptionalTuple, TupleWithOptionalElements) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(
    class Node { v: int; fn __init__(x: int) { self.v = x; } }
    fn pick(n: Node?, k: int) -> (Node?, int) { return (n, k); }
    fn show(t: (Node?, int)) -> Str {
      x, k = t;
      match x {
        n: Node { return "some " + Str<int>(n.v) + "/" + Str<int>(k); }
        None    { return "none/" + Str<int>(k); }
      }
    }
    fn main() -> int {
      a: Node? = Node(7);
      b: Node? = None;
      t: (Node?, int) = pick(a, 1);
      u = pick(b, 2);
      println(show(t));
      println(show(u));
      println(u);
      println(Str<bool>(t == pick(a, 1)));
      println(Str<bool>(u == pick(None, 2)));
      println(Str<bool>(t == u));
      y: Node?, j: int = u;
      println(Str<bool>(y == None));
      ts: (Node?, int)[] = [];
      ts.push(t);
      ts.push(u);
      println(Str<int>(ts.len()));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "some 7/1\nnone/2\n(None, 2)\nTrue\nTrue\nFalse\nTrue\n2\n");
  g.expectNoLeaks("TupleWithOptionalElements");
}

TEST(OptionalTuple, ImportedTupleOfOptionals) {
  LeakGuard g;
  auto tmp = (paykan::test::tempDir() / "pkn_opt_tuple_cg").string();
  std::filesystem::remove_all(tmp);
  writeFile(tmp, "lib.pkn", R"pkn(
    class Node { v: int; fn __init__(x: int) { self.v = x; } }
    fn wrap(xs: (Node?, int)[]?) -> (Node?, int)[]? { return xs; }
    fn mk(n: Node?) -> (Node?, int) { return (n, 5); }
  )pkn");
  auto main = writeFile(tmp, "main.pkn", R"pkn(
import lib;
fn main() -> int {
  a: (lib::Node?, int)[]? = lib::wrap(None);
  println(Str<bool>(a == None));
  x, k = lib::mk(lib::Node(3));
  match x {
    n: lib::Node { println(Str<int>(n.v + k)); }
    None         { println("none"); }
  }
  y, j = lib::mk(None);
  println(Str<bool>(y == None));
  return 0;
}
)pkn");
  auto r = compileAndRunFile(main);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\n8\nTrue\n");
  std::filesystem::remove_all(tmp);
  g.expectNoLeaks("ImportedTupleOfOptionals");
}
