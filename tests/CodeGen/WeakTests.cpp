// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: weak references (docs/language/16-memory.md, "Weak
// references"): `weak` locals and fields read as None once their object is
// gone, break reference cycles, and are released like any reference.  A
// weak reference is a `shared` reference that owns nothing: `weak` names a
// `shared` object, `weak mut` a `shared mut` one.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// A `weak` local refers to its object without keeping it alive: it reads as
// the object while something else holds it and as None afterwards, is
// assigned like any `T?` variable, and a strong copy taken from it keeps
// the object (which is `shared`, so it can have that second owner).
TEST(Weak, LocalsReadNoneOnceTheObjectIsGone) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { value: int; }
    impl Node { fn __init__(v: int) { self.value = v; } }
    fn show(w: view Node?) -> Str {
      match w { p: Node { return Str(p.value); } None { return "gone"; } }
    }
    fn main() -> int {
      a: shared = Node(1);
      w: weak Node? = a;
      u: weak = a;
      e: weak Node?;
      io::println(show(w) + " " + show(u) + " " + show(e) + " " + Str(w == None) +
              " " + Str(e == None));
      a = Node(2);
      io::println(show(w) + " " + show(u) + " " + Str(w == None) + " " + Str(w == u));
      w = a;
      u = None;
      io::println(show(w) + " " + show(u));
      s: shared Node? = w;
      a = Node(3);
      io::println(show(w) + " " + show(s));
      s = None;
      io::println(show(w));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1 1 gone False True\n"
                      "gone gone True True\n"
                      "2 gone\n"
                      "2 2\n"
                      "gone\n");
  g.expectNoLeaks("weak locals");
}

// A `weak` field is the back link of a tree: the children hold their parent
// weakly, so the tree is freed when its root is dropped, and a child whose
// parent is gone sees None.  The nodes are `shared mut`: the tree and the
// locals both hold them, the back link is `weak mut`, and `add`, which
// stores `self` there, is a `shared fn` (its `self` is `shared mut`).
TEST(Weak, FieldsBreakCycles) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node {
      name: Str;
      parent: weak mut Node?;
      first: shared mut Node?;
      next: shared mut Node?;
    }
    impl Node {
      fn __init__(name: Str) {
        self.name = move name; self.parent = None; self.first = None; self.next = None;
      }
      shared fn add(child: shared mut Node) {
        child.parent = self;
        child.next = self.first;
        self.first = child;
      }
      view fn path() -> Str {
        match self.parent {
          p: Node { return p.path() + "/" + self.name; }
          None { return self.name + ""; }
        }
      }
    }
    fn main() -> int {
      root: shared mut = Node("root");
      a: shared mut = Node("a");
      b: shared mut = Node("b");
      root.add(a);
      a.add(b);
      io::println(b.path());
      w: weak mut = root;
      root = Node("other");
      io::println(b.path());
      match w { n: Node { io::println(n.name); } None { io::println("root gone"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "root/a/b\na/b\nroot gone\n");
  g.expectNoLeaks("weak fields");
}

// A lambda captures a `weak` variable weakly: a closure stored in the object
// it refers to forms no cycle, and a call after the object is gone sees
// None.  The button is `shared mut` and `wire` a `shared fn`, so `self` can
// be named by a `weak mut` local.  The closure is taken out of the button
// (`mem::replace`) before the button goes.
TEST(Weak, LambdasCaptureWeakly) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::memory as mem;
    class Button {
      label: Str;
      onClick: () -> Str;
    }
    impl Button {
      fn __init__(label: Str) {
        self.label = move label;
        self.onClick = fn() -> Str { return "unwired"; };
      }
      shared fn wire() {
        me: weak mut Button? = self;
        self.onClick = fn() -> Str {
          match me {
            b: Button { return "clicked " + b.label; }
            None { return "no button"; }
          }
        };
      }
      view fn click() -> Str { return self.onClick(); }
    }
    fn main() -> int {
      b: shared mut = Button("ok");
      io::println(b.click());
      b.wire();
      io::println(b.click());
      f = mem::replace(b.onClick, fn() -> Str { return "taken"; });
      b = Button("other");
      io::println(f());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "unwired\nclicked ok\nno button\n");
  g.expectNoLeaks("weak captures");
}

// A lambda inside a lambda captures a `weak` variable weakly too, however
// deep it is: the closure an outer one returns sees None once the object is
// gone, and one stored in the object (a `shared mut` one, named by a `weak
// mut` local) forms no cycle.  (Each inner closure held the object strongly:
// `alive`, and two blocks leaked.)
TEST(Weak, NestedLambdasCaptureWeakly) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class B { n: int; f: (() -> Str)?; }
    fn state(w: view B?) -> Str => if w == None then "gone" else "alive";
    fn main() -> int {
      b: shared mut = B(1, None);
      me: weak mut = b;
      let outer = fn() -> () -> Str { return fn() -> Str => state(me); };
      let three = fn() -> () -> () -> Str {
        return fn() -> () -> Str { return fn() -> Str => state(me); };
      };
      let inner = outer();
      let deep = three()();
      io::println(inner() + " " + deep());
      b.f = outer();
      b = B(2, None);
      io::println(inner() + " " + deep() + " " + three()()());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "alive alive\ngone gone gone\n");
  g.expectNoLeaks("nested weak captures");
}

