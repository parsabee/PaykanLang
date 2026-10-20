// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Codegen tests: weak references (docs/language/08-memory-model.md, "Weak
// references"): `weak` locals and fields read as None once their object is
// gone, break reference cycles, and are released like any reference.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// A `weak` local refers to its object without keeping it alive: it reads as
// the object while something else holds it and as None afterwards, is
// assigned like any `T?` variable, and a strong copy taken from it keeps
// the object.
TEST(Weak, LocalsReadNoneOnceTheObjectIsGone) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { value: int; }
    impl Node { fn __init__(v: int) { self.value = v; } }
    fn show(w: Node?) -> Str {
      match w { p: Node { return Str(p.value); } None { return "gone"; } }
    }
    fn main() -> int {
      a = Node(1);
      weak w: Node? = a;
      weak u = a;
      weak e: Node?;
      io::println(show(w) + " " + show(u) + " " + show(e) + " " + Str(w == None) +
              " " + Str(e == None));
      a = Node(2);
      io::println(show(w) + " " + show(u) + " " + Str(w == None) + " " + Str(w == u));
      w = a;
      u = None;
      io::println(show(w) + " " + show(u));
      s: Node? = w;
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
// parent is gone sees None.
TEST(Weak, FieldsBreakCycles) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node {
      name: Str;
      weak parent: Node?;
      first: Node?;
      next: Node?;
    }
    impl Node {
      fn __init__(name: Str) {
        self.name = name; self.parent = None; self.first = None; self.next = None;
      }
      fn add(child: Node) {
        child.parent = self;
        child.next = self.first;
        self.first = child;
      }
      fn path() -> Str {
        match self.parent {
          p: Node { return p.path() + "/" + self.name; }
          None { return self.name; }
        }
      }
    }
    fn main() -> int {
      root = Node("root");
      a = Node("a");
      b = Node("b");
      root.add(a);
      a.add(b);
      io::println(b.path());
      weak w = root;
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
// None.
TEST(Weak, LambdasCaptureWeakly) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Button {
      label: Str;
      onClick: () -> Str;
    }
    impl Button {
      fn __init__(label: Str) {
        self.label = label;
        self.onClick = fn() -> Str { return "unwired"; };
      }
      fn wire() {
        weak me: Button? = self;
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
      b = Button("ok");
      io::println(b.click());
      b.wire();
      io::println(b.click());
      f = b.onClick;
      b = Button("other");
      io::println(f());
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "unwired\nclicked ok\nno button\n");
  g.expectNoLeaks("weak captures");
}

// A weak reference goes everywhere a `T?` goes: passed by value and as a
// `view`, returned, stored in a strong or a weak field, taken from a weak
// field, and assigned by destructuring.
TEST(Weak, PassedReturnedAndStored) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class P { v: int; }
    impl P { fn __init__(v: int) { self.v = v; } }
    fn take(p: P?) -> Str { match p { q: P { return Str(q.v); } None { return "none"; } } }
    fn look(p: view P?) -> Str { match p { q: P { return Str(q.v); } None { return "none"; } } }
    fn pass(p: P?) -> P? { return p; }
    class Holder { item: P?; weak peek: P?; }
    impl Holder { fn __init__() { self.item = None; self.peek = None; } }
    fn main() -> int {
      x = P(5);
      weak w = x;
      h = Holder();
      h.item = w;
      h.peek = x;
      h2 = Holder();
      h2.peek = h.peek;
      io::println(take(w) + " " + look(w) + " " + take(pass(w)) + " " + take(h.item) +
              " " + take(h.peek) + " " + take(h2.peek));
      x = P(6);
      io::println(take(w) + " " + take(h2.peek));
      h.item = None;
      io::println(take(w) + " " + take(h.peek) + " " + take(h2.peek));
      w, n = (x, 1);
      io::println(take(w) + " " + Str(n));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 5 5 5 5 5\n5 5\nnone none none\n6 1\n");
  g.expectNoLeaks("weak values");
}

// A `weak` field is inherited, and a generic class may have one.
TEST(Weak, InheritedAndGenericFields) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Base { weak owner: Base?; }
    impl Base { fn __init__() { self.owner = None; } }
    class Derived : Base { tag: Str; }
    impl Derived { fn __init__(t: Str) { __super__(); self.tag = t; } }
    class Cell<T> { value: T; weak next: Cell<T>?; }
    impl Cell<T> { fn __init__(v: T) { self.value = v; self.next = None; } }
    fn main() -> int {
      d = Derived("d");
      e = Derived("e");
      d.owner = e;
      match d.owner { o: Derived { io::println(o.tag); } _ { io::println("other"); } }
      e = Derived("f");
      match d.owner { o: Base { io::println("alive"); } None { io::println("gone"); } }
      c1 = Cell<int>(1);
      c2 = Cell<int>(2);
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
// locals that keep its object go.
TEST(Weak, LoopsAndEarlyExits) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class P { v: int; }
    impl P { fn __init__(v: int) { self.v = v; } }
    fn find(limit: int) -> P? {
      i = 0;
      while i < 10 {
        p = P(i);
        weak w = p;
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
// takes no reference to `self`'s nodes, and each read of it is a fresh
// reference the method may bind, read and return.
TEST(Weak, CursorInAViewMethod) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; next: Node?; }
    class Chain { head: Node?; }
    impl Chain {
      fn __init__() { self.head = None; }
      fn push(v: int) { self.head = Node(v, self.head); }
      view fn sum() -> int {
        total = 0;
        weak cur = self.head;
        while cur != None {
          match cur {
            n: Node { total = total + n.v; cur = n.next; }
            None { }
          }
        }
        return total;
      }
      view fn find(v: int) -> Node? {
        weak cur = self.head;
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
    fn show(c: view Chain) -> Str {
      match c.find(2) {
        n: Node { return Str(c.sum()) + " found " + Str(n.v); }
        None { return Str(c.sum()) + " none"; }
      }
    }
    fn main() -> int {
      c = Chain();
      io::println(show(c));
      c.push(1);
      c.push(2);
      c.push(3);
      io::println(show(c));
      kept: Node? = c.find(2);
      c = Chain();
      match kept { n: Node { io::println(Str(n.v)); } None { io::println("gone"); } }
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "0 none\n6 found 2\n2\n");
  g.expectNoLeaks("weak cursor");
}

// The runtime's classes are classes: a `Str` or an `Obj` can be referred to
// weakly, and the immortal `None` object stays.
TEST(Weak, StringsAndObjects) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    fn main() -> int {
      s = "abc";
      weak ws: Str? = s;
      o: Obj = None;
      weak wo: Obj? = o;
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
