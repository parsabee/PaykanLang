// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Codegen tests: qualifiers on the parts of a type (#368;
// docs/language/19-concurrency.md, "Qualifiers in a type").  A part held
// `shared` is the same object in every copy of what holds it, counted by
// each copy: a tuple's or an array's element, a container's element through
// its type argument, and a function value's parameter or result.

#include "CodeGenTestUtils.h"
#include <gtest/gtest.h>

using namespace paykan::test;

// Copies of a tuple or an array of `shared mut` nodes name the same nodes:
// a write through one copy is seen through the others, and each copy, a
// destructured element and a weak reference count as they do for a
// variable.
TEST(QualifiedPart, CopiesOfElementsNameOneObject) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    fn main() -> int {
      s: shared mut Node = Node(1);
      t = (10, s);
      u = t;
      u.1.v = 2;
      xs = [s, Node(5)];
      ys = xs;
      ys[0].v = 3;
      ks = [s; 3];
      ks[2].v += 1;
      n, k = t;
      k.v *= 10;
      w: weak mut Node? = ys[1];
      io::println(Str(s.v) + " " + Str(t.1.v) + " " + Str(xs[0].v) + " " +
                  Str(n) + " " + Str(w == None));
      ys = [s, s];
      xs = [s, s];
      io::println(Str(w == None));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "40 40 40 10 False\nTrue\n");
  g.expectNoLeaks("CopiesOfElementsNameOneObject");
}

