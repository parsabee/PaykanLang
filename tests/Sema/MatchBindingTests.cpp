// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Sema tests: the qualifier of a `match` binding (#368;
// docs/language/08-match.md, "Bindings"; docs/design/concurrency.md).  A
// binding holds its subject's form: a downcast of a `shared Obj` binds a
// `shared Node`, an `inout` arm of a `shared mut` subject a `shared mut`
// one.  A qualifier written on the arm is optional and must be that form;
// a borrow's arm takes none.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <string>

using namespace paykan::test;

namespace {

void expectErrors(const SemaResult &r,
                  std::initializer_list<const char *> diags) {
  EXPECT_FALSE(r.Ok);
  for (const char *diag : diags)
    EXPECT_NE(r.Diagnostics.find(diag), std::string::npos) << diag << "\n"
                                                           << r.Diagnostics;
}

} // namespace

// A downcast binding keeps its subject's form: on a `shared Base` it is a
// `shared Node`, copied out freely and read-only; on a `shared mut` one it
// is written through.  Writing the form on the arm is optional, and a
// written `mut` must be the subject's too.
TEST(MatchBinding, ABindingHoldsItsSubjectsForm) {
  auto r = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  s: shared Base = Node(1, 2);
  m: shared mut Base = Node(3, 4);
  kept: shared Node? = None;
  match s { n: Node { kept = n; } _ { } }
  match s { n: shared Node { kept = n; } _ { } }
  match m { n: Node { n.w = 5; } _ { } }
  match m { n: shared mut Node { n.w = 6; } _ { } }
  return 0;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
  auto bad = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  s: shared Base = Node(1, 2);
  match s { n: Node { n.w = 3; } _ { } }
  match s { n: atomic shared Node { } _ { } }
  match s { n: shared mut Node { } _ { } }
  o: shared mut Node? = Node(4, 5);
  match o { n: view shared mut Node { } None { } }
  b: Base = Node(6, 7);
  match b { n: mut Node { } _ { } }
  return 0;
}
)");
  expectErrors(
      bad, {":5:23: error: 'n' is immutable (a 'shared Node'; write 'shared "
            "mut Node' to change it)",
            ":6:13: error: 'n' binds part of a 'shared Base': its type is "
            "'shared Node', not 'atomic shared Node'",
            ":7:13: error: 'n' binds part of a 'shared Base': its type is "
            "'shared Node', not 'shared mut Node'",
            ":9:13: error: 'view' arm 'n' takes no qualifier ('shared mut'): "
            "a borrow binds the subject whatever its form",
            ":11:13: error: 'n' binds part of a 'Base': its type is 'Node', "
            "not 'mut Node'"});
}

