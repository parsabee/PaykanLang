// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: qualifiers on the parts of a type (#368;
// docs/language/19-concurrency.md, "Qualifiers in a type"): a type
// argument (`List<shared Node>`), a tuple's or an array's element
// (`(int, shared Node)`, `shared Node[4]`) and a function type's parameter
// or result (`(shared Node) -> int`).  A qualified part is part of the
// type: `List<shared Node>` is an instance of its own, Copyable where
// `List<Node>` is not, and a qualifier written on `T` joins T's own.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>

using namespace paykan::test;

namespace {

/// Expect @p r to have failed with each of @p diags.
void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

} // namespace

// A qualifier on a part is kept on the type: a tuple's or an array's
// element held `shared` copies, and so does what holds it; the instance of
// a qualified type argument is its own, and its members take T's form.
TEST(QualifiedPart, PartsKeepTheirQualifier) {
  auto r = semaCheck(R"(import ::collections as cl;
class Node { v: int; }
fn keep(n: shared mut Node) -> int { return n.v; }
fn main() -> int {
  s: shared mut Node = Node(1);
  t: (int, shared mut Node) = (1, s);
  u = t;
  xs: shared mut Node[3] = [s, s, Node(2)];
  ys = xs;
  l = cl::List<shared mut Node>();
  l.push(s);
  l.push(Node(3));
  first = l.get(0);
  first.v = 4;
  f: (shared mut Node) -> int = keep;
  return u.1.v + ys[2].v + f(s) + keep(t.1);
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  // `List<shared Node>` is not `List<Node>`, and a `shared` element never
  // becomes a unique one.
  auto distinct = semaCheck(R"(import ::collections as cl;
class Node { v: int; }
fn main() -> int {
  a: cl::List<Node> = cl::List<shared Node>();
  t = (1, Node(2));
  b: (int, shared Node) = move t;
  s: shared Node = Node(3);
  c: (int, Node) = (1, s);
  d = (1, s);
  e: (int, Node) = d;
  return 0;
}
)");
  expectErrors(distinct,
               {":4:3: error: initializer of type 'List<shared Node>' does not "
                "match declared type 'List<Node>' for variable 'a'",
                ":8:24: error: cannot store a 'shared Node' in an element of "
                "the tuple, a 'Node'",
                ":10:3: error: initializer of type '(int, shared Node)' does "
                "not match declared type '(int, Node)' for variable 'e'"});
  EXPECT_EQ(distinct.ErrorCount, 3u) << distinct.Diagnostics;
}

// What each part takes: a type argument every form but `locked`, an element
// `shared` and `shared mut` (it is written as its variable is, so no `mut`),
// and a function type's part the forms a parameter takes but the handles;
// a borrow in a function type takes none, as a parameter does.
TEST(QualifiedPart, EachPartTakesItsForms) {
  auto r = semaCheck(R"(import ::collections as cl;
class Node { v: int; }
fn main() -> int {
  a: (int, mut Node) = (1, Node(1));
  b: (int, atomic shared Node) = (1, Node(1));
  c: cl::List<locked mut Node> = cl::List<locked mut Node>();
  d: (atomic shared Node) -> int = fn(n: atomic shared Node) => 1;
  e: (view shared Node) -> int = fn(n: view Node) => 1;
  g: cl::List<shared int> = cl::List<shared int>();
  return 0;
}
)");
  expectErrors(
      r,
      {":4:3: error: an element takes no 'mut': it is written as its variable "
       "is",
       ":5:3: error: 'atomic shared' cannot qualify an element yet",
       ":6:3: error: 'locked mut' cannot qualify a type argument yet: a "
       "cell's contents are reached in a 'lock' block",
       ":7:3: error: 'atomic shared' cannot qualify a function type's "
       "parameter or result yet",
       ":8:3: error: variable 'e' parameter 1 is 'view shared': a borrow "
       "carries no qualifier",
       ":9:3: error: 'shared' qualifies a reference; 'int' is a value"});
  auto fine = semaCheck(R"(import ::collections as cl;
class Node { v: int; }
fn main() -> int {
  a: cl::List<mut Node> = cl::List<mut Node>();
  b: cl::List<atomic shared Node> = cl::List<atomic shared Node>();
  c: (mut Node) -> shared Node = fn(n: mut Node) -> shared Node { return move n; };
  d: shared Node?[2] = [None, Node(1)];
  return 0;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// `shared` and `shared mut` bind to the primary type: on an array of
// references they qualify its elements, and an array of values takes none.
TEST(QualifiedPart, SharedOnAnArrayIsItsElements) {
  auto r = semaCheck(R"(class Node { v: int; }
fn total(xs: view shared Node[]) -> int { return xs[0].v + xs[1].v; }
fn main() -> int {
  s: shared Node = Node(1);
  xs: shared Node[2] = [s, s];
  ys: shared Node[2][2] = [xs, [s, s]];
  copy = ys;
  return total(xs) + copy[1][0].v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
fn main() -> int {
  ns: shared int[2] = [1, 2];
  s: shared Node = Node(1);
  xs: shared Node[2] = [s, s];
  xs[0].v = 2;
  t = (1, s);
  t.1.v = 3;
  return 0;
}
)");
  expectErrors(bad,
               {":3:3: error: 'shared' qualifies a reference; 'int[2]' is a "
                "value",
                ":6:3: error: element 'xs[0]' is immutable (a 'shared Node'; "
                "write 'shared mut Node' to change it)",
                ":8:3: error: element 't.1' is immutable (a 'shared Node'"});
  EXPECT_EQ(bad.ErrorCount, 3u) << bad.Diagnostics;
}

// A qualifier written on `T` joins T's own (docs/design/concurrency.md,
// "Generics and Copyable"): `mut T` with T = `shared mut Node` writes
// through it, `T?` with T = `shared Node` is `shared Node?`, and a join
// that is no qualifier is an error.
TEST(QualifiedPart, AWrittenQualifierJoinsTheArgument) {
  auto r = semaCheck(R"(class Node { v: int; }
struct Slot<T> { x: mut T; o: T?; }
fn main() -> int {
  s: shared mut Node = Node(1);
  w = Slot<shared mut Node>(s, None);
  w.x.v = 2;
  w.o = s;
  return w.x.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
struct Slot<T> { x: mut T; }
struct Twice<T> { x: shared T; }
fn main() -> int {
  s: shared Node = Node(1);
  w = Slot<shared Node>(s);
  m: shared mut Node = Node(2);
  t = Twice<shared mut Node>(m);
  return 0;
}
)");
  expectErrors(bad,
               {"error: 'mut' cannot qualify the type argument 'shared Node': "
                "a 'shared' reference is never written through",
                "error: 'shared' cannot qualify the type argument 'shared mut "
                "Node': a type takes one qualifier, and 'shared' and 'shared "
                "mut' are two"});
}

// A by-value `T` (or `T?`) parameter binds T to a `shared` argument's form,
// so a shared reference meets `T: Copyable`; a unique argument beside it
// joins that form; a borrow binds the plain type.
TEST(QualifiedPart, InferenceBindsASharedForm) {
  auto r = semaCheck(R"(class Node { v: int; }
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn pick<T>(a: T, b: T) -> T { return a; }
fn maybe<T: Copyable>(x: T?) -> (T?, T?) { return (x, x); }
fn look<T>(x: view T) -> int { return 0; }
fn main() -> int {
  s: shared Node = Node(1);
  p = twice(s);
  q = pick(s, Node(2));
  o: shared Node? = s;
  m = maybe(o);
  n = look(s);
  e = twice<shared Node>(s);
  return p.1.v + q.v + n + e.0.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn main() -> int {
  p = twice(Node(1));
  return 0;
}
)");
  expectErrors(bad, {":4:7: error: 'Node' does not implement trait "
                     "'Copyable', which 'twice' requires of 'T'"});
}

// A literal's elements take its slot's forms, or with none their own
// `shared` one; an array literal's elements agree on it.  A destructuring's
// targets are held as its elements are.
TEST(QualifiedPart, LiteralsAndDestructuringTakeTheElementsForms) {
  auto r = semaCheck(R"(class Node { v: int; }
fn main() -> int {
  s: shared Node = Node(1);
  t = (1, s);
  u = t;
  xs = [s, Node(2)];
  ys = xs;
  ks = [s; 3];
  ls = ks;
  a, b = t;
  c = b;
  w: weak Node? = s;
  a, w = t;
  return u.1.v + ys[1].v + ls[2].v + c.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
fn main() -> int {
  s: shared Node = Node(1);
  m: shared mut Node = Node(2);
  xs = [s, m];
  t = (1, s);
  a, b: Node = t;
  return 0;
}
)");
  expectErrors(bad,
               {":5:12: error: array literal has elements of two forms: "
                "'shared Node' and 'shared mut Node'",
                ":7:6: error: cannot store element 1 of the tuple (a 'shared "
                "Node') in variable 'b', a 'Node': a 'shared' reference "
                "converts only as it is"});
}

// A function type's parts are part of it: a function value matches only
// the same qualifiers, and a call through one gives each argument its
// parameter's form and its result the function type's.
TEST(QualifiedPart, FunctionTypesCarryTheirPartsForms) {
  auto r = semaCheck(R"(class Node { v: int; }
fn make() -> shared mut Node { return Node(1); }
fn read(n: shared Node) -> int { return n.v; }
fn main() -> int {
  f: () -> shared mut Node = make;
  n = f();
  n.v = 2;
  g = n;
  h: (shared Node) -> int = read;
  s: shared Node = Node(3);
  return h(s) + g.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
fn read(n: shared Node) -> int { return n.v; }
fn take(n: Node) -> int { return n.v; }
fn main() -> int {
  f: (Node) -> int = read;
  g: (Node) -> int = take;
  s: shared Node = Node(1);
  return g(s);
}
)");
  expectErrors(bad,
               {":5:3: error: initializer of type '(shared Node) -> int' does "
                "not match declared type '(Node) -> int' for variable 'f'",
                ":8:12: error: cannot store a 'shared Node' in parameter 1 of "
                "'g', a 'Node'"});
}

// A template's `T` with T = `shared Node` is Copyable, and so Cloneable: its
// clone is the copy, the same object, though Node has no `clone`.  A
// reference declared `shared` is cloned by its class, into a new unique
// object, which Node cannot.
TEST(QualifiedPart, TheCloneOfASharedTypeArgumentIsItsCopy) {
  auto r = semaCheck(R"(class Node { v: int; }
fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn main() -> int {
  s: shared mut Node = Node(1);
  c = dup<shared mut Node>(s);
  c.v = 2;
  return s.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { v: int; }
fn main() -> int {
  s: shared mut Node = Node(1);
  c = s.clone();
  return 0;
}
)");
  expectErrors(bad, {":4:7: error: class 'Node' is not Cloneable"});
}

// A `shared` part never crosses to another task, as a `shared` field does
// not: not in a tuple, an array or a buffer.
TEST(QualifiedPart, ASharedPartDoesNotCross) {
  auto r = semaCheck(R"(import ::memory as mem;
class Node { v: int; }
class Pair { t: (int, shared Node); }
class Row { xs: shared Node[2]; }
class Store { b: mem::Buffer<shared Node>; }
async fn pair(p: Pair) -> int { return 0; }
async fn row(r: Row) -> int { return 0; }
async fn store(s: Store) -> int { return 0; }
fn main() -> int {
  s: shared Node = Node(1);
  a = pair(Pair((1, s)));
  b = row(Row([s, s]));
  c = store(Store(mem::Buffer<shared Node>()));
  return (await a) + (await b) + (await c);
}
)");
  expectErrors(r, {"field 't.1' is 'shared Node': a 'shared' reference is "
                   "aliased on its own thread",
                   "field 'xs' is 'shared Node': a 'shared' reference",
                   "field 'b' is 'shared Node': a 'shared' reference"});
  EXPECT_EQ(r.ErrorCount, 3u) << r.Diagnostics;
}

// A `view T` or `inout T` whose T is a `shared` form reads as that form, so
// the template's code may keep a copy: it takes exactly that form (a
// `view` also a temporary), and inference binds an `inout` T to its
// argument's form, so `mem::replace` of a `shared mut` place gives back a
// `shared mut` reference, never the object as a unique one.
TEST(QualifiedPart, ABorrowOfATypeArgumentsFormTakesThatForm) {
  auto r = semaCheck(R"(import ::memory as mem;
class Node { id: int; }
fn grab<T>(a: T, x: inout T) -> T { return x; }
fn put<T>(slot: inout T, v: T) { slot = v; }
fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn main() -> int {
  s: shared mut Node = Node(1);
  u: mut Node = Node(2);
  a = grab(s, u);
  w: shared mut Node = Node(3);
  ro: shared Node = Node(4);
  put(w, ro);
  f = dup<atomic shared Node>(u);
  m: shared mut Node = Node(5);
  let back: mut Node = mem::replace(m, Node(6));
  return 0;
}
)");
  expectErrors(
      r,
      {":9:15: error: cannot pass 'u' (a 'mut Node') to parameter 'x' of "
       "'grab<shared mut Node>', an 'inout shared mut Node': a borrow of a "
       "type argument's form takes that form, which the callee may keep a "
       "copy of",
       ":12:3: error: cannot infer type parameter 'T' of 'put': deduced as "
       "both 'shared mut Node' and 'shared Node'",
       ":13:31: error: cannot pass 'u' (a 'mut Node') to parameter 'x' of "
       "'dup<atomic shared Node>', a 'view atomic shared Node'",
       ":15:3: error: cannot store a 'shared mut Node' in variable 'back', a "
       "'mut Node'"});
  auto fine = semaCheck(R"(import ::memory as mem;
class Node { id: int; }
fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn main() -> int {
  m: shared mut Node = Node(5);
  let back = mem::replace(m, Node(6));
  back.id = 7;
  a: shared mut Node = Node(1);
  b: shared mut Node = Node(2);
  mem::swap(a, b);
  c = dup<shared mut Node>(Node(3));
  d = dup<shared mut Node>(a);
  return back.id + c.id + d.id;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// A tuple of unique references converts into one of `shared` ones only as
// a temporary or by `move`: under a `view` the place would still own the
// object the `shared` copy names.  An element declared `shared` is never
// changed through an `inout`.
TEST(QualifiedPart, APlacesUniqueElementsStayUnique) {
  auto r = semaCheck(R"(class Node { id: int; }
fn dup(t: view (int, shared Node)) -> (int, shared Node) { return t.clone(); }
fn grow(n: inout Node) { n = Node(2); }
fn main() -> int {
  u = (1, Node(5));
  let d = dup(u);
  s: shared Node = Node(1);
  xs: shared Node[2] = [s, s];
  grow(xs[0]);
  return 0;
}
)");
  expectErrors(r, {":6:15: error: argument 1 of 'dup' has type '(int, Node)', "
                   "expected '(int, shared Node)'",
                   ":9:8: error: element 'xs[0]' is a 'shared Node', which "
                   "nothing changes, so it cannot be passed to 'inout' "
                   "parameter 'n'"});
  auto fine = semaCheck(R"(class Node { id: int; }
fn dup(t: view (int, shared Node)) -> (int, shared Node) { return t.clone(); }
fn main() -> int {
  u = (1, Node(5));
  let d = dup((2, Node(6)));
  let e: (int, shared Node) = move u;
  return d.1.id + e.1.id;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// Inference sees a part's form through a slice, a tuple and a function
// type, and an inferred result keeps its value's form.
TEST(QualifiedPart, InferenceSeesThePartsForms) {
  auto r = semaCheck(R"(class Node { id: int; }
fn head<T: Cloneable>(xs: view T[]) -> T { return xs[0].clone(); }
fn first<T>(t: (T, int)) -> T { return t.0; }
fn call<T>(f: (T) -> int, x: T) -> int { return f(x); }
fn read(n: shared Node) -> int { return n.id; }
fn same(n: shared Node) => n;
fn main() -> int {
  s: shared Node = Node(1);
  xs: shared Node[2] = [s, s];
  let h = head(xs);
  let x = first((s, 1));
  let f = fn(n: shared Node) => n;
  let k: shared Node = f(s);
  let j: shared Node = same(s);
  return h.id + x.id + call(read, Node(1)) + k.id + j.id;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// What a `view` of a type argument's form takes beyond that form: a fresh
// unique value only (a new object, a unique result, `move u`), never one
// whose form is another (a call's `shared` result, a field of a call's
// result, a branch of a conditional).  An `inout` result names storage of
// exactly its form, and an `inout` of an object's field writes the object.
TEST(QualifiedPart, OnlyAFreshValueTakesABorrowsForm) {
  auto r = semaCheck(R"(class Node { id: int; }
class H { n: Node; m: shared mut Node; }
fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn giveRO(r: shared Node) -> shared Node { return r; }
fn getH(h: shared mut H) -> shared mut H { return h; }
fn slot(xs: inout shared Node[2]) -> inout Node { return xs[0]; }
fn put<T>(s: inout T, v: T) { s = v; }
fn main() -> int {
  ro: shared Node = Node(1);
  a = dup<shared mut Node>(giveRO(ro));
  h: shared mut H = H(Node(2), Node(6));
  b = dup<shared mut Node>(getH(h).n);
  u: mut Node = Node(3);
  c = dup<shared Node>(if ro.id > 0 then ro else u);
  let fixed: H = H(Node(4), Node(7));
  put(fixed.m, Node(5));
  return 0;
}
)");
  expectErrors(
      r, {":10:28: error: cannot pass 'giveRO(...)' (a 'shared Node') to "
          "parameter "
          "'x' of 'dup<shared mut Node>', a 'view shared mut Node'",
          ":12:28: error: cannot pass 'getH(...).n' (a 'mut Node')",
          ":14:50: error: cannot pass 'u' (a 'mut Node') to parameter 'x' of "
          "'dup<shared Node>'",
          ":6:58: error: element 'xs[0]' is a 'shared Node', which nothing "
          "changes, so it cannot be returned 'inout'",
          ":16:7: error: 'fixed' is immutable (a 'H'; write 'mut H' to change "
          "it); its field 'm' cannot be passed to 'inout' parameter 's'"});
  auto fine = semaCheck(R"(class Node { id: int; }
fn dup<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn make() -> Node { return Node(1); }
fn main() -> int {
  a = dup<shared mut Node>(Node(1));
  b = dup<atomic shared Node>(make());
  u: mut Node = Node(2);
  c = dup<shared Node>(move u);
  return a.id + b.id + c.id;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// A form the template writes is its own: in a template's code, the clone
// of a `shared` variable of a concrete type, or of a `shared T`, is its
// class's, as in the program's code; only a type argument's form clones
// as its copy.  A trait's type argument keeps its form.
TEST(QualifiedPart, ATemplatesOwnFormsCloneByTheirClass) {
  auto r = semaCheck(R"(class Node { id: int; }
impl Cloneable for Node {}
class Box1<T> {
  item: shared T;
  view fn fresh() -> T { return self.item.clone(); }
}
fn gen<T>(x: T, s: shared mut Node) -> mut Node { return s.clone(); }
trait Src<T> { view fn count() -> int; }
class Bag<T> { item: T; }
impl Src<T> for Bag<T> { view fn count() -> int { return 1; } }
fn need<S: Src<U>, U>(s: view S) -> int { return 1; }
fn main() -> int {
  s: shared mut Node = Node(1);
  b = Box1<Node>(Node(2));
  let f: mut Node = b.fresh();
  let g = gen(0, s);
  let bag = Bag<shared mut Node>(s);
  return need<Bag<shared mut Node>, shared mut Node>(bag) + f.id + g.id;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Node { id: int; }
trait Src<T> { view fn count() -> int; }
class Bag<T> { item: T; }
impl Src<T> for Bag<T> { view fn count() -> int { return 1; } }
fn need<S: Src<U>, U>(s: view S) -> int { return 1; }
fn main() -> int {
  s: shared mut Node = Node(1);
  let bag = Bag<shared mut Node>(s);
  return need<Bag<shared mut Node>, Node>(bag);
}
)");
  expectErrors(bad, {":9:10: error: 'Bag<shared mut Node>' does not implement "
                     "trait 'Src<Node>'"});
}

// A function type holds only the forms a caller sees: `mut` is the
// function's own (`fn grow(n: mut Node)` is a `(Node) -> int`), and a
// borrow carries none.  Values whose parts differ only in their forms
// compare; a constructor takes its forms from the slot's type; and a
// literal in a conditional's branch takes the slot's forms.
TEST(QualifiedPart, FormsTheTypeDoesNotTie) {
  auto r = semaCheck(R"(class Node { id: int; }
class H<T> { v: T; }
fn grow(n: mut Node) -> int { n.id += 1; return n.id; }
fn mk() -> mut Node { return Node(1); }
fn apply(f: (Node) -> int, n: Node) -> int { return f(move n); }
fn make(g: () -> Node) -> Node { return g(); }
fn pick(c: bool, s: shared mut Node) -> (int, shared mut Node) {
  return if c then (1, s) else (2, Node(0));
}
fn main() -> int {
  let n = apply(grow, Node(1));
  let m = make(mk);
  s: shared Node = Node(2);
  let a = (1, s);
  let same = a == (1, Node(2));
  let h: H<shared Node> = H(Node(3));
  return n + m.id + h.v.id + pick(True, Node(4)).1.id;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// Where a form must be the same: an override's result and by-value
// parameters, as a caller through the base sees them; a copy of what an
// `-> inout` result names (another owner); a conditional made a cell or
// frozen whole; a `parallel for` reading a payload of references.
TEST(QualifiedPart, FormsThatMustAgree) {
  auto r = semaCheck(R"(class Node { v: int; }
class Base { fn make() -> Node { return Node(1); } }
class Der : Base {
  s: shared mut Node;
  fn make() -> shared mut Node { return self.s; }
}
class H { n: Node; fn slot() -> inout Node { return self.n; } }
enum E<T> { A(v: T), B }
fn main() -> int {
  h: mut H = H(Node(2));
  let c = h.slot();
  l: locked mut Node = Node(3);
  let m: locked mut Node = if c.v > 0 then l else Node(4);
  let e = E<shared mut Node>::A(Node(5));
  total = 0;
  parallel for i in 0..4 {
    match e { a: A { total += a.v.v; } B { } }
  }
  return 0;
}
)");
  expectErrors(
      r,
      {":5:3: error: override of 'Base.make() -> Node' takes and gives the "
       "forms "
       "it does: its result is 'shared mut Node', not 'Node'",
       ":11:3: error: cannot copy 'h.slot()' (a unique 'Node')",
       "has a branch of that form and a new value: make the new value 'locked "
       "mut' first",
       ":17:11: error: 'e' (a 'E<shared mut Node>') cannot be used in a "
       "'parallel for' body"});
}

// What the rules leave free: a type argument's form through destructuring,
// a function value's result and an inferred result; a `mut T` given a
// `shared mut` argument; arrays of tuples, slices and literals whose parts
// join; a returned local, `move` and `None` as fresh values.
TEST(QualifiedPart, FormsTheRulesLeaveFree) {
  auto r = semaCheck(R"(class Node { v: int; }
fn bump<T>(x: mut T) -> T { return x; }
fn sum<T>(xs: view T[], n: T) -> int { return 0; }
fn pair(s: shared Node) -> (int, shared Node) {
  let u = (1, Node(2));
  return u;
}
async fn make() -> Node { return Node(9); }
fn keep<T: Cloneable>(x: view T) -> T { return x.clone(); }
fn main() -> int {
  s: shared mut Node = Node(1);
  let b = bump(s);
  let n = sum([Node(1), Node(2)], b);
  let xs: (int, shared Node)[2] = [(1, Node(1)), (2, Node(2))];
  let r: shared Node = Node(3);
  let ys = [(1, r), (2, Node(4))];
  let o: (int, shared Node)? = if b.v > 0 then (1, Node(5)) else None;
  let t = make();
  made = await t;
  let k = keep<shared Node>(move made);
  return pair(r).0 + ys[1].0 + k.v + n;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// A borrow of a type argument's form is held to it everywhere: an
// override's `view T` or `-> inout T` of a base's plain borrow, a local
// `v: view T`; and a `move` takes only what it owns, never a borrowed
// result or a conditional as a whole.
TEST(QualifiedPart, ABorrowsFormHoldsEverywhere) {
  auto r = semaCheck(R"(class Node { v: int; }
class Base {
  view fn alias(x: view Node) -> int { return 0; }
}
class Copier<T> : Base {
  view fn alias(x: view T) -> int { return 1; }
}
fn poke<T: Cloneable>(n: view Node, w: T) -> T {
  v: view T = n;
  return v.clone();
}
class H { n: Node; fn slot() -> inout Node { return self.n; } }
fn main() -> int {
  let c = Copier<shared mut Node>();
  s: shared mut Node = Node(1);
  let p = poke(Node(2), s);
  h: mut H = H(Node(3));
  let m = move h.slot();
  u: mut Node = Node(4);
  w: mut Node = Node(5);
  let k = move (if s.v > 0 then u else w);
  return 0;
}
)");
  expectErrors(
      r,
      {":6:3: error: override of 'Base.alias(Node) -> int' takes and gives the "
       "forms it does: its parameter 'x' is 'shared mut Node', not 'Node'",
       ":9:15: error: cannot pass 'n' (a 'Node') to local 'v', a 'view shared "
       "mut Node': a borrow of a type argument's form takes that form",
       ":18:11: error: cannot move 'h.slot()': it is a borrow of storage its "
       "owner keeps",
       ":21:11: error: cannot move a conditional: write 'move' in each "
       "branch"});
}