// A container of `shared mut` references: its elements are the program's
// objects, read out as copies (`get`, whose clone of a `T` with T =
// `shared mut Node` is the copy), and the hash table, which chains its
// entries from a buffer of `shared mut` references, holds them as values.
TEST(QualifiedPart, ContainersHoldSharedReferences) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as cl;
    class Node { v: int; }
    fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
    fn main() -> int {
      s: shared mut Node = Node(1);
      l = cl::List<shared mut Node>();
      l.push(s);
      l.push(Node(7));
      l.get(0).v = 2;
      p = twice(s);
      p.1.v += 1;
      m = cl::HashMap<Str, shared mut Node>();
      m.put("a", s);
      m.put("b", l.get(1));
      m.get("b").v = 8;
      copy = l.clone();
      copy.get(0).v += 10;
      io::println(Str(s.v) + " " + Str(l.at(1).v) + " " + Str(m.at("a").v) +
                  " " + Str(copy.len()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "13 8 13 2\n");
  g.expectNoLeaks("ContainersHoldSharedReferences");
}

// A function value whose parameter and result are `shared mut`: a call
// through it passes the program's object and gives one back, and a lambda
// that captures one keeps it as long as the function value lives.
TEST(QualifiedPart, FunctionValuesTakeAndGiveSharedReferences) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    fn bump(n: shared mut Node) -> shared mut Node { n.v += 1; return n; }
    fn main() -> int {
      s: shared mut Node = Node(1);
      f: (shared mut Node) -> shared mut Node = bump;
      g = fn(n: shared mut Node) -> shared mut Node { n.v *= 2; return n; };
      r = g(f(s));
      r.v += 1;
      h = fn() -> shared mut Node { return s; };
      io::println(Str(s.v) + " " + Str(h().v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 5\n");
  g.expectNoLeaks("FunctionValuesTakeAndGiveSharedReferences");
}

// The containers that move their values out of their nodes (a LinkedList,
// a Stack, a Queue) take a `shared` type argument: in a template's code a
// `shared` value moves as its copy.  A generic enum of a `shared`
// reference is held in a list, and cloned by copying.
TEST(QualifiedPart, EveryContainerTakesASharedTypeArgument) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as cl;
    class Node { id: int; }
    enum Opt<T> { Some(v: T), Nada }
    fn main() -> int {
      s: shared mut Node = Node(1);
      ll: mut = cl::LinkedList<shared mut Node>();
      ll.pushBack(s);
      ll.pushFront(Node(2));
      back = ll.popBack();
      back.id = 3;
      st = cl::Stack<shared mut Node>();
      st.push(s);
      q = cl::Queue<shared mut Node>();
      q.enqueue(s);
      let front = q.dequeue();
      front.id += 1;
      l = cl::List<Opt<shared mut Node>>();
      l.push(Opt::Some(s));
      copy = l.clone();
      match copy.get(0) {
        o: Some { o.v.id *= 10; }
        Nada { }
      }
      io::println(Str(s.id) + " " + Str(ll.len()) + " " + Str(st.pop().id));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "40 1 40\n");
  g.expectNoLeaks("EveryContainerTakesASharedTypeArgument");
}

// In a template's code a `T` with T = `shared mut Node` clones as its copy
// however it is reached: a variable, a tuple's element, an array's element,
// a list made of an array.  The class's own `clone` runs in the program's
// code, on a variable declared `shared`.
TEST(QualifiedPart, ATemplatesSharedValuesCloneAsTheirCopy) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as cl;
    class Node { id: int; }
    impl Cloneable for Node {}
    fn viaVar<T: Cloneable>(v: T) -> T { x: T = move v; return x.clone(); }
    fn viaTuple<T: Cloneable>(p: (T, int)) -> T { return p.0.clone(); }
    fn viaArray<T: Cloneable>(v: T) -> T { xs: T[2] = [v.clone(), move v]; return xs[1].clone(); }
    fn main() -> int {
      s: shared mut Node = Node(0);
      viaVar(s).id = 1;
      io::print(Str(s.id));
      viaTuple<shared mut Node>((s, 0)).id = 2;
      io::print(" " + Str(s.id));
      viaArray(s).id = 3;
      io::print(" " + Str(s.id));
      arr: shared mut Node[2] = [s, s];
      l = cl::listOf<shared mut Node>(arr);
      l.get(0).id = 5;
      io::print(" " + Str(s.id));
      c: mut = s.clone();
      c.id = 9;
      io::println(" " + Str(s.id) + " " + Str(c.id));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "1 2 3 5 5 9\n");
  g.expectNoLeaks("ATemplatesSharedValuesCloneAsTheirCopy");
}

// A fresh object lent to a `view T` with T = `atomic shared Node` is
// frozen there, as one flowing into a slot of that form is, so the tasks
// that share the clone (its copy) count it atomically; and `move n.value`
// of a type argument's form in a template is a copy that leaves the field.
TEST(QualifiedPart, AFreshValueTakesTheBorrowsFormAtRunTime) {
  auto r = compileAndRun(R"(import ::io;
    class Node { id: int; }
    class Cell<T> { value: T; }
    fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
    fn take<T>(c: Cell<T>) -> T { return move c.value; }
    async fn read(n: atomic shared Node) -> int { return n.id; }
    fn main() -> int {
      let f = dup<atomic shared Node>(Node(7));
      let t = read(f);
      let u = read(f);
      s: shared mut Node = Node(1);
      let c = Cell<shared mut Node>(s);
      let v = take(move c);
      v.id = 9;
      io::println(Str((await t) + (await u)) + " " + Str(s.id));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "14 9\n");
}

// A new value assigned to a `locked mut` variable becomes a cell there, as
// one stored in a field does, and a conditional's None into a `locked mut
// Node?` slot stays None, no cell around nothing.  A template's `parallel
// for` is one in each instance.
TEST(QualifiedPart, CellsAreMadeWhereValuesBecomeThem) {
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    fn sum<T>(xs: T[4]) -> int {
      total = 0;
      parallel for i in 0..4 { total += 1; }
      return total;
    }
    fn main() -> int {
      l: locked mut Node = Node(1);
      l = Node(5);
      lock l as n: inout { n.v += 1; }
      lock l as n: view { io::print(Str(n.v)); }
      let c = False;
      let o: locked mut Node? = if c then Node(2) else None;
      match o { n: Node { io::print(" cell"); } None { io::print(" none"); } }
      io::println(" " + Str(sum<int>([1, 2, 3, 4])));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "6 none 4\n");
}

// A match binding holds its subject's form: an `inout` arm of a `shared
// mut` optional trades shared references, and in a template's code the
// clone of a `view T?` subject's binding with T = `shared mut Node` is the
// same object.  An arm over a borrowed subject binds it without counting.
TEST(QualifiedPart, MatchBindingsHoldTheirSubjectsForm) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::memory as mem;
    class Base { v: int; }
    class Node : Base { w: int; }
    fn first<T: Cloneable>(o: view T?) -> T? {
      match o { v: T { return v.clone(); } None { return None; } }
    }
    fn peek(o: view Base) -> int {
      match o { n: Node { return n.w; } _ { return 0; } }
    }
    fn main() -> int {
      s: shared mut Node = Node(1, 2);
      o: shared mut Node? = s;
      match first<shared mut Node>(o) { n: Node { n.w = 7; } None { } }
      match o {
        n: inout Node {
          let old = mem::replace(n, Node(0, 9));
          old.w += 1;
        }
        None { }
      }
      match o { n: Node { io::print(Str(n.w) + " "); } None { } }
      io::println(Str(s.w) + " " + Str(peek(s)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "9 8 8\n");
  g.expectNoLeaks("MatchBindingsHoldTheirSubjectsForm");
}

// An arm over a borrowed subject binds it without counting, so what leaves
// the arm is counted where it lands: a clone pushed into a list, as its
// copy, outlives the subject's variable, and a `view fn`'s arm over `self`
// reads its binding.
TEST(QualifiedPart, AnUncountedBindingIsCountedWhereItLands) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::collections as cl;
    class Base { v: int; }
    class Node : Base { w: int; }
    fn keepAll<T: Cloneable>(o: view T?, l: inout cl::List<T>) {
      match o { v: T { l.push(v.clone()); } None { } }
    }
    fn grab<T: Cloneable>(o: view T?) -> T? {
      match o { v: T { let k = v.clone(); return k; } None { return None; } }
    }
    impl Base {
      view fn kind() -> int {
        match self { n: Node { return n.w; } _ { return 0; } }
      }
    }
    fn main() -> int {
      l: cl::List<shared mut Node> = cl::List<shared mut Node>();
      o: shared mut Node? = Node(1, 2);
      keepAll(o, l);
      keepAll(o, l);
      o = None;
      let g = grab<shared mut Node>(l.get(0));
      match g { n: Node { n.w = 5; } None { } }
      io::print(Str(l.get(1).w) + " " + Str(l.len()) + " ");
      b: Base = Node(3, 4);
      io::println(Str(b.kind()));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "5 2 4\n");
  g.expectNoLeaks("AnUncountedBindingIsCountedWhereItLands");
}

// A `shared fn`'s `self` is a `shared mut` reference, so a binding of it is
// a copy the arm may assign, move or pass `inout`, counted as any copy is;
// the object `self` names stays as it was.
TEST(QualifiedPart, ASharedFnsSelfBindingIsACountedCopy) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    import ::memory as mem;
    class Base { v: int; }
    class Node : Base { w: int; }
    fn take(x: shared mut Node) -> int { return x.w; }
    impl Base {
      shared fn swapOut() -> int {
        match self {
          n: Node {
            let old = mem::replace(n, Node(0, 0));
            return old.w + n.w;
          }
          _ { return 0; }
        }
      }
      shared fn renew() -> int {
        match self {
          n: Node { for i in 0..3 { n = Node(i, i); } return n.w; }
          _ { return 0; }
        }
      }
      shared fn hand() -> int {
        match self { n: Node { return take(move n); } _ { return 0; } }
      }
    }
    fn main() -> int {
      s: shared mut Base = Node(1, 2);
      io::print(Str(s.swapOut()) + " " + Str(s.swapOut()) + " ");
      io::print(Str(s.renew()) + " " + Str(s.hand()) + " ");
      io::println(Str(s.v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "2 2 2 2 1\n");
  g.expectNoLeaks("ASharedFnsSelfBindingIsACountedCopy");
}

// An arm's `view` of a `view` parameter is another name for it, lent on to
// each call as the parameter is; an optional `view` parameter's arm and a
// counted arm over a field read the same object.
TEST(QualifiedPart, ABorrowedArmIsLentAsItsSubjectIs) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Animal { n: int; }
    class Dog : Animal { }
    class H { a: Animal; }
    fn show(x: view Animal) -> int { return x.n; }
    fn viaParam(p: view Animal, k: int) -> int {
      t = 0;
      match p { d: Dog { for i in 0..k { t += show(d); } } _ { } }
      return t;
    }
    fn viaOptional(p: view Animal?) -> int {
      match p { d: Dog { return show(d); } _ { return 0; } }
    }
    fn viaField(h: view H, k: int) -> int {
      t = 0;
      match h.a { d: Dog { for i in 0..k { t += show(d); } } _ { } }
      return t;
    }
    fn main() -> int {
      let h = H(Dog(7));
      o: Animal? = Dog(5);
      io::println(Str(viaParam(h.a, 3)) + " " + Str(viaOptional(o)) + " " +
                  Str(viaField(h, 3)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "21 5 21\n");
  g.expectNoLeaks("ABorrowedArmIsLentAsItsSubjectIs");
}

// A `lock` block's `view` binding is the address of the cell's contents,
// so an arm's `view` of it reads the contents through that address: a
// local cell, a field cell and an optional cell, passed on to a `view`
// parameter too.  A cell is never freed for now (docs/language/
// 19-concurrency.md, "Limits for now"): the three cells and their contents
// are the six blocks left, and the arms add none.
TEST(QualifiedPart, AnArmInALockBlockReadsTheContents) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Base { v: int; }
    class Dog : Base { w: int; }
    class H { c: locked mut Base; }
    fn show(d: view Dog) -> int { return d.w; }
    fn peek(h: view H) -> int {
      t = 0;
      lock h.c as v: view { match v { d: Dog { t = d.w; } _ { t = -1; } } }
      return t;
    }
    fn main() -> int {
      let c: locked mut Base = Dog(1, 2);
      lock c as v: view {
        match v { d: Dog { io::print("dog " + Str(show(d)) + " "); } _ { } }
      }
      o: locked mut Base? = Dog(3, 4);
      match o {
        b: Base {
          lock b as v: view { match v { d: view Dog { io::print(Str(d.w) + " "); } _ { } } }
        }
        None { }
      }
      let h = H(Dog(5, 6));
      io::println(Str(peek(h)));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "dog 2 4 6\n");
  EXPECT_EQ(liveBlocksAfterRun(), 0);
}

// A template's `T: Copyable` copies a `view` of a type argument's form as
// the argument copies (#368): with T = `shared mut Node`, a `view fn`'s
// field, a `view` slice's element and a `view` parameter come out as the
// same object, counted.
TEST(QualifiedPart, ACopyableArgumentsFormIsCopiedOutOfABorrow) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::io;
    class Node { v: int; }
    class Box<T: Copyable> { v: T; view fn get() -> T { return self.v; } }
    impl Box<T: Copyable> { fn __init__(v: T) { self.v = move v; } }
    fn first<T: Copyable>(xs: view T[]) -> T { return xs[0]; }
    fn keep<T: Copyable>(x: view T) -> T { let k = x; return k; }
    fn main() -> int {
      s: shared mut Node = Node(1);
      let b = Box<shared mut Node>(s);
      let g = b.get();
      g.v = 2;
      let a: shared mut Node[2] = [s, s];
      let t = first<shared mut Node>(a);
      let k = keep<shared mut Node>(s);
      k.v += 1;
      io::println(Str(s.v) + " " + Str(t.v) + " " + Str(b.get().v));
      return 0;
    }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "3 3 3\n");
  g.expectNoLeaks("ACopyableArgumentsFormIsCopiedOutOfABorrow");
}

// The archetype check (#368) instantiates an unused template, whose code
// copies another module's instance (`cl::List<Str>`) into this one: none
// of it is lowered, and the program builds and runs.
TEST(QualifiedPart, AnUnusedTemplatesInstancesAreNotLowered) {
  LeakGuard g;
  auto r = compileAndRun(R"(import ::collections as cl;
    import ::io;
    fn names<T>(x: view T) -> unsigned {
      l = cl::List<Str>();
      l.push("a");
      return l.len();
    }
    fn main() -> int { io::println("ok"); return 0; }
  )");
  ASSERT_TRUE(r.CompileOk) << r.StdErr;
  EXPECT_EQ(r.StdOut, "ok\n");
  g.expectNoLeaks("AnUnusedTemplatesInstancesAreNotLowered");
}