// An `inout` arm of a `shared mut` subject names storage of that form, so
// `mem::replace` through it trades `shared mut` references and never hands
// the object out as a unique one.
TEST(MatchBinding, AnInoutArmIsHeldAsItsSubject) {
  auto r = semaCheck(R"(import ::memory as mem;
class Node { v: int; }
fn main() -> int {
  o: shared mut Node? = Node(1);
  match o {
    n: inout Node {
      let back: mut Node = mem::replace(n, Node(2));
    }
    None { }
  }
  return 0;
}
)");
  expectErrors(r, {"error: cannot store a 'shared mut Node' in variable "
                   "'back', a 'mut Node'"});
  auto fine = semaCheck(R"(import ::memory as mem;
class Node { v: int; }
fn main() -> int {
  o: shared mut Node? = Node(1);
  match o {
    n: inout Node {
      let back = mem::replace(n, Node(2));
      back.v = 3;
    }
    None { }
  }
  return 0;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// In a template's code a binding of a `view T?` subject with T = `shared
// mut Node` holds that form, so its clone is the copy, the same object.
TEST(MatchBinding, ATemplatesBindingKeepsItsTypeArgumentsForm) {
  auto r = semaCheck(R"(class Node { v: int; }
fn first<T: Cloneable>(o: view T?) -> T? {
  match o { v: T { return v.clone(); } None { return None; } }
}
fn main() -> int {
  s: shared mut Node = Node(1);
  o: shared mut Node? = s;
  match first<shared mut Node>(o) { n: Node { n.v = 2; } None { } }
  return s.v;
}
)");
  EXPECT_TRUE(r.Ok) << r.Diagnostics;
}

// An arm without a binding takes a qualifier only in its subject's form,
// as a binding's arm does.
TEST(MatchBinding, AnArmWithoutABindingIsInItsSubjectsForm) {
  auto r = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  s: shared Base = Node(1, 2);
  match s { atomic shared Node { } _ { } }
  o: shared Node? = Node(3, 4);
  match o { locked mut Node { } None { } }
  match s { shared Node { } _ { } }
  match o { shared Node { } None { } }
  return 0;
}
)");
  expectErrors(r, {":5:13: error: the arm matches part of a 'shared Base': its "
                   "type is 'shared Node', not 'atomic shared Node'",
                   ":7:13: error: the arm matches part of a 'shared Node?': "
                   "its type is 'shared Node', not 'locked mut Node'"});
  EXPECT_EQ(r.Diagnostics.find(":8:"), std::string::npos) << r.Diagnostics;
  EXPECT_EQ(r.Diagnostics.find(":9:"), std::string::npos) << r.Diagnostics;
}

// A cell's contents are reached in a `lock` block: a type arm's class test
// on a `locked` subject reads them, and so does a field write through one
// (both tested or wrote the cell).  An optional cell's arm naming its own
// type binds the cell, a handle that a `lock` block opens.
TEST(MatchBinding, ACellsContentsAreMatchedInALockBlock) {
  auto r = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  let c: locked mut Base = Node(5, 6);
  match c { n: Node { } _ { } }
  o: locked mut Base? = c;
  match o { n: Node { } _ { } }
  let d: locked mut Node = Node(7, 8);
  d.w = 9;
  p: locked mut Node? = d;
  match p { n: Node { n.w = 9; } None { } }
  return 0;
}
)");
  expectErrors(
      r, {":5:13: error: 'c' is 'locked': reach its contents in 'lock c as c: "
          "view { }' (or 'inout')",
          ":7:13: error: 'o' is 'locked': reach its contents in 'lock o",
          ":9:3: error: 'd' is 'locked': reach its contents in 'lock d",
          ":11:23: error: 'n' is 'locked': reach its contents in 'lock n"});
  auto fine = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  let c: locked mut Base = Node(5, 6);
  lock c as v: inout {
    match v { n: inout Node { n.w = 7; } _ { } }
  }
  o: locked mut Base? = c;
  match o {
    b: Base { lock b as v: view { match v { n: Node { } _ { } } } }
    None { }
  }
  return 0;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
}

