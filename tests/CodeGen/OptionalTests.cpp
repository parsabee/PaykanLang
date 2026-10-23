// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
    }

    impl Node {
      fn __init__(x: int) { self.v = x; }
    }
    class Leaf : Node {}

    impl Leaf {
      fn __init__(x: int) { __super__(x); }
    }
)pkn";

// -- Str? — Some / None through match, println, reassignment

TEST(Optional, StrSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    fn describe(s: Str?) -> Str {
      match s {
        v: Str { return "some(" + v + ")"; }
        None   { return "none"; }
      }
    }
    fn main() -> int {
      a: Str? = "hello";
      b: Str? = None;
      io::println(describe(a));
      io::println(describe(b));
      io::println(a);
      io::println(b);
      b = "later";
      io::println(describe(b));
      b = None;
      io::println(describe(b));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "some(hello)\nnone\nhello\nNone\nsome(later)\nnone\n");
  g.expectNoLeaks("StrSomeAndNone");
}

TEST(Optional, ClassSomeAndNone) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    fn main() -> int {
      a: Node? = Node(1);
      b: Node? = None;
      match a {
        n: Node { io::println(Str<int>(n.v)); }
        None    { io::println("none"); }
      }
      match b {
        n: Node { io::println(Str<int>(n.v)); }
        None    { io::println("none"); }
      }
      b = a;                 // share
      a = None;
      match b {
        n: Node { io::println(Str<int>(n.v)); }
        _       { io::println("none"); }
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
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
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
      io::println(Str<int>(sum(head)));
      match find(head, 2) {
        n: Node { io::println("found " + Str<int>(n.v)); }
        None    { io::println("not found"); }
      }
      match find(head, 9) {
        n: Node { io::println("found " + Str<int>(n.v)); }
        None    { io::println("not found"); }
      }
      // Field left implicitly None by __init__.
      match Node(7).next {
        Node { io::println("has next"); }
        _    { io::println("tail"); }
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
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    fn make(c: bool) -> Node? { return if c then Node(1) else None; }
    fn main() -> int {
      a: Str? = "x";
      b: Str? = None;
      io::println(Str<bool>(a == None));
      io::println(Str<bool>(a != None));
      io::println(Str<bool>(b == None));
      io::println(Str<bool>(None != b));
      io::println(Str<bool>(make(True) == None));    // call temporary released
      io::println(Str<bool>(make(False) == None));
      n: Node = Node(5);
      io::println(Str<bool>(n.next == None));        // field read
      n.next = Node(6);
      io::println(Str<bool>(n.next != None));
      xs: Node?[1] = [None];
      io::println(Str<bool>(xs[0] == None));         // array element
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
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    class Pt {
      x: int;
      view fn equals(other: Obj) -> bool {
        match other {
          p: Pt { return p.x == self.x; }
          _     { return False; }
        }
      }
    }

    impl Pt {
      fn __init__(v: int) { self.x = v; }
    }
    fn make(c: bool) -> Str? { return if c then "a" + "b" else None; }
    fn main() -> int {
      a: Str? = "ab";
      b: Str? = "ab";
      c: Str? = None;
      d: Str? = None;
      io::println(Str<bool>(a == b));      // content equality via Str.equals
      io::println(Str<bool>(a == c));      // one None
      io::println(Str<bool>(c == d));      // both None
      io::println(Str<bool>(a != c));
      io::println(Str<bool>(c != d));
      io::println(Str<bool>(make(True) == make(True)));   // temporaries
      io::println(Str<bool>(make(False) == make(True)));
      p: Pt? = Pt(3);
      q: Pt? = Pt(3);
      n: Pt? = None;
      io::println(Str<bool>(p == q));      // user equals override honoured
      io::println(Str<bool>(p == n));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "True\nFalse\nTrue\nTrue\nFalse\nTrue\nFalse\nTrue\nFalse\n");
  g.expectNoLeaks("CompareTwoOptionals");
}

// -- An optional copied, relayed and reassigned (present and None)

TEST(Optional, OptionalCopiedAndRelayed) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    fn describe(s: Str?) -> Str {
      match s {
        v: Str { return v; }
        None   { return "none"; }
      }
    }
    fn relay(s: Str?) -> Str? { return s; }
    fn main() -> int {
      a: Str? = "moved";
      b = a;
      io::println(describe(b));
      a = "revived";
      io::println(describe(a));
      n: Str? = None;
      m = n;                      // copying None copies a NULL slot
      io::println(describe(m));
      io::println(describe(relay(b)));
      io::println(describe(relay(None)));
      c: Str? = a;
      io::println(describe(c));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "moved\nrevived\nnone\nmoved\nnone\nrevived\n");
  g.expectNoLeaks("OptionalCopiedAndRelayed");
}

// -- Optional parameters / returns through free functions and methods

TEST(Optional, ParamAndReturnThroughFunctions) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    fn wrap(n: Node) -> Node? { return n; }
    fn pass(n: Node?) -> Node? { return n; }
    fn unwrapOr(n: Node?, d: int) -> int {
      match n {
        x: Node { return x.v; }
        None    { return d; }
      }
    }
    fn main() -> int {
      io::println(Str<int>(unwrapOr(wrap(Node(1)), 0)));
      io::println(Str<int>(unwrapOr(pass(None), 9)));
      io::println(Str<int>(unwrapOr(pass(Node(2)), 0)));
      io::println(Str<int>(unwrapOr(None, 4)));
      n: Node = Node(5);
      io::println(Str<int>(unwrapOr(n, 0)));    // T widened to T?
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1\n9\n2\n4\n5\n");
  g.expectNoLeaks("ParamAndReturnThroughFunctions");
}

TEST(Optional, MethodWithOptionalParamAndReturn) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    class List {
      head: Node?;
      fn link(n: Node?) { self.head = n; }
      fn first() -> Node? { return self.head; }
      fn firstOr(d: int) -> int {
        match self.head {
          n: Node { return n.v; }
          None    { return d; }
        }
      }
    }

    impl List {
      fn __init__() {}
    }
    fn main() -> int {
      l: List = List();
      io::println(Str<int>(l.firstOr(-1)));
      l.link(Node(3));
      io::println(Str<int>(l.firstOr(-1)));
      match l.first() {
        n: Node { io::println(Str<int>(n.v)); }
        None    { io::println("none"); }
      }
      l.link(None);
      io::println(Str<int>(l.firstOr(-1)));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "-1\n3\n3\n-1\n");
  g.expectNoLeaks("MethodWithOptionalParamAndReturn");
}

// -- T? -> Obj coercion: an Obj slot never holds a NULL box

// The print builtins print an optional reference, `None` or its value
// (an optional is not an `Obj`, #96): a variable, a call's fresh result, a
// field and an element.
TEST(Optional, PrintingAReferenceOptional) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    class H { s: Str?; }
    impl H { fn __init__() { } }
    fn make(c: bool) -> Str? { return if c then "one" else None; }
    fn main() -> int {
      a: Str? = None;
      io::println(a);
      io::println(make(False));
      io::println(make(True));
      h = H();
      io::print(h.s); io::print(" ");
      h.s = "two";
      io::println(h.s);
      xs: Str?[2] = [None, "three"];
      io::println(xs[0]); io::println(xs[1]);
      io::printerrln(make(True));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\nNone\none\nNone two\nNone\nthree\n");
  EXPECT_NE(r.StdErr.find("one\n"), std::string::npos) << r.StdErr;
  g.expectNoLeaks("PrintingAReferenceOptional");
}

// -- Subclass arm on an optional subject

TEST(Optional, SubclassArmThenInnerArm) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    fn kind(n: Node?) -> Str {
      match n {
        l: Leaf { return "leaf " + Str<int>(l.v); }
        x: Node { return "node " + Str<int>(x.v); }
        None    { return "none"; }
      }
    }
    fn main() -> int {
      io::println(kind(Leaf(1)));
      io::println(kind(Node(2)));
      io::println(kind(None));
      a: Node? = Leaf(3);
      match a {
        n: Node { io::println("inner arm takes a subclass instance"); }
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

// -- T?[n] — arrays with None slots

TEST(Optional, ArrayOfOptionals) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
    fn show(n: Node?) -> Str {
      match n {
        x: Node { return Str<int>(x.v); }
        None    { return "-"; }
      }
    }
    fn main() -> int {
      xs: Node?[3] = [None, None, None];
      xs[1] = Node(1);
      xs[2] = Node(2);
      xs[2] = None;           // releases Node(2)
      xs[0] = Node(0);        // fills a None slot
      i: int = 0;
      while (i < xs.len()) { io::print(show(xs[i]) + " "); i = i + 1; }
      io::println("");
      p: Node? = xs[2];       // None
      q: Node? = xs[1];       // Node(1), taken out of its slot
      xs[1] = None;
      io::println(show(p) + show(q));
      e: Node? = xs[0];       // retains the element
      xs[0] = None;
      io::println(show(e));
      ss: Str?[2] = [None, None];
      ss[0] = "a";
      ss[1] = None;
      io::println(Str<int>(ss.len()));
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
  auto r = compileAndRun(R"pkn(import ::io;
    fn pick(c: bool) -> Str? { return if c then "yes" else None; }
    fn main() -> int {
      a: Str? = pick(True);
      b: Str? = pick(False);
      c: bool = False;
      d: Str? = if c then "x" else None;
      e: Str? = if c then a else b;
      io::println(Str<bool>(a != None));
      io::println(Str<bool>(b == None));
      io::println(Str<bool>(d == None));
      io::println(Str<bool>(e == None));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\nTrue\nTrue\nTrue\n");
  g.expectNoLeaks("TernaryToOptional");
}

TEST(Optional, BreakContinueReturnInsideOptionalMatch) {
  LeakGuard g;
  auto r = compileAndRun(std::string(kNode) + R"pkn(import ::io;
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
      io::println(Str<int>(firstOver(a, 4)));
      io::println(Str<int>(firstOver(a, 20)));
      io::println(Str<int>(firstOver(None, 0)));
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
  auto r = compileAndRun(R"pkn(import ::io;
    fn main() -> int {
      o: Obj = None;
      match o {
        Str { io::println("str"); }
        _   { io::println("none"); }
      }
      o = "text";
      match o {
        s: Str { io::println(s); }
        _      { io::println("none"); }
      }
      io::println(Str<bool>(o == None));
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
      fn tail() -> Node? { return self.next; }
    }

    impl Node {
      fn __init__(x: int) { self.v = x; }
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
  auto main = writeFile(tmp, "main.pkn", R"pkn(import ::io;
import list;
fn main() -> int {
  a: list::Node = list::Node(1);
  b: list::Node = list::Node(2);
  a.next = b;
  io::println(Str<int>(list::count(a)));
  io::println(Str<int>(list::count(None)));
  match list::find(a, 2) {
    n: list::Node { io::println("found " + Str<int>(n.v)); }
    None          { io::println("not found"); }
  }
  match list::find(None, 2) {
    n: list::Node { io::println("found " + Str<int>(n.v)); }
    None          { io::println("not found"); }
  }
  match b.tail() {
    list::Node { io::println("has tail"); }
    _          { io::println("no tail"); }
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

// Interaction with tuples (#4): an optional reference element is a box that
// is NULL for None; destructuring, equality, printing and arrays must stay
// leak-free.

TEST(OptionalTuple, TupleWithOptionalElements) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
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
      io::println(show(t));
      io::println(show(u));
      io::println(u);
      io::println(Str<bool>(t == pick(a, 1)));
      io::println(Str<bool>(u == pick(None, 2)));
      io::println(Str<bool>(t == u));
      y: Node?, j: int = u;
      io::println(Str<bool>(y == None));
      ts: (Node?, int)[2] = [t, u];
      io::println(Str<int>(ts.len()));
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
    class Node { v: int; }
    impl Node { fn __init__(x: int) { self.v = x; } }
    fn mk(n: Node?) -> (Node?, int) { return (n, 5); }
  )pkn");
  auto main = writeFile(tmp, "main.pkn", R"pkn(import ::io;
import lib;
fn main() -> int {
  x, k = lib::mk(lib::Node(3));
  match x {
    n: lib::Node { io::println(Str<int>(n.v + k)); }
    None         { io::println("none"); }
  }
  y, j = lib::mk(None);
  io::println(Str<bool>(y == None));
  return 0;
}
)pkn");
  auto r = compileAndRunFile(main);
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "8\nTrue\n");
  std::filesystem::remove_all(tmp);
  g.expectNoLeaks("ImportedTupleOfOptionals");
}

// Optionals of enums, tuples and structs (docs/language/12-optionals.md): an
// enum's like `int?`, a tuple's or a struct's held in place as a presence
// flag and the value.  Made, compared, matched, printed, and stored in
// fields, arrays, tuples, parameters and results.
TEST(Optional, EnumTupleAndStructOptionals) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
enum Suit { Hearts, Spades }
struct Point { x: int; name: Str; }
class Holder { p: Point?; t: (int, Str)?; s: Suit?; }
fn find(ok: bool) -> Point? { return if ok then Point(1, "a") else None; }
fn pick(ok: bool) -> Suit? {
  if ok { return Suit::Spades; }
  return None;
}
fn describe(p: Point?) -> Str {
  match p {
    q: Point { return "point " + q.name + Str(q.x); }
    None { return "nothing"; }
  }
}
fn main() -> int {
  a: Point? = Point(2, "b");
  b: Point? = None;
  io::println(a);
  io::println(b);
  io::println(describe(a) + " " + describe(b) + " " + describe(find(True)));
  io::println(Str(a == None) + " " + Str(b == None) + " " + Str(a != b));
  c: Point? = Point(2, "b");
  io::println(a == c);
  t: (int, Str)? = (3, "c");
  io::println(t);
  match t { v: (int, Str) { io::println(v.1); } None { } }
  s = pick(True);
  match s { x: Suit { io::println(x == Suit::Spades); } None { io::println("none"); } }
  io::println(pick(False) == None);
  h = Holder(None, None, None);
  io::println(h.p);
  h.p = Point(9, "z");
  h.t = (1, "q");
  io::println(h.p);
  io::println(h.t);
  h.p = None;
  io::println(h.p == None);
  xs: Point?[2] = [Point(1, "m"), None];
  io::println(xs[0]);
  io::println(xs[1]);
  w = (Point(5, "w"), b);
  io::println(w);
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "Point(x: 2, name: b)\n"
                      "None\n"
                      "point b2 nothing point a1\n"
                      "False True True\n"
                      "True\n"
                      "(3, c)\n"
                      "c\n"
                      "True\n"
                      "True\n"
                      "None\n"
                      "Point(x: 9, name: z)\n"
                      "(1, q)\n"
                      "True\n"
                      "Point(x: 1, name: m)\n"
                      "None\n"
                      "(Point(x: 5, name: w), None)\n");
  EXPECT_EQ(r.ExitCode, 0);
  g.expectNoLeaks("EnumTupleAndStructOptionals");
}

// -- Optionals of tuples and structs holding references

// A tuple or a struct holding references passed where its optional is
// expected (an argument, a constructor's field, a generic's `T?`): the
// temporary that wraps it is released as the optional it is, its references
// after the presence flag.
TEST(Optional, StructWithReferencesWrappedAsAnArgument) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    import ::result as res;
    import ::collections;
    class Node { n: int; }
    impl Node { fn __init__(n: int) { self.n = n; } }
    struct Item { name: Str; node: Node; }
    struct Holder { v: Item?; }
    fn take(p: Item?) -> bool { return p != None; }
    fn main() -> int {
      io::println(take(Item("x", Node(1))));
      h = Holder(Item("y", Node(2)));
      match h.v {
        i: Item { io::println(i.name + Str(i.node.n)); }
        None { io::println("none"); }
      }
      r = res::ok((1, "a"));
      io::println(r.isOk());
      match r.value {
        t: (int, Str) { io::println(t); }
        None { io::println("none"); }
      }
      l = collections::List<(int, Str)?>();
      l.push((2, "b"));
      io::println(l.get(0));
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "True\ny2\nTrue\n(1, a)\n(2, b)\n");
  g.expectNoLeaks("StructWithReferencesWrappedAsAnArgument");
}

// None of an optional tuple or struct whose value holds references, in a
// buffer's slot: all zeros is None, not a slot that was never set.
TEST(Optional, NoneOfAStructWithReferencesInABuffer) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    import ::collections;
    import ::memory;
    import ::result as res;
    class Node { n: int; }
    impl Node { fn __init__(n: int) { self.n = n; } }
    struct Item { name: Str; node: Node; }
    fn main() -> int {
      l = collections::List<(int, Str)?>();
      l.push(None);
      l.push((1, "a"));
      io::println(l.get(0));
      io::println(l.get(1));
      b = memory::Buffer<Item?>();
      io::println(res::name(b.push(None).code));
      c = b.copy(0);
      io::println(res::name(c.error.code));
      io::println(c.value == None);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "None\n(1, a)\nOk\nOk\nTrue\n");
  g.expectNoLeaks("NoneOfAStructWithReferencesInABuffer");
}

// An `inout` arm on an optional tuple or struct binds the subject's storage,
// the value after the presence flag: a write through it changes the subject.
TEST(Optional, InoutArmOnAnOptionalStruct) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
    struct P { x: int; s: Str; }
    class Box { p: P?; }
    impl Box { fn __init__() { self.p = P(1, "a"); } }
    fn main() -> int {
      po: P? = P(2, "z");
      match po { p: inout P { p.x = 9; p.s = "w"; } None { } }
      io::println(po);
      t: (int, Str)? = (1, "b");
      match t { u: inout (int, Str) { u = (3, "c"); } None { } }
      io::println(t);
      b = Box();
      match b.p { p: inout P { p = P(7, "q"); } None { } }
      io::println(b.p);
      return 0;
    }
  )pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "P(x: 9, s: w)\n(3, c)\nP(x: 7, s: q)\n");
  g.expectNoLeaks("InoutArmOnAnOptionalStruct");
}

// A nested tuple literal widens into an optional slot of the enclosing
// literal, at any depth, as a call's result does.
TEST(OptionalTuple, NestedLiteralIntoAnOptionalSlot) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
struct P { x: int; y: int; }
fn main() -> int {
  a: (int, (Str, Str)?) = (1, ("x", "y"));
  b: (int, (Str, Str)?) = (2, None);
  c: ((int, int)?, Str) = ((3, 4), "c");
  d: (int, P?) = (5, P(6, 7));
  e: (int, (int, (Str, int)?)?) = (8, (9, ("z", 10)));
  f: ((Str, Str)?, (Str, Str)?)[2] = [(("a", "b"), None), (None, ("c", "d"))];
  match a.1 { v: (Str, Str) { io::println(v.0 + v.1); } None { io::println("none a"); } }
  match b.1 { v: (Str, Str) { io::println(v.0 + v.1); } None { io::println("none b"); } }
  match c.0 { v: (int, int) { io::println(Str(v.0 + v.1)); } None { io::println("none c"); } }
  match d.1 { pt: P { io::println(Str(pt.x * pt.y)); } None { io::println("none d"); } }
  match e.1 {
    v: (int, (Str, int)?) {
      match v.1 { w: (Str, int) { io::println(w.0 + Str(v.0 + w.1)); } None { io::println("none e2"); } }
    }
    None { io::println("none e"); }
  }
  match f[1].1 { v: (Str, Str) { io::println(v.0 + v.1); } None { io::println("none f"); } }
  io::println(a.toString() + " " + b.toString() + " " + e.toString() + " " + d.toString());
  a = (11, ("p", "q"));
  io::println(a.toString());
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut,
            "xy\nnone b\n7\n42\nz19\ncd\n"
            "(1, (x, y)) (2, None) (8, (9, (z, 10))) (5, P(x: 6, y: 7))\n"
            "(11, (p, q))\n");
  g.expectNoLeaks("NestedLiteralIntoAnOptionalSlot");
}

// The value of an optional tuple or struct is at most 64 bytes in place; a
// bigger one goes in a `memory::Box`, whose optional is a reference.
TEST(Optional, ValueOf64BytesInPlaceAndBiggerBoxed) {
  LeakGuard g;
  auto r = compileAndRun(R"pkn(import ::io;
import ::memory;
struct Eight { a: int; b: int; c: int; d: int; e: int; f: int; g: int; h: int; }
struct Nine { a: int; b: int; c: int; d: int; e: int; f: int; g: int; h: int; i: int; }
fn pick(x: Eight?) -> int { match x { e: Eight { return e.a + e.h; } None { return -1; } } }
fn boxed(n: memory::Box<Nine>?) -> int { match n { b: memory::Box<Nine> { return b.get().i; } None { return -2; } } }
fn main() -> int {
  t: (Str, int, int, int, int, int, int, int)? = ("t", 2, 3, 4, 5, 6, 7, 8);
  io::println(Str(pick(Eight(1, 2, 3, 4, 5, 6, 7, 8))) + " " + Str(pick(None)));
  match t { v: (Str, int, int, int, int, int, int, int) { io::println(v.0 + Str(v.7)); } None { io::println("none"); } }
  io::println(Str(boxed(memory::Box<Nine>(Nine(1, 2, 3, 4, 5, 6, 7, 8, 9)))) + " " + Str(boxed(None)));
  return 0;
}
)pkn");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9 -1\nt8\n9 -2\n");
  g.expectNoLeaks("ValueOf64BytesInPlaceAndBiggerBoxed");
}