// A weak reference goes everywhere a `T?` goes: passed by value and as a
// `view`, returned, stored in a strong or a weak field, and taken from a
// weak field.  The object is `shared`, as a weak reference's always is, so
// what a weak reference gives can be another owner of it.  A tuple holds no
// `shared` reference, so no destructuring assigns a weak variable.
TEST(Weak, PassedReturnedAndStored) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class P { v: int; }
    impl P { fn __init__(v: int) { self.v = v; } }
    fn take(p: shared P?) -> Str { match p { q: P { return Str(q.v); } None { return "none"; } } }
    fn look(p: view P?) -> Str { match p { q: P { return Str(q.v); } None { return "none"; } } }
    fn pass(p: shared P?) -> shared P? { return p; }
    class Holder { item: shared P?; peek: weak P?; }
    impl Holder { fn __init__() { self.item = None; self.peek = None; } }
    fn main() -> int {
      x: shared = P(5);
      w: weak = x;
      h: mut = Holder();
      h.item = w;
      h.peek = x;
      h2: mut = Holder();
      h2.peek = h.peek;
      io::println(take(w) + " " + look(w) + " " + take(pass(w)) + " " + take(h.item) +
              " " + take(h.peek) + " " + take(h2.peek));
      x = P(6);
      io::println(take(w) + " " + take(h2.peek));
      h.item = None;
      io::println(take(w) + " " + take(h.peek) + " " + take(h2.peek));
      w = None;
      n = 1;
      io::println(take(w) + " " + Str(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 5 5 5 5 5\n5 5\nnone none none\nnone 1\n");
  g.expectNoLeaks("weak values");
}

// A `weak` field is inherited, and a generic class may have one; the objects
// they name are `shared`.
TEST(Weak, InheritedAndGenericFields) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Base { owner: weak Base?; }
    impl Base { fn __init__() { self.owner = None; } }
    class Derived : Base { tag: Str; }
    impl Derived { fn __init__(t: Str) { __super__(); self.tag = move t; } }
    class Cell<T> { value: T; next: weak Cell<T>?; }
    impl Cell<T> { fn __init__(v: T) { self.value = move v; self.next = None; } }
    fn main() -> int {
      d: mut = Derived("d");
      e: shared = Derived("e");
      d.owner = e;
      match d.owner { o: Derived { io::println(o.tag); } _ { io::println("other"); } }
      e = Derived("f");
      match d.owner { o: Base { io::println("alive"); } None { io::println("gone"); } }
      c1: mut = Cell<int>(1);
      c2: shared = Cell<int>(2);
      c1.next = c2;
      match c1.next { n: Cell<int> { io::println(Str(n.value)); } None { io::println("none"); } }
      c2 = Cell<int>(3);
      match c1.next { n: Cell<int> { io::println(Str(n.value)); } None { io::println("none"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "e\ngone\n2\nnone\n");
  g.expectNoLeaks("inherited and generic weak fields");
}

// Weak locals declared in loops are released on every exit (`continue`,
// `break`, `return`), and a returned weak reference is loaded before the
// locals that keep its object go (the object is `shared`, so the result is
// another owner of it).
TEST(Weak, LoopsAndEarlyExits) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class P { v: int; }
    impl P { fn __init__(v: int) { self.v = v; } }
    fn find(limit: int) -> shared P? {
      i = 0;
      while i < 10 {
        p: shared = P(i);
        w: weak = p;
        if i == limit { return w; }
        if i % 2 == 0 { i = i + 1; continue; }
        if i > 7 { break; }
        i = i + 1;
      }
      return None;
    }
    fn main() -> int {
      match find(3) { p: P { io::println(Str(p.v)); } None { io::println("none"); } }
      match find(20) { p: P { io::println(Str(p.v)); } None { io::println("none"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3\nnone\n");
  g.expectNoLeaks("weak locals in loops");
}

// A `view fn` walks a chain of nodes through a `weak` cursor: the cursor
// takes no reference to `self`'s nodes, and each read of it is a `view` the
// method may bind and read.  A method that may change `self` walks one as
// well and returns what it finds, which outlives the chain: the nodes are
// `shared`.
TEST(Weak, CursorInAViewMethod) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; next: shared Node?; }
    class Chain { head: shared Node?; }
    impl Chain {
      fn __init__() { self.head = None; }
      fn push(v: int) { self.head = Node(v, self.head); }
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
      fn find(v: int) -> shared Node? {
        cur: weak = self.head;
        while cur != None {
          match cur {
            n: Node {
              if n.v == v { return n; }
              cur = n.next;
            }
            None { }
          }
        }
        return None;
      }
    }
    fn show(c: inout Chain) -> Str {
      match c.find(2) {
        n: Node { return Str(c.sum()) + " found " + Str(n.v); }
        None { return Str(c.sum()) + " none"; }
      }
    }
    fn main() -> int {
      c: mut = Chain();
      io::println(show(c));
      c.push(1);
      c.push(2);
      c.push(3);
      io::println(show(c));
      kept: shared Node? = c.find(2);
      c = Chain();
      match kept { n: Node { io::println(Str(n.v)); } None { io::println("gone"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0 none\n6 found 2\n2\n");
  g.expectNoLeaks("weak cursor");
}

// The runtime's classes are classes: a `shared Str` or `shared Obj` can be
// referred to weakly, and the immortal `None` object stays.
TEST(Weak, StringsAndObjects) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s: shared = "abc";
      ws: weak Str? = s;
      o: shared Obj = None;
      wo: weak Obj? = o;
      match ws { t: Str { io::println(t); } None { io::println("gone"); } }
      s = "x";
      match ws { t: Str { io::println(t); } None { io::println("gone"); } }
      io::println(Str(wo == None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "abc\ngone\nFalse\n");
  g.expectNoLeaks("weak strings");
}