// A conditional's branches are each held as its binding is: two forms are
// refused (a frozen object bound 'shared mut', a unique place 'shared'),
// a new object or a 'None' joins.  A binding of storage, a conditional of
// places or an 'inout' result, is a borrow of it, never a copy; an 'inout'
// result's storage takes a written 'mut'.  A 'locked Str' is compared in a
// 'lock' block, and a refused arm's binding reports nothing more.
TEST(MatchBinding, AConditionalOrAnInoutResultIsBoundAsItsStorage) {
  auto r = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
class H { b: mut Base; fn slot() -> inout Base { return self.b; } }
fn main() -> int {
  let sm: shared mut Base = Node(1, 2);
  let fz: atomic shared Base = Node(3, 4);
  let u: mut Base = Node(5, 6);
  let u2: mut Base = Node(7, 8);
  let s: shared Base = Node(9, 10);
  h: mut H = H(Node(11, 12));
  c = True;
  owned: Node? = None;
  match (if c then sm else fz) { n: Node { } _ { } }
  match (if c then u else s) { n: Node { } _ { } }
  match (if c then u else u2) { n: Node { owned = move n; } _ { } }
  match h.slot() { n: Node { owned = move n; } _ { } }
  match h.slot() { n: mut Node { } _ { } }
  match (if c then s else Node(13, 14)) { n: Node { } _ { } }
  match (if c then sm else None) { n: Node { n.w = 1; } _ { } }
  let ls: locked mut Str = "a";
  match ls { "a" { } _ { } }
  let lc: locked mut Base = Node(15, 16);
  match lc { n: Node { n.w = 2; n.v = 3; } _ { } }
  return 0;
}
)");
  expectErrors(
      r, {":13:34: error: 'n' binds part of a conditional whose branches have "
          "two forms, 'shared mut Base' and 'atomic shared Base': a binding "
          "holds one",
          ":14:32: error: 'n' binds part of a conditional whose branches have "
          "two forms, 'shared Base' and 'mut Base': a binding holds one",
          ":15:51: error: cannot move 'n': it is a 'view', a borrow",
          ":16:38: error: cannot move 'n': it is a 'view', a borrow",
          ":21:3: error: 'ls' is 'locked': reach its contents in 'lock ls",
          ":23:14: error: 'lc' is 'locked': reach its contents in 'lock lc"});
  for (const char *line : {":17:", ":18:", ":19:", ":23:2", ":23:3"})
    EXPECT_EQ(r.Diagnostics.find(line), std::string::npos) << line << "\n"
                                                           << r.Diagnostics;
}

// A conditional reaches each branch's contents, so a `locked` branch in any
// place is refused as the cell alone is; a conditional of `inout` results
// and `mut` places takes a written `mut`, and one with a `None` branch does
// not, its binding being immutable.
TEST(MatchBinding, AConditionalsCellBranchIsReachedInALockBlock) {
  auto r = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
class W { b: mut Base; }
fn main() -> int {
  let fz: atomic shared Base = Node(1, 2);
  let lc: locked mut Base = Node(3, 4);
  let wsm: shared mut W = W(Node(5, 6));
  let lw: locked mut W = W(Node(7, 8));
  c = True;
  match (if c then fz else lc) { Node { } _ { } }
  match (if c then wsm else lw).b { n: Node { } _ { } }
  let ls: locked mut Str = "a";
  let s: shared Str = "b";
  match (if c then s else ls) { "a" { } _ { } }
  return 0;
}
)");
  expectErrors(r, {":10:34: error: 'lc' is 'locked'",
                   ":11:9: error: 'lw' is 'locked'",
                   ":14:3: error: 'ls' is 'locked'"});
  auto fine = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
class H { b: mut Base; fn slot() -> inout Base { return self.b; } }
fn main() -> int {
  h: mut H = H(Node(1, 2));
  h2: mut H = H(Node(3, 4));
  u: mut Base = Node(5, 6);
  c = True;
  match (if c then h.slot() else u) { n: mut Node { } _ { } }
  match (if c then h.slot() else h2.slot()) { n: mut Node { } _ { } }
  return 0;
}
)");
  EXPECT_TRUE(fine.Ok) << fine.Diagnostics;
  auto none = semaCheck(R"(class Base { v: int; }
class Node : Base { w: int; }
fn mk(i: int) -> mut Base { return Node(i, i); }
fn main() -> int {
  c = True;
  match (if c then mk(1) else None) { n: mut Node { n.w = 8; } _ { } }
  match (if c then mk(1) else mk(2)) { n: mut Node { n.w = 8; } _ { } }
  return 0;
}
)");
  expectErrors(none, {":6:39: error: 'n' binds part of a 'Base?': its type is "
                      "'Node', not 'mut Node'"});
  EXPECT_EQ(none.Diagnostics.find(":7:"), std::string::npos)
      << none.Diagnostics;
}
