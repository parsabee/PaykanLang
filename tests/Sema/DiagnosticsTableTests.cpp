// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The diagnostics of the ownership and concurrency model, each with a
// program that gives it (#368): the table of docs/design/concurrency.md,
// "Diagnostics".  A message reworded in the compiler fails the first test
// until its row here says the same, and the second until the document does.

#include "TestUtils.h"
#include <gtest/gtest.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace paykan::test;

namespace {

/// Who reports the row: the parser, or the semantic analysis.
enum class By { Parser, Sema };

struct Row {
  const char *Situation; ///< the document's row
  By Stage;
  const char *Kind; ///< "error" or "warning"
  const char *Message;
  const char *Program;
};

// clang-format off
const Row kRows[] = {
    {"copy of a unique place",
     By::Sema, "error",
     "cannot copy 'a' (a unique 'Node'); write 'move a' to transfer it, or declare it 'shared Node'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let a = Node(1);
  let b = a;
  return 0;
}
)pkn"},
    {"copy of a task handle",
     By::Sema, "error",
     "cannot copy 't' (a 'Task<int>'); write 'move t' to transfer it",
     R"pkn(async fn fib(n: int) -> int { return n; }
async fn main() -> int {
  let t = fib(5);
  let u = t;
  return await u;
}
)pkn"},
    {"copy of a value that holds a unique part (a struct, a tuple, an array)",
     By::Sema, "error",
     "cannot copy 'p': 'Person' holds unique field 'name' ('Str'); write 'move p' to transfer it, or 'p.clone()' to copy it",
     R"pkn(struct Person { name: Str; age: int; }
fn main() -> int {
  let p = Person("a", 1);
  let q = p;
  return 0;
}
)pkn"},
    {"copy of a value that holds a unique part (a struct, a tuple, an array)",
     By::Sema, "error",
     "cannot copy 't': '(int, Node)' holds unique element '.1' ('Node'); write 'move t' to transfer it",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let t = (1, Node(1));
  let u = t;
  return 0;
}
)pkn"},
    {"copy of a value that holds a unique part (a struct, a tuple, an array)",
     By::Sema, "error",
     "cannot copy 't': 'Node[2]' holds unique elements ('Node'); write 'move t' to transfer it",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let t = [Node(1), Node(2)];
  let u = t;
  return 0;
}
)pkn"},
    {"copy of a borrow (a `view` parameter, a `view` local, an `inout` parameter, an `inout` local)",
     By::Sema, "error",
     "'n' is a 'view' parameter; it cannot be stored, only read or passed to a 'view' parameter",
     R"pkn(class Node { key: int; }
fn f(n: view Node) -> int {
  let b = n;
  return 0;
}
fn main() -> int { return 0; }
)pkn"},
    {"copy of a borrow (a `view` parameter, a `view` local, an `inout` parameter, an `inout` local)",
     By::Sema, "error",
     "'v' is a 'view' local; it cannot be stored, only read or passed to a 'view' parameter",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let a = Node(1);
  v: view = a;
  let b = v;
  return 0;
}
)pkn"},
    {"copy of a borrow (a `view` parameter, a `view` local, an `inout` parameter, an `inout` local)",
     By::Sema, "error",
     "cannot copy 'n' (a unique 'mut Node'); it is an 'inout' borrow, and what it names stays with its owner",
     R"pkn(class Node { key: int; }
fn f(n: inout Node) -> int {
  let b = n;
  return 0;
}
fn main() -> int { return 0; }
)pkn"},
    {"copy of a borrow (a `view` parameter, a `view` local, an `inout` parameter, an `inout` local)",
     By::Sema, "error",
     "cannot copy 'x' (a unique 'mut Node'); it is an 'inout' borrow, and what it names stays with its owner",
     R"pkn(class Node { key: int; }
fn main() -> int {
  a: mut = Node(1);
  x: inout = a;
  let b = x;
  return 0;
}
)pkn"},
    {"`[v; n]` of a place",
     By::Sema, "error",
     "'[v; n]' evaluates its value for each of its 3 elements, and 'node' (a 'Node') is not Copyable: write a value made for each",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let node = Node(1);
  let arr = [node; 3];
  return 0;
}
)pkn"},
    {"use after `move`",
     By::Sema, "error",
     "'a' was moved at 5:8; it cannot be used after the move",
     R"pkn(class Node { key: int; }
fn take(n: Node) -> int { return n.key; }
fn main() -> int {
  let a = Node(1);
  take(move a);
  return a.key;
}
)pkn"},
    {"use after a `move` on some paths",
     By::Sema, "error",
     "'a' may have been moved (by the 'if' at 5:3, at 5:23); it cannot be used after a move",
     R"pkn(class Node { key: int; }
fn take(n: Node) -> int { return n.key; }
fn main() -> int {
  let a = Node(1);
  if a.key > 3 { take(move a); }
  return a.key;
}
)pkn"},
    {"`move` in a loop",
     By::Sema, "error",
     "'job' is moved in the loop at 5:3 and not assigned again before the next iteration",
     R"pkn(class Node { key: int; }
fn take(n: Node) -> int { return n.key; }
fn main() -> int {
  let job = Node(1);
  for i in 0..2 { take(move job); }
  return 0;
}
)pkn"},
    {"capture of a unique variable",
     By::Sema, "error",
     "cannot capture 'job' (a unique 'Job'): a function captures by copy; declare it 'shared Job' (and then, to keep no owner, capture a 'weak' local of it)",
     R"pkn(class Job { name: Str; }
fn main() -> int {
  let job = Job("j");
  let f = fn() -> int { return job.name.len(); };
  return f();
}
)pkn"},
    {"`move` of a captured variable",
     By::Sema, "error",
     "cannot move 'job': the function captures it by copy",
     R"pkn(class Job { name: Str; }
fn keep(j: shared Job) -> int { return j.name.len(); }
fn main() -> int {
  let job: shared = Job("j");
  let f = fn() -> int { n = job.name.len(); return n + keep(move job); };
  return f();
}
)pkn"},
    {"copy of a part (an array's element, an optional one, a tuple's element, a field of an owned variable, of an `inout` borrow)",
     By::Sema, "error",
     "cannot copy 'xs[0]' (a unique 'mut Node'); an element leaves its array by 'mem::replace'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  xs = [Node(1), Node(2)];
  let a = xs[0];
  return 0;
}
)pkn"},
    {"copy of a part (an array's element, an optional one, a tuple's element, a field of an owned variable, of an `inout` borrow)",
     By::Sema, "error",
     "cannot copy 'xs[0]' (a unique 'mut Node?'); an element leaves its array by 'mem::replace' or 'mem::take'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  xs: Node?[2] = [Node(1), None];
  let a = xs[0];
  return 0;
}
)pkn"},
    {"copy of a part (an array's element, an optional one, a tuple's element, a field of an owned variable, of an `inout` borrow)",
     By::Sema, "error",
     "cannot copy 't.0' (a unique 'mut Node'): an element leaves its tuple only with the others; destructure it ('a, b = move t;'), or declare the element 'shared mut Node'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  t = (Node(1), 2);
  let a = t.0;
  return 0;
}
)pkn"},
    {"copy of a part (an array's element, an optional one, a tuple's element, a field of an owned variable, of an `inout` borrow)",
     By::Sema, "error",
     "cannot copy 'h.node' (a unique 'Node'); write 'move h.node' to take it (which ends 'h'), or declare the field 'shared Node'",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn main() -> int {
  let h = Holder(Node(1));
  let n = h.node;
  return 0;
}
)pkn"},
    {"copy of a part (an array's element, an optional one, a tuple's element, a field of an owned variable, of an `inout` borrow)",
     By::Sema, "error",
     "cannot copy 'h.node' (a unique 'mut Node'); write 'mem::replace(h.node, v)' to take it in exchange, or declare the field 'shared mut Node'",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn f(h: inout Holder) -> int {
  let n = h.node;
  return 0;
}
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'self.node': 'move' takes a variable, or a field of one it owns; write 'mem::replace(self.node, v)'",
     R"pkn(class Node { key: int; }
class Holder {
  node: Node;
  fn take() -> int { return keep(move self.node); }
}
fn keep(n: Node) -> int { return n.key; }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.node': 'h' is a 'view' borrow, which changes nothing",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn f(h: view Holder) -> int { return keep(move h.node); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.node': 'h' is an 'inout' borrow, and what it names stays whole; write 'mem::replace(h.node, v)'",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn f(h: inout Holder) -> int { return keep(move h.node); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.sh.n': 'h' is an 'inout' borrow, and what it names stays whole",
     R"pkn(class Node { key: int; }
class Inner { n: Node; }
class Holder { sh: shared Inner; }
fn keep(n: Node) -> int { return n.key; }
fn take(h: inout Holder) -> int { return keep(move h.sh.n); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'w.node': 'w' is a 'weak' reference, which owns nothing",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  h: shared = Holder(Node(1));
  w: weak Holder? = h;
  return keep(move w.node);
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.node': 'h' is a 'shared Holder', which other references reach",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  h: shared = Holder(Node(1));
  let k = fn() -> int { return keep(move h.node); };
  return k();
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.node': 'h' is a 'shared mut Holder', which other references reach; write 'mem::replace(h.node, v)'",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  h: shared mut = Holder(Node(1));
  return keep(move h.node);
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'h.node': it is 'shared Node', not a unique part of 'h'",
     R"pkn(class Node { key: int; }
class Holder { node: shared Node; }
fn keep(n: shared Node) -> int { return n.key; }
fn main() -> int {
  let h = Holder(Node(1));
  return keep(move h.node);
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'make().node': 'move' takes a variable, or a field of one it owns",
     R"pkn(class Node { key: int; }
class Holder { node: Node; }
fn keep(n: Node) -> int { return n.key; }
fn make() -> Holder { return Holder(Node(1)); }
fn main() -> int {
  return keep(move make().node);
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 'xs[0]': an element leaves by 'mem::replace' (or 'mem::take', where it is optional)",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  xs = [Node(1)];
  return keep(move xs[0]);
}
)pkn"},
    {"`move` of a part (a field of `self`, of a `view` borrow, of an `inout` borrow, a longer path through one, of a `weak` reference, of a `shared` handle, of a `shared mut` one, a `shared` field, of a temporary, an array's element, a tuple's element)",
     By::Sema, "error",
     "cannot move 't.0': an element leaves its tuple only with the others; destructure it ('a, b = move t;')",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  t = (Node(1), 2);
  return keep(move t.0);
}
)pkn"},
    {"`move self`",
     By::Sema, "error",
     "cannot move 'self': a method borrows its receiver",
     R"pkn(class Job {
  name: Str;
  fn me() -> int { return keep(move self); }
}
fn keep(j: Job) -> int { return j.name.len(); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a borrow (a `view`, an `inout` parameter, an `inout` local, a `lock` block's binding)",
     By::Sema, "error",
     "cannot move 'n': it is a 'view' borrow, and what it names stays with its owner",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn f(n: view Node) -> int { return keep(move n); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a borrow (a `view`, an `inout` parameter, an `inout` local, a `lock` block's binding)",
     By::Sema, "error",
     "cannot move 'n': it is an 'inout' borrow, and what it names stays with its owner",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn f(n: inout Node) -> int { return keep(move n); }
fn main() -> int { return 0; }
)pkn"},
    {"`move` of a borrow (a `view`, an `inout` parameter, an `inout` local, a `lock` block's binding)",
     By::Sema, "error",
     "cannot move 'x': it is an 'inout' borrow, and what it names stays with its owner",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  a: mut = Node(1);
  x: inout = a;
  return keep(move x);
}
)pkn"},
    {"`move` of a borrow (a `view`, an `inout` parameter, an `inout` local, a `lock` block's binding)",
     By::Sema, "error",
     "cannot move 'c': it is an 'inout' borrow, and what it names stays with its owner",
     R"pkn(class Counts { total: int; }
fn keep(n: Counts) -> int { return n.total; }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  r = 0;
  lock counts as c: inout {
    r = keep(move c);
  }
  return r;
}
)pkn"},
    {"`move` of a `weak` variable",
     By::Sema, "error",
     "cannot move 'w': a 'weak' variable holds no strong reference",
     R"pkn(class Node { key: int; }
fn keep(n: Node?) -> int { return 0; }
fn main() -> int {
  a: shared = Node(1);
  w: weak Node? = a;
  return keep(move w);
}
)pkn"},
    {"`move`, or any use, of what a local borrows",
     By::Sema, "error",
     "'l' is viewed by 'view' local 'x' until 'x' is last used; it cannot be moved",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  let l = Node(1);
  x: view = l;
  n = keep(move l);
  return n + x.key;
}
)pkn"},
    {"`move`, or any use, of what a local borrows",
     By::Sema, "error",
     "'l' is borrowed by 'inout' local 'x' until 'x' is last used; it cannot be used before then",
     R"pkn(class Node { key: int; }
fn keep(n: Node) -> int { return n.key; }
fn main() -> int {
  l: mut = Node(1);
  x: inout = l;
  n = keep(move l);
  x.key = 2;
  return n;
}
)pkn"},
    {"an argument borrowed, and passed `inout` or moved by a later argument of the call",
     By::Sema, "error",
     "'s' is passed to 'view' parameter 'a' of 'both'; a later argument of 'both' cannot pass it to 'inout' parameter 'x' of 'shout'",
     R"pkn(fn shout(x: inout Str) -> Str { x.concat("!"); return "b"; }
fn both(a: view Str, b: Str) -> int { return a.len() + b.len(); }
fn main() -> int {
  s: mut = "a";
  return both(s, shout(s));
}
)pkn"},
    {"an argument borrowed, and passed `inout` or moved by a later argument of the call",
     By::Sema, "error",
     "'j' is passed to 'view' parameter 'j'; it cannot also be moved in argument 2",
     R"pkn(class Job { n: int; }
fn keep(j: Job) -> int { return j.n; }
fn look(j: view Job, k: int) -> int { return j.n + k; }
fn main() -> int {
  let j = Job(1);
  return look(j, keep(move j));
}
)pkn"},
    {"an argument borrowed, and passed `inout` or moved by a later argument of the call",
     By::Sema, "error",
     "'j' is passed to 'view' parameter 'j'; it cannot also be moved into parameter 'k'",
     R"pkn(class Job { n: int; }
fn look(j: view Job, k: Job) -> int { return j.n + k.n; }
fn main() -> int {
  let j = Job(1);
  return look(j, move j);
}
)pkn"},
    {"an argument borrowed, and passed `inout` or moved by a later argument of the call",
     By::Sema, "error",
     "'j' is the receiver of 'view fn plus'; it cannot also be moved in argument 1",
     R"pkn(class Job { n: int; view fn plus(k: int) -> int { return self.n + k; } }
fn keep(j: Job) -> int { return j.n; }
fn main() -> int {
  let j = Job(1);
  return j.plus(keep(move j));
}
)pkn"},
    {"qualifier on a value",
     By::Sema, "error",
     "'shared' qualifies a reference; 'int' is a value",
     R"pkn(fn main() -> int {
  let f: shared int = 2;
  return 0;
}
)pkn"},
    {"qualifier on a value",
     By::Sema, "error",
     "'shared' qualifies a reference; 'int[3]' is a value",
     R"pkn(fn main() -> int {
  let p: shared int[3] = [1, 2, 3];
  return 0;
}
)pkn"},
    {"qualifier on a value",
     By::Sema, "error",
     "'atomic shared' qualifies a reference; 'int' is a value",
     R"pkn(fn main() -> int {
  let k: atomic shared int = 1;
  return 0;
}
)pkn"},
    {"qualifier on a struct, tuple or array that holds references",
     By::Sema, "error",
     "'atomic shared' qualifies a reference; 'Pair' is a value: box it ('mem::Box<Pair>') to share what it holds",
     R"pkn(class Node { key: int; }
struct Pair { left: Node; n: int; }
fn main() -> int {
  let p: atomic shared Pair = Pair(Node(6), 1);
  return 0;
}
)pkn"},
    {"qualifier on a struct, tuple or array that holds references",
     By::Sema, "error",
     "'atomic shared' qualifies a reference; 'Node[3]' is a value: hold it in a class to share what it holds",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let p: atomic shared Node[3] = [Node(1), Node(2), Node(3)];
  return 0;
}
)pkn"},
    {"`mut` on a value",
     By::Sema, "error",
     "'mut' applies to objects; 'int' is a value: its storage is rewritten by assignment, which 'let' fixes",
     R"pkn(fn main() -> int {
  let k: mut int = 1;
  return 0;
}
)pkn"},
    {"`mut` on a value",
     By::Sema, "error",
     "'mut' applies to objects; 'Pair' is a value: its storage is rewritten by assignment, which 'let' fixes",
     R"pkn(class Node { key: int; }
struct Pair { left: Node; n: int; }
fn main() -> int {
  let v: mut Pair = Pair(Node(5), 1);
  return 0;
}
)pkn"},
    {"qualifier on a function value",
     By::Sema, "error",
     "a function value takes no qualifier ('mut () -> int'): it is Copyable, and its copies share its closure",
     R"pkn(fn main() -> int {
  let f: mut () -> int = fn() -> int { return 1; };
  return 0;
}
)pkn"},
    {"two qualifiers, or `mut` first (the parser)",
     By::Parser, "error",
     "a type takes one qualifier: 'locked atomic' is two",
     R"pkn(class Config { key: int; }
fn main() -> int {
  let c: locked atomic shared Config = Config(1);
  return 0;
}
)pkn"},
    {"two qualifiers, or `mut` first (the parser)",
     By::Parser, "error",
     "a type takes one qualifier: 'shared mut shared' is two",
     R"pkn(class Config { key: int; }
fn main() -> int {
  let c: shared mut shared Config = Config(1);
  return 0;
}
)pkn"},
    {"two qualifiers, or `mut` first (the parser)",
     By::Parser, "error",
     "'mut shared' is spelled 'shared mut': the form first, then the writes",
     R"pkn(class Config { key: int; }
fn main() -> int {
  let c: mut shared Config = Config(1);
  return 0;
}
)pkn"},
    {"`mut` on a frozen object or a cell (the parser)",
     By::Parser, "error",
     "'atomic shared mut T' would be written from several threads with no lock; write 'atomic shared T' to freeze it, or 'locked mut T' to change it under a lock",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let c: atomic shared mut Node = Node(1);
  return 0;
}
)pkn"},
    {"`mut` on a frozen object or a cell (the parser)",
     By::Parser, "error",
     "an 'atomic' cell takes no 'mut': it changes by its operations; an object crosses threads as 'atomic shared T' (frozen) or 'locked mut T'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let c: atomic mut Node = Node(1);
  return 0;
}
)pkn"},
    {"`locked T` without `mut`: a warning where it is declared, an error at an `inout` block",
     By::Sema, "warning",
     "'locked Node' locks an object nothing writes; 'atomic shared Node' is read from every thread with no lock",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let x: locked Node = Node(1);
  return 0;
}
)pkn"},
    {"`locked T` without `mut`: a warning where it is declared, an error at an `inout` block",
     By::Sema, "error",
     "'x' is 'locked Node', whose contents are never written; declare it 'locked mut Node' for a 'lock ... as v: inout' block",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let x: locked Node = Node(1);
  lock x as v: inout { v.key = 2; }
  return 0;
}
)pkn"},
    {"what a cell holds",
     By::Sema, "error",
     "'atomic' holds a number, a 'bool', a 'char' or a fixed array of them, not 'Node': an object crosses threads as 'atomic shared Node' (frozen) or 'locked mut Node'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let o: atomic Node = Node(1);
  return 0;
}
)pkn"},
    {"what a cell holds",
     By::Sema, "error",
     "'locked mut' holds an object for now, not 'int': wrap the value in a class",
     R"pkn(fn main() -> int {
  let c: locked mut int = 1;
  return 0;
}
)pkn"},
    {"what a cell holds",
     By::Sema, "error",
     "field 'c' in struct 'P' has type 'atomic int': a struct cannot hold an 'atomic' field yet (hold the cell in a class)",
     R"pkn(struct P {
  c: atomic int;
}
fn main() -> int { return 0; }
)pkn"},
    {"qualifier on a borrow",
     By::Sema, "error",
     "parameter 'n' is 'inout mut': a borrow carries no qualifier; a 'view' takes every form of the object (read-only through it) and an 'inout' a 'mut' or 'shared mut' one; write it without 'mut'",
     R"pkn(class Node { key: int; }
fn f(n: inout mut Node) -> int { return 0; }
fn main() -> int { return 0; }
)pkn"},
    {"qualifier on a borrow",
     By::Sema, "error",
     "parameter 'n' is 'view mut': a borrow carries no qualifier; a 'view' takes every form of the object (read-only through it) and an 'inout' a 'mut' or 'shared mut' one; write it without 'mut'",
     R"pkn(class Node { key: int; }
fn f(n: view mut Node) -> int { return 0; }
fn main() -> int { return 0; }
)pkn"},
    {"qualifier on a type argument that has a form already",
     By::Sema, "error",
     "'locked mut' cannot qualify the type argument 'shared Job': a type takes one qualifier, and 'locked mut' and 'shared' are two",
     R"pkn(class Job { n: int; }
fn hold<T>(x: locked mut T) -> int { return 0; }
fn main() -> int {
  let j: shared = Job(1);
  return hold<shared Job>(j);
}
)pkn"},
    {"qualifier on a type argument that has a form already",
     By::Sema, "error",
     "'mut' cannot qualify the type argument 'shared Job': a 'shared' reference is never written through",
     R"pkn(class Job { n: int; }
fn hold<T>(x: mut T) -> int { return 0; }
fn main() -> int {
  let j: shared = Job(1);
  return hold<shared Job>(j);
}
)pkn"},
    {"qualifier on a type argument that has a form already",
     By::Sema, "error",
     "'locked mut' cannot qualify a type argument yet: a cell's contents are reached in a 'lock' block, which a template's code does not write",
     R"pkn(class Job { n: int; }
fn hold<T>(x: view T) -> int { return 0; }
fn main() -> int {
  let j: locked mut = Job(1);
  return hold<locked mut Job>(j);
}
)pkn"},
    {"conversion between forms",
     By::Sema, "error",
     "cannot store a 'shared Node' in variable 'u', a unique reference: a 'shared' reference is stored only as 'shared'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let s: shared = Node(1);
  let u: Node = s;
  return 0;
}
)pkn"},
    {"conversion between forms",
     By::Sema, "error",
     "cannot store a 'shared Node' in variable 'u', an 'atomic shared' reference: a 'shared' reference is stored only as 'shared'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let s: shared = Node(1);
  let u: atomic shared Node = s;
  return 0;
}
)pkn"},
    {"conversion between forms",
     By::Sema, "error",
     "cannot store a 'shared Node' in variable 'u', a 'shared mut' reference: a 'shared' reference is stored only as 'shared'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let s: shared = Node(1);
  let u: shared mut Node = s;
  return 0;
}
)pkn"},
    {"conversion between forms",
     By::Sema, "error",
     "cannot store a 'shared mut Node' in variable 'u', a 'shared' reference: a 'shared mut' reference is stored only as 'shared mut'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let s: shared mut = Node(1);
  let u: shared Node = s;
  return 0;
}
)pkn"},
    {"conversion between forms",
     By::Sema, "error",
     "cannot store an 'atomic shared Node' in variable 'u', a unique reference: an 'atomic shared' reference is stored only as 'atomic shared'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let f: atomic shared = Node(1);
  let u: Node = f;
  return 0;
}
)pkn"},
    {"gaining `mut`",
     By::Sema, "error",
     "cannot store a 'Node' in variable 'm', a 'mut' reference: a reference gains 'mut' only by 'move'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let c = Node(1);
  let m: mut Node = c;
  return 0;
}
)pkn"},
    {"a `weak` reference to a unique object",
     By::Sema, "error",
     "cannot store 'a' (a 'Node') in variable 'w', a 'weak Node?': a 'weak' reference names a 'shared' object; declare 'a' 'shared Node'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let a = Node(1);
  w: weak Node? = a;
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'Config'; write 'mut Config' to change it); cannot assign to its field 'path'",
     R"pkn(class Config { path: Str; }
fn main() -> int {
  cfg = Config("p");
  cfg.path = "x";
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'Config'; write 'mut Config' to change it); 'rename' is not a 'view fn'",
     R"pkn(class Config {
  path: Str;
  fn rename(n: Str) { self.path = move n; }
}
fn main() -> int {
  cfg = Config("p");
  cfg.rename("y");
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'Config'; write 'mut Config' to change it); it cannot be passed to 'inout' parameter 'p'",
     R"pkn(class Config { path: Str; }
fn mark(p: inout Config) { }
fn main() -> int {
  cfg = Config("p");
  mark(cfg);
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'Config'; write 'mut Config' to change it); cannot assign to its elements",
     R"pkn(class Config { xs: int[2]; }
fn main() -> int {
  cfg = Config([1, 2]);
  cfg.xs[0] = 1;
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'Config'; write 'mut Config' to change it); cannot assign to 'cfg.limits.hi'",
     R"pkn(class Limits { lo: int; hi: int; }
class Config { limits: Limits; }
fn main() -> int {
  cfg = Config(Limits(0, 1));
  cfg.limits.hi = 5;
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "field 'lim' is immutable (a 'shared Limits'; write 'shared mut Limits' to change it); cannot assign to 'cfg.lim.lo'",
     R"pkn(class Limits { lo: int; }
class Config { lim: shared Limits; }
fn main() -> int {
  cfg: mut = Config(Limits(0));
  cfg.lim.lo = 1;
  return 0;
}
)pkn"},
    {"write through an immutable reference (a field, a method that is no `view fn`, an `inout` argument, an element, a nested field, a `shared` field, a `shared` reference)",
     By::Sema, "error",
     "'cfg' is immutable (a 'shared Config'; write 'shared mut Config' to change it); cannot assign to its field 'path'",
     R"pkn(class Config { path: Str; }
fn main() -> int {
  let cfg: shared = Config("p");
  cfg.path = "x";
  return 0;
}
)pkn"},
    {"write through a frozen object",
     By::Sema, "error",
     "'cfg' is frozen (an 'atomic shared Config'); cannot assign to its field 'path'",
     R"pkn(class Config { path: Str; }
fn main() -> int {
  let cfg: atomic shared = Config("p");
  cfg.path = "x";
  return 0;
}
)pkn"},
    {"write through a frozen object",
     By::Sema, "error",
     "'cfg' is frozen (an 'atomic shared Config'); 'rename' is not a 'view fn'",
     R"pkn(class Config {
  path: Str;
  fn rename(n: Str) { self.path = move n; }
}
fn main() -> int {
  let cfg: atomic shared = Config("p");
  cfg.rename("y");
  return 0;
}
)pkn"},
    {"write through a frozen object",
     By::Sema, "error",
     "'cfg' is frozen (an 'atomic shared Config'); it cannot be passed to 'inout' parameter 'p'",
     R"pkn(class Config { path: Str; }
fn setp(p: inout Str) { }
fn main() -> int {
  let cfg: atomic shared = Config("p");
  setp(cfg.path);
  return 0;
}
)pkn"},
    {"write through a frozen object",
     By::Sema, "error",
     "cannot move 'cfg.path': 'cfg' is an 'atomic shared Config', which other references reach",
     R"pkn(class Config { path: Str; }
fn main() -> int {
  let cfg: atomic shared = Config("p");
  let n = move cfg.path;
  return 0;
}
)pkn"},
    {"call of a `shared fn` through another form",
     By::Sema, "error",
     "'add' is a 'shared fn', which takes its receiver as a 'shared mut Node'; 'root' is a 'Node': declare it 'shared mut Node'",
     R"pkn(class Node {
  n: int;
  shared fn add() -> shared mut Node { self.n = self.n + 1; return self; }
}
fn main() -> int {
  let root = Node(1);
  root.add();
  return 0;
}
)pkn"},
    {"call of a `shared fn` through another form",
     By::Sema, "error",
     "'add' is a 'shared fn', which takes its receiver as a 'shared mut Node'; 'self' is borrowed in 'reset': make it a 'shared fn' too",
     R"pkn(class Node {
  n: int;
  shared fn add() -> shared mut Node { self.n = self.n + 1; return self; }
  fn reset() { self.add(); }
}
fn main() -> int {
  return 0;
}
)pkn"},
    {"call of a `shared fn` through another form",
     By::Sema, "error",
     "'peek' is a 'shared view fn', which takes its receiver as a 'shared Config'; 'cfg' is an 'atomic shared Config': declare it 'shared Config'",
     R"pkn(class Config {
  path: Str;
  shared view fn peek() -> int { return 1; }
}
fn main() -> int {
  let cfg: atomic shared = Config("p");
  return cfg.peek();
}
)pkn"},
    {"override of a `shared fn`, or by one",
     By::Sema, "error",
     "override of 'add' must be a 'shared fn', like the method it overrides (a call of either takes its receiver as the overridden method says)",
     R"pkn(class Node {
  n: int;
  shared fn add() -> shared mut Node { self.n = self.n + 1; return self; }
}
class Leaf : Node {
  fn add() -> shared mut Node { return Node(1); }
}
fn main() -> int {
  return 0;
}
)pkn"},
    {"override of a `shared fn`, or by one",
     By::Sema, "error",
     "override of 'add' cannot be a 'shared fn': the method it overrides is not (a call of either takes its receiver as the overridden method says)",
     R"pkn(class Node {
  n: int;
  fn add() { self.n = self.n + 1; }
}
class Leaf : Node {
  shared fn add() { }
}
fn main() -> int {
  return 0;
}
)pkn"},
    {"storing `self` (in a method, in a `view fn`, in a struct's method)",
     By::Sema, "error",
     "cannot copy 'self' (a unique 'mut Node'); a method borrows its receiver (a 'shared fn' hands on a shared one)",
     R"pkn(class Node {
  key: int;
  fn me() -> int { let s = self; return 0; }
}
fn main() -> int { return 0; }
)pkn"},
    {"storing `self` (in a method, in a `view fn`, in a struct's method)",
     By::Sema, "error",
     "'self' is read-only in 'view fn me'; it cannot be stored, only read or passed to a 'view' parameter",
     R"pkn(class Node {
  key: int;
  view fn me() -> int { let s = self; return 0; }
}
fn main() -> int { return 0; }
)pkn"},
    {"storing `self` (in a method, in a `view fn`, in a struct's method)",
     By::Sema, "error",
     "cannot copy 'self': 'P' holds unique field 'name' ('Str'); a method borrows its receiver: write 'self.clone()' to copy it",
     R"pkn(struct P { name: Str; }
impl P {
  fn me() -> int { let s = self; return 0; }
}
fn main() -> int { return 0; }
)pkn"},
    {"capturing `self`",
     By::Sema, "error",
     "cannot capture 'self': a method borrows its receiver, and a function captures by copy; a 'shared fn' may capture it, or a 'weak' local of it ('me: weak mut = self;')",
     R"pkn(class Counter {
  n: int;
  fn keepIt() -> () -> int { return fn() -> int => self.n; }
}
fn main() -> int {
  return 0;
}
)pkn"},
    {"binding of a unique place stored",
     By::Sema, "error",
     "'n' is a 'match' binding, a borrow of 'x' ('match move x' binds what it owns); it cannot be stored, only read or passed to a 'view' parameter",
     R"pkn(class Node { key: int; }
fn main() -> int {
  x: Node? = Node(1);
  kept: Node? = None;
  match x {
    n: Node { kept = n; }
    None { }
  }
  return 0;
}
)pkn"},
    {"binding qualifier",
     By::Sema, "error",
     "'p' binds part of a 'shared Obj': its type is 'shared Node', not 'atomic shared Node'",
     R"pkn(class Node { key: int; }
fn main() -> int {
  let s: shared Obj = Node(1);
  match s {
    p: atomic shared Node { }
    _ { }
  }
  return 0;
}
)pkn"},
    {"binding a conditional of two forms",
     By::Sema, "error",
     "'n' binds part of a conditional whose branches have two forms, 'shared mut Base' and 'atomic shared Base': a binding holds one",
     R"pkn(class Base { v: int; }
class Node : Base { w: int; }
fn main() -> int {
  let sm: shared mut Base = Node(1, 2);
  let fz: atomic shared Base = Node(3, 4);
  c = True;
  match (if c then sm else fz) { n: Node { } _ { } }
  return 0;
}
)pkn"},
    {"an `atomic` read and written separately",
     By::Sema, "error",
     "'hits' is 'atomic': 'hits = ...' reads and writes it separately; write 'hits += e', or use 'exchange' or 'compareExchange'",
     R"pkn(fn main() -> int {
  hits: atomic int = 0;
  hits = hits * 2 + 1;
  return 0;
}
)pkn"},
    {"an `atomic` read and written separately",
     By::Sema, "error",
     "'hits' is 'atomic': this operator reads and writes it separately; its atomic operations are '+=', '-=', 'exchange' and 'compareExchange'",
     R"pkn(fn main() -> int {
  hits: atomic int = 0;
  hits *= 2;
  return 0;
}
)pkn"},
    {"an `atomic` array used whole",
     By::Sema, "error",
     "'seen' is an 'atomic bool[64]': it is read by element",
     R"pkn(fn main() -> int {
  seen: atomic bool[64] = [False; 64];
  let s = seen;
  return 0;
}
)pkn"},
    {"lending an `atomic`",
     By::Sema, "error",
     "'seen[3]' is an element of an 'atomic bool[64]': it has no handle and cannot be passed to 'inout' parameter 'b'",
     R"pkn(fn flip(b: inout bool) { b = True; }
fn main() -> int {
  seen: atomic bool[64] = [False; 64];
  flip(seen[3]);
  return 0;
}
)pkn"},
    {"lending an `atomic`",
     By::Sema, "error",
     "'hits' is 'atomic': it has no address and cannot be passed to 'inout' parameter 'n'",
     R"pkn(fn bump(n: inout int) { n += 1; }
fn main() -> int {
  hits: atomic int = 0;
  bump(hits);
  return 0;
}
)pkn"},
    {"lending an `atomic`",
     By::Sema, "error",
     "'hits' is 'atomic': it has no address and cannot be named by 'inout' local 'x'",
     R"pkn(fn main() -> int {
  hits: atomic int = 0;
  x: inout = hits;
  x += 1;
  return 0;
}
)pkn"},
    {"lending an `atomic`",
     By::Sema, "error",
     "'seen[3]' is an element of an 'atomic bool[64]': it has no handle to share; declare 'h' with its type to read it",
     R"pkn(fn main() -> int {
  seen: atomic bool[64] = [False; 64];
  h = seen[3];
  return 0;
}
)pkn"},
    {"`locked` contents outside a `lock` block",
     By::Sema, "error",
     "'st.counts' is 'locked': reach its contents in 'lock st.counts as c: view { }' (or 'inout')",
     R"pkn(class Counts { total: int; }
class State { counts: locked mut Counts; }
fn main() -> int {
  let st = State(Counts(0));
  return st.counts.total;
}
)pkn"},
    {"`lock` operand",
     By::Sema, "error",
     "'o' is not 'locked': a 'lock' block takes a 'locked' variable, field or parameter",
     R"pkn(class Counts { total: int; }
fn main() -> int {
  o: mut = Counts(0);
  lock o as m: inout { }
  return 0;
}
)pkn"},
    {"nested `lock`",
     By::Sema, "error",
     "a 'lock' block cannot hold another 'lock'",
     R"pkn(class Counts { total: int; }
fn main() -> int {
  let a: locked mut = Counts(0);
  let b: locked mut = Counts(0);
  lock a as c: inout {
    lock b as d: inout { }
  }
  return 0;
}
)pkn"},
    {"`parallel for` or `await` in a `lock` block",
     By::Sema, "error",
     "a 'parallel for' cannot run in a 'lock' block",
     R"pkn(class Counts { total: int; }
fn main() -> int {
  let a: locked mut = Counts(0);
  lock a as c: inout {
    parallel for i in 0..4 { }
  }
  return 0;
}
)pkn"},
    {"`parallel for` or `await` in a `lock` block",
     By::Sema, "error",
     "'await' cannot be used in a 'lock' block: the task would wait while it holds the lock",
     R"pkn(class Counts { total: int; }
async fn fib(n: int) -> int { return n; }
async fn main() -> int {
  let counts: locked mut = Counts(0);
  lock counts as c: inout {
    c.total = await fib(1);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block of what may wait (it takes a lock, through another function, it runs a `parallel for`, it awaits a task, another module's function)",
     By::Sema, "error",
     "'bump' may wait (it takes a lock); a 'lock' block cannot call it",
     R"pkn(class Counts { total: int; }
fn bump(c: locked mut Counts) { lock c as v: inout { v.total += 1; } }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  lock counts as c: inout {
    bump(counts);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block of what may wait (it takes a lock, through another function, it runs a `parallel for`, it awaits a task, another module's function)",
     By::Sema, "error",
     "'update' may wait (it takes a lock, through 'g'); a 'lock' block cannot call it",
     R"pkn(class Counts { total: int; }
fn g(c: locked mut Counts) { lock c as v: inout { v.total += 1; } }
fn update(c: locked mut Counts) { g(c); }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  lock counts as c: inout {
    update(counts);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block of what may wait (it takes a lock, through another function, it runs a `parallel for`, it awaits a task, another module's function)",
     By::Sema, "error",
     "'sum' may wait (it runs a 'parallel for'); a 'lock' block cannot call it",
     R"pkn(class Counts { total: int; }
fn sum(n: int) -> int { t = 0; parallel for i in 0..n { t += i; } return t; }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  lock counts as c: inout {
    c.total = sum(4);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block of what may wait (it takes a lock, through another function, it runs a `parallel for`, it awaits a task, another module's function)",
     By::Sema, "error",
     "'waitFor' may wait (it awaits a task); a 'lock' block cannot call it",
     R"pkn(class Counts { total: int; }
async fn work(n: int) -> int { return n + 1; }
fn waitFor(n: int) -> int { return await work(n); }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  lock counts as c: inout {
    c.total = waitFor(1);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block of what may wait (it takes a lock, through another function, it runs a `parallel for`, it awaits a task, another module's function)",
     By::Sema, "error",
     "'File.readln' may wait; a 'lock' block cannot call it",
     R"pkn(import ::io;
class Counts { total: int; }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  input = io::stdin();
  lock counts as c: inout {
    let line = input.readln();
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block that cannot be resolved (a function value, through a function, a method another module may override)",
     By::Sema, "error",
     "'f' may run code that takes a lock; a 'lock' block calls only functions it can resolve",
     R"pkn(class Counts { total: int; }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  let f = fn(n: int) -> int { return n + 1; };
  lock counts as c: inout {
    c.total = f(c.total);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block that cannot be resolved (a function value, through a function, a method another module may override)",
     By::Sema, "error",
     "'callIt' may run code that takes a lock (it calls 'f', a function value); a 'lock' block calls only functions it can resolve",
     R"pkn(class Counts { total: int; }
fn callIt(f: (int) -> int) -> int { return f(2); }
fn main() -> int {
  counts: locked mut Counts = Counts(0);
  let f = fn(n: int) -> int { return n + 1; };
  lock counts as c: inout {
    c.total = callIt(f);
  }
  return 0;
}
)pkn"},
    {"a call in a `lock` block that cannot be resolved (a function value, through a function, a method another module may override)",
     By::Sema, "error",
     "'Base.size' may run code that takes a lock (a class derived from 'Base' may override it; a 'final class Base' has none); a 'lock' block calls only functions it can resolve",
     R"pkn(class Counts { total: int; }
class Base { n: int; view fn size() -> int { return self.n; } }
fn measure(c: locked mut Counts, b: view Base) -> int {
  r = 0;
  lock c as v: inout {
    r = b.size();
  }
  return r;
}
)pkn"},
    {"a task started in a `lock` block whose body may wait",
     By::Sema, "error",
     "'bump' may wait (it takes a lock); a 'lock' block cannot start it (a task may run where it is started)",
     R"pkn(class Counts { total: int; }
async fn bump(c: locked mut Counts) -> int { lock c as v: inout { v.total += 1; } return 1; }
async fn quiet(n: int) -> int { return n + 1; }
async fn main() -> int {
  counts: locked mut Counts = Counts(0);
  t = quiet(0);
  n = await t;
  lock counts as c: inout {
    t = bump(counts);
  }
  n += await t;
  return n;
}
)pkn"},
    {"making a value `atomic shared` or `locked` (a `shared` field, a nested one, an `Obj`, through a type argument)",
     By::Sema, "error",
     "cannot make 'Settings(...)' (a 'Settings') 'atomic shared': field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Settings { name: Str; cache: shared Cache; }
fn main() -> int {
  let k: atomic shared = Settings("k", Cache(2));
  return 0;
}
)pkn"},
    {"making a value `atomic shared` or `locked` (a `shared` field, a nested one, an `Obj`, through a type argument)",
     By::Sema, "error",
     "cannot make 'Settings(...)' (a 'Settings') 'locked mut': field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Settings { name: Str; cache: shared Cache; }
fn main() -> int {
  let k: locked mut = Settings("k", Cache(2));
  return 0;
}
)pkn"},
    {"making a value `atomic shared` or `locked` (a `shared` field, a nested one, an `Obj`, through a type argument)",
     By::Sema, "error",
     "cannot make 'Outer(...)' (an 'Outer') 'atomic shared': field 's.cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Settings { name: Str; cache: shared Cache; }
class Outer { s: Settings; }
fn main() -> int {
  let k: atomic shared = Outer(Settings("k", Cache(2)));
  return 0;
}
)pkn"},
    {"making a value `atomic shared` or `locked` (a `shared` field, a nested one, an `Obj`, through a type argument)",
     By::Sema, "error",
     "cannot make 'o' (an 'Obj') 'atomic shared': it is an 'Obj', which may be any class",
     R"pkn(class Cache { hits: int; }
fn main() -> int {
  let o: Obj = Cache(2);
  let k: atomic shared Obj = move o;
  return 0;
}
)pkn"},
    {"making a value `atomic shared` or `locked` (a `shared` field, a nested one, an `Obj`, through a type argument)",
     By::Sema, "error",
     "cannot make 'x' (a 'Job') 'locked mut': field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
fn hold<T>(x: T) -> int { let c: locked mut T = move x; return 0; }
fn main() -> int {
  return hold<Job>(Job("j", Cache(0)));
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'j' (a 'Job'): field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
async fn run(j: Job) -> int { return j.name.len(); }
async fn main() -> int {
  let j = Job("j", Cache(0));
  let t = run(move j);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'a' (an 'A'): field 'b.job.cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
class Job { name: Str; cache: shared Cache; }
class B { job: Job; }
class A { b: B; }
async fn run(a: A) -> int { return 1; }
async fn main() -> int {
  let a = A(B(Job("j", Cache(0))));
  let t = run(move a);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "'c' is 'shared Cache': it never crosses to a task (a 'shared' reference is aliased on its own thread)",
     R"pkn(class Cache { hits: int; }
async fn run(c: shared Cache) -> int { return c.hits; }
async fn main() -> int {
  let c: shared = Cache(0);
  let t = run(c);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'f' (a '() -> int'): it is a function value, whose captures are not known",
     R"pkn(async fn call(f: () -> int) -> int { return f(); }
async fn main() -> int {
  let f = fn() => 3;
  let c = call(move f);
  return await c;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'o' (an 'Obj'): it is an 'Obj', which may be any class",
     R"pkn(class Node { key: int; }
async fn any(o: Obj) -> int { return 1; }
async fn main() -> int {
  let o: Obj = Node(1);
  let t = any(move o);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'a' (a 'shared Cache[2]'): it holds 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
async fn run(a: shared Cache[2]) -> int { return 1; }
async fn main() -> int {
  let c: shared = Cache(0);
  let a: shared Cache[2] = [c, c];
  let t = run(a);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send this value (a '((int, shared Cache), int)'): element '.0.1' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
async fn run(t: ((int, shared Cache), int)) -> int { return 1; }
async fn main() -> int {
  let c: shared = Cache(0);
  let t = run(((1, c), 2));
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'a' (a '(int, shared Cache)[2]'): element '[].1' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { hits: int; }
async fn run(a: (int, shared Cache)[2]) -> int { return 1; }
async fn main() -> int {
  let c: shared = Cache(0);
  let a: (int, shared Cache)[2] = [(1, c), (2, c)];
  let t = run(a);
  return await t;
}
)pkn"},
    {"send check: an argument of an `async fn` (a `shared` field, a nested one, a `shared` reference, a function value, an `Obj`, an array's elements, an element of a tuple's element, a part of each element of an array, a `weak` field)",
     By::Sema, "error",
     "cannot send 'x' (a 'Watch'): field 'w' is 'weak': a 'weak' reference watches an object of its own thread",
     R"pkn(class Node { key: int; }
class Watch { w: weak Node?; }
async fn run(x: Watch) -> int { return 1; }
async fn main() -> int {
  n: shared = Node(1);
  let x = Watch(n);
  let t = run(move x);
  return await t;
}
)pkn"},
    {"a class derived from one that is sent",
     By::Sema, "error",
     "cannot send 'w' (a 'Widget'): field 'onClick' is 'shared Handler': a 'shared' reference is aliased on its own thread (of class 'Button', derived from 'Widget')",
     R"pkn(class Handler { n: int; }
class Widget { id: int; }
class Button : Widget { onClick: shared Handler; }
async fn show(w: Widget) -> int { return w.id; }
async fn main() -> int {
  let w = Widget(1);
  let t = show(move w);
  return await t;
}
)pkn"},
    {"a class derived from one that is sent",
     By::Sema, "error",
     "class 'Button<shared Handler>' extends 'Widget', which is sent to another thread: field 'onClick' is 'shared Handler': a 'shared' reference is aliased on its own thread",
     R"pkn(class Handler { n: int; }
class Widget { id: int; }
class Button<T> : Widget { onClick: T; }
async fn show(w: Widget) -> int { return w.id; }
async fn main() -> int {
  let w = Widget(1);
  let t = show(move w);
  let h: shared = Handler(1);
  let b = Button<shared Handler>(2, h);
  return await t;
}
)pkn"},
    {"copy of `T`",
     By::Sema, "error",
     "'T' is not 'Copyable': declare 'T: Copyable' to copy it",
     R"pkn(fn keep<T>(x: T) -> T { let k = x; return k; }
fn main() -> int { return 0; }
)pkn"},
    {"method or `static fn` of an `impl` block whose bound the instance does not meet (a method, a `static fn`, a method of two blocks, of a trait's block, a derived `clone`)",
     By::Sema, "error",
     "no method 'get' on type 'List<Node>': 'get' is in 'impl List<T: Copyable>', and 'Node' is not 'Copyable'",
     R"pkn(class Node { key: int; }
class List<T> { v: T; }
impl List<T> { fn __init__(v: T) { self.v = move v; } }
impl List<T: Copyable> { view fn get() -> T { return self.v; } }
fn main() -> int {
  let l = List<Node>(Node(1));
  let n = l.get();
  return 0;
}
)pkn"},
    {"method or `static fn` of an `impl` block whose bound the instance does not meet (a method, a `static fn`, a method of two blocks, of a trait's block, a derived `clone`)",
     By::Sema, "error",
     "'List<Node>' has no static function 'pair': 'pair' is in 'impl List<T: Copyable>', and 'Node' is not 'Copyable'",
     R"pkn(class Node { key: int; }
class List<T> { v: T; }
impl List<T> { fn __init__(v: T) { self.v = move v; } }
impl List<T: Copyable> { static fn pair(v: T) -> (T, T) { return (v, v); } }
fn main() -> int {
  let p = List<Node>::pair(Node(1));
  return 0;
}
)pkn"},
    {"method or `static fn` of an `impl` block whose bound the instance does not meet (a method, a `static fn`, a method of two blocks, of a trait's block, a derived `clone`)",
     By::Sema, "error",
     "no method 'get' on type 'List<Job>': 'get' is in 'impl List<T: Copyable>', and 'Job' is not 'Copyable'; and in 'impl List<T: Sendable>', and 'Job' is not 'Sendable'",
     R"pkn(class Node { key: int; }
class Job { n: shared Node; }
class List<T> { v: T; }
impl List<T: Copyable> { view fn get() -> int { return 1; } }
impl List<T: Sendable> { view fn get() -> int { return 2; } }
fn main() -> int {
  let l = List<Node>(Node(1));
  let j = List<Job>(Job(Node(3)));
  return l.get() + j.get();
}
)pkn"},
    {"method or `static fn` of an `impl` block whose bound the instance does not meet (a method, a `static fn`, a method of two blocks, of a trait's block, a derived `clone`)",
     By::Sema, "error",
     "no method 'name' on type 'Box<Node>': 'name' is in 'impl Named for Box<T: Copyable>', and 'Node' is not 'Copyable'",
     R"pkn(class Node { key: int; }
trait Named { view fn name() -> Str; }
class Box<T> { v: T; }
impl Box<T> { fn __init__(v: T) { self.v = move v; } }
impl Named for Box<T: Copyable> { view fn name() -> Str { return "box"; } }
fn main() -> int {
  let a = Box<int>(1);
  let b = Box<Node>(Node(1));
  return a.name().len() + b.name().len();
}
)pkn"},
    {"method or `static fn` of an `impl` block whose bound the instance does not meet (a method, a `static fn`, a method of two blocks, of a trait's block, a derived `clone`)",
     By::Sema, "error",
     "no method 'clone' on type 'List<Node>': 'clone' is in 'impl Cloneable for List<T: Cloneable>', and 'Node' is not 'Cloneable'",
     R"pkn(class Node { key: int; }
class List<T> { v: T; }
impl Cloneable for List<T: Cloneable> {}
fn main() -> int {
  let l = List<Node>(Node(1));
  let c = l.clone();
  return 0;
}
)pkn"},
    {"`clone` of an instance of a generic class with no `impl Cloneable for` block",
     By::Sema, "error",
     "class 'List<Node>' is not Cloneable: its generic class 'List' has no 'impl Cloneable for' block (an empty one derives 'clone')",
     R"pkn(class Node { key: int; }
class List<T> { v: T; }
fn main() -> int {
  let l = List<Node>(Node(1));
  let c = l.clone();
  return 0;
}
)pkn"},
    {"`Copyable` bound not met",
     By::Sema, "error",
     "'Node' does not implement trait 'Copyable', which 'twice' requires of 'T': a unique reference, a 'Str' or a task is move-only",
     R"pkn(class Node { key: int; }
fn twice<T: Copyable>(x: T) -> (T, T) { return (x, x); }
fn main() -> int {
  let d = twice(Node(2));
  return 0;
}
)pkn"},
    {"`impl Copyable`",
     By::Sema, "error",
     "'Copyable' is built in: a type is Copyable by what it holds, and cannot be implemented",
     R"pkn(class Named { tag: Str; n: int; }
impl Copyable for Named {}
fn main() -> int { return 0; }
)pkn"},
    {"task handle not awaited (never, before a `return`, on some paths, replaced)",
     By::Sema, "error",
     "task 't' is never awaited: a task handle is spent by 'await' (or 'move') on every path before its scope ends",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn work() {
  let t = fib(5);
}
fn main() -> int {
  work();
  return 0;
}
)pkn"},
    {"task handle not awaited (never, before a `return`, on some paths, replaced)",
     By::Sema, "error",
     "task 't' is not awaited before this 'return': a task handle is spent by 'await' (or 'move') on every path before its scope ends",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn main() -> int {
  let t = fib(5);
  return 0;
}
)pkn"},
    {"task handle not awaited (never, before a `return`, on some paths, replaced)",
     By::Sema, "error",
     "task 't' is not awaited on every path before this 'return': it is spent on some paths only (by the 'if' at 6:3, at 6:14); every path awaits it, or moves it",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn main() -> int {
  let c = True;
  let t = fib(5);
  k = 0;
  if c { k = await t; }
  return k;
}
)pkn"},
    {"task handle not awaited (never, before a `return`, on some paths, replaced)",
     By::Sema, "error",
     "task 't' is replaced before it is awaited: await it (or move it) first",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn main() -> int {
  t = fib(5);
  t = fib(4);
  return await t;
}
)pkn"},
    {"task handle awaited twice",
     By::Sema, "error",
     "'t' was awaited at 4:11; a task handle is spent by its 'await'",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn main() -> int {
  let t = fib(5);
  let a = await t;
  let b = await t;
  return a + b;
}
)pkn"},
    {"task handle borrowed by a local (a variable's, a call's)",
     By::Sema, "error",
     "'view' local 'v' cannot name a task: a task handle is spent by 'await' or 'move', and a borrow does neither",
     R"pkn(async fn f(n: int) -> int { return n + 1; }
async fn main() -> int {
  let t = f(1);
  v: view = t;
  return await t;
}
)pkn"},
    {"task handle borrowed by a local (a variable's, a call's)",
     By::Sema, "error",
     "'inout' local 'x' cannot name a task: a task handle is spent by 'await' or 'move', and a borrow does neither",
     R"pkn(async fn f(n: int) -> int { return n + 1; }
async fn main() -> int {
  x: inout = f(1);
  return 0;
}
)pkn"},
    {"task handle qualified, or given to a template",
     By::Sema, "error",
     "'shared' qualifies a reference; 'Task<int>' is a value",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn main() -> int {
  let t: shared = fib(5);
  return await t;
}
)pkn"},
    {"task handle qualified, or given to a template",
     By::Sema, "error",
     "type argument 1 of 'peek' cannot be a task yet: a task is held by a variable",
     R"pkn(async fn fib(n: int) -> int { return n; }
fn peek<T>(t: view T) -> int { return 1; }
fn main() -> int {
  let t = fib(5);
  peek(t);
  return await t;
}
)pkn"},
    {"async call whose task is dropped",
     By::Sema, "error",
     "this task is never awaited: 'await' the call, or hold the task in a variable and 'await' that",
     R"pkn(async fn fetch(n: int) -> int { return n; }
fn main() -> int {
  fetch(1);
  return 0;
}
)pkn"},
    {"`await` operand",
     By::Sema, "error",
     "'await' takes a task (the result of calling an 'async fn'), not an 'int'",
     R"pkn(fn main() -> int {
  let w = await 3;
  return w;
}
)pkn"},
    {"`await` in a `parallel for`",
     By::Sema, "error",
     "'await' cannot be used in a 'parallel for' body",
     R"pkn(async fn fib(n: int) -> int { return n; }
async fn main() -> int {
  parallel for i in 0..4 {
    let k = await fib(i);
  }
  return 0;
}
)pkn"},
    {"async function as a value",
     By::Sema, "error",
     "async function 'f' is not a value: call it",
     R"pkn(async fn f() -> int { return 1; }
fn main() -> int {
  let g = f;
  return 0;
}
)pkn"},
    {"override of an `async fn`, or by one",
     By::Sema, "error",
     "override of 'run' must be an 'async fn', like the method it overrides",
     R"pkn(class Base {
  n: int;
  async fn run() -> int { return self.n; }
}
class Der : Base {
  fn run() -> int { return 1; }
}
fn main() -> int { return 0; }
)pkn"},
    {"override of an `async fn`, or by one",
     By::Sema, "error",
     "override of 'run' cannot be an 'async fn': the method it overrides is not",
     R"pkn(class Base {
  n: int;
  fn run() -> int { return self.n; }
}
class Der : Base {
  async fn run() -> int { return 1; }
}
fn main() -> int { return 0; }
)pkn"},
    {"async `__init__`",
     By::Sema, "error",
     "'__init__' cannot be 'async': constructing gives the object at once",
     R"pkn(class E { n: int; }
impl E { async fn __init__() { self.n = 1; } }
fn main() -> int { return 0; }
)pkn"},
    {"async borrowed result",
     By::Sema, "error",
     "an 'async fn' returns a value: its result cannot be 'view' or 'inout'",
     R"pkn(class Node { v: int; }
class Holder {
  n: Node;
  async fn peek() -> view Node { return self.n; }
}
fn main() -> int { return 0; }
)pkn"},
    {"async result that cannot cross",
     By::Sema, "error",
     "the result of 'async fn' 'give' (a 'shared Job') cannot cross to the task's awaiter: a 'shared' reference is aliased on its own thread",
     R"pkn(class Job { name: Str; }
async fn give() -> shared Job { return Job("x"); }
fn main() -> int { return 0; }
)pkn"},
    {"async result that cannot cross",
     By::Sema, "error",
     "the result of 'async fn' 'give' (a 'Job') cannot cross to the task's awaiter: field 'cache' is 'shared Cache': a 'shared' reference is aliased on its own thread",
     R"pkn(class Cache { n: int; }
class Job { cache: shared Cache; }
async fn give() -> Job { return Job(Cache(1)); }
fn main() -> int { return 0; }
)pkn"},
    {"async result that cannot cross",
     By::Sema, "error",
     "the result of 'async fn' 'give' (an 'Obj') cannot cross to the task's awaiter: it is an 'Obj', which may be any class",
     R"pkn(async fn give() -> Obj { return "x"; }
fn main() -> int { return 0; }
)pkn"},
    {"borrowed parameter of an `async fn`",
     By::Sema, "error",
     "parameter 'xs' of 'async fn' 'f' cannot be 'view': a task outlives the call that starts it",
     R"pkn(async fn f(xs: view int[4]) -> int { return xs[0]; }
fn main() -> int { return 0; }
)pkn"},
    {"receiver of an async method (borrowed by the task, or of a form that cannot cross)",
     By::Sema, "error",
     "'w.run()' lends 'w' to the task it starts: 'await' it where it is called",
     R"pkn(class Worker {
  n: int;
  async view fn run() -> int { return self.n; }
}
fn main() -> int {
  let w = Worker(1);
  let t = w.run();
  return await t;
}
)pkn"},
    {"receiver of an async method (borrowed by the task, or of a form that cannot cross)",
     By::Sema, "error",
     "the receiver of 'async fn' 'run' ('w', a 'shared Worker') cannot cross to its task: a 'shared' reference is aliased on its own thread",
     R"pkn(class Worker {
  n: int;
  async view fn run() -> int { return self.n; }
}
fn main() -> int {
  let w: shared = Worker(1);
  return await w.run();
}
)pkn"},
    {"`inout` across a wait",
     By::Sema, "error",
     "'self.n' is reached through 'self'; it cannot be borrowed 'inout' across an 'await': another task may change it there",
     R"pkn(class Counter { n: int; }
async fn tick(k: int) -> int { return k; }
impl Counter {
  async fn add(k: int) -> int {
    c: inout = self.n;
    let e = await tick(10);
    c = c + k;
    return e;
  }
}
fn main() -> int { return 0; }
)pkn"},
    {"`inout` across a wait",
     By::Sema, "error",
     "'s.n' is reached through 'shared mut' reference 's'; it cannot be borrowed 'inout' across an 'await': another task may change it there",
     R"pkn(class Counter { n: int; }
async fn tick(k: int) -> int { return k; }
fn viaShared(s: shared mut Counter) -> int {
  x: inout = s.n;
  let r = await tick(1);
  x = x + r;
  return r;
}
fn main() -> int { return 0; }
)pkn"},
    {"`view` across a wait",
     By::Sema, "error",
     "'s.meta' is reached through 'shared mut' reference 's' and is not Copyable; it cannot be lent to an awaited call: another task may change it there",
     R"pkn(struct Meta { name: Str; n: int; }
impl Meta {
  async view fn size() -> int { return self.name.len() + self.n; }
}
class Holder { meta: Meta; }
fn use(s: shared mut Holder) -> int {
  return await s.meta.size();
}
fn main() -> int { return 0; }
)pkn"},
    {"`view` across a wait",
     By::Sema, "error",
     "'self.meta' is reached through 'self' and is not Copyable; it cannot be lent to an awaited call: another task may change it there",
     R"pkn(struct Meta { name: Str; n: int; }
impl Meta {
  async view fn size() -> int { return self.name.len() + self.n; }
}
class Holder { meta: Meta; }
impl Holder {
  async fn work() -> int {
    return await self.meta.size();
  }
}
fn main() -> int { return 0; }
)pkn"},
    {"`view` across a wait",
     By::Sema, "error",
     "'h.meta' is reached through 'view' parameter 'h' and is not Copyable; it cannot be lent to an awaited call: another task may change it there",
     R"pkn(struct Meta { name: Str; n: int; }
impl Meta {
  async view fn size() -> int { return self.name.len() + self.n; }
}
class Holder { meta: Meta; }
fn use(h: view Holder) -> int {
  return await h.meta.size();
}
fn main() -> int { return 0; }
)pkn"},
    {"`parallel` before another statement (the parser)",
     By::Parser, "error",
     "unexpected 'while'; expected 'for' after 'parallel'",
     R"pkn(fn main() -> int {
  n = 0;
  parallel while n < 4 { n += 1; }
  return 0;
}
)pkn"},
    {"outer reference in the body",
     By::Sema, "error",
     "'nodes' (a 'List<Node>') cannot be used in a 'parallel for' body: an iteration uses only values, arrays of values and 'atomic' variables of the enclosing function",
     R"pkn(import ::collections as cl;
class Node { id: int; }
fn main() -> int {
  let nodes = cl::List<Node>();
  total = 0;
  parallel for i in 0..4 {
    total += nodes.len();
  }
  return total;
}
)pkn"},
    {"outer variable assigned (an object, a number), moved, bound `inout` or passed `inout`",
     By::Sema, "error",
     "cannot assign to 'cfg' in a 'parallel for' body: a variable of the enclosing function is not changed by an iteration (a number changes by a reduction, an 'atomic' variable by its operations)",
     R"pkn(class Config { lo: int; }
fn main() -> int {
  cfg: atomic shared Config = Config(0);
  parallel for i in 0..4 {
    cfg = Config(i);
  }
  return 0;
}
)pkn"},
    {"outer variable assigned (an object, a number), moved, bound `inout` or passed `inout`",
     By::Sema, "error",
     "cannot assign to 'total' in a 'parallel for' body: a variable of the enclosing function changes only by a reduction ('total += e', 'total -= e', 'total *= e') or, if 'atomic', by its operations",
     R"pkn(fn main() -> int {
  total = 0;
  parallel for i in 0..4 {
    total = i;
  }
  return total;
}
)pkn"},
    {"outer variable assigned (an object, a number), moved, bound `inout` or passed `inout`",
     By::Sema, "error",
     "'cfg' is moved in a 'parallel for' body: what the enclosing function holds changes only by a reduction of a variable, or at the loop's variable in an array",
     R"pkn(class Config { lo: int; }
fn main() -> int {
  cfg: atomic shared Config = Config(0);
  parallel for i in 0..4 {
    let mine = move cfg;
  }
  return 0;
}
)pkn"},
    {"outer variable assigned (an object, a number), moved, bound `inout` or passed `inout`",
     By::Sema, "error",
     "'cfg' is bound 'inout' in a 'parallel for' body: what the enclosing function holds changes only by a reduction of a variable, or at the loop's variable in an array",
     R"pkn(class Config { lo: int; }
fn main() -> int {
  cfg: atomic shared Config = Config(0);
  parallel for i in 0..4 {
    r: inout = cfg;
  }
  return 0;
}
)pkn"},
    {"outer variable assigned (an object, a number), moved, bound `inout` or passed `inout`",
     By::Sema, "error",
     "'ys' is passed 'inout' in a 'parallel for' body: what the enclosing function holds changes only by a reduction of a variable, or at the loop's variable in an array",
     R"pkn(fn fill(p: inout int[]) { p[0] = 1; }
fn main() -> int {
  ys = [0; 4];
  parallel for i in 0..4 {
    fill(ys);
  }
  return 0;
}
)pkn"},
    {"`atomic` changed on every iteration",
     By::Sema, "warning",
     "'hits' changes on every iteration of the 'parallel for': each atomic operation moves its cache line between cores, at many times a reduction's cost; count in a reduction (a plain variable of the enclosing function, '+='), or change it less often",
     R"pkn(fn main() -> int {
  hits: atomic int = 0;
  parallel for i in 0..100 {
    hits += 1;
  }
  return hits;
}
)pkn"},
    {"write at another index",
     By::Sema, "error",
     "'ys' is written in a 'parallel for' body at an index other than the loop's variable 'i': two iterations could write one element",
     R"pkn(fn main() -> int {
  ys = [0; 8];
  parallel for i in 0..7 {
    ys[i + 1] = 1;
  }
  return 0;
}
)pkn"},
    {"read of a written array",
     By::Sema, "error",
     "'ys' is written in a 'parallel for' body and cannot be read there except at the place written: another iteration could be writing what it reads",
     R"pkn(fn main() -> int {
  ys = [0; 8];
  parallel for i in 1..8 {
    ys[i] = ys[i - 1] + 1;
  }
  return 0;
}
)pkn"},
    {"reduction read",
     By::Sema, "error",
     "'s' is a reduction of this 'parallel for' and cannot be read in its body: an iteration sees only part of it",
     R"pkn(fn main() -> int {
  s = 0;
  parallel for i in 0..4 {
    if s > 10 { continue; }
    s += 1;
  }
  return s;
}
)pkn"},
    {"`break`, `return`",
     By::Sema, "error",
     "'break' cannot be used in a 'parallel for' body",
     R"pkn(fn main() -> int {
  parallel for i in 0..4 { break; }
  return 0;
}
)pkn"},
    {"`break`, `return`",
     By::Sema, "error",
     "'return' cannot leave a 'parallel for' body",
     R"pkn(fn main() -> int {
  parallel for i in 0..4 { return 1; }
  return 0;
}
)pkn"},
    {"call that is not par-safe",
     By::Sema, "error",
     "a 'parallel for' body cannot call 'log', which calls 'g', which calls '::io::println<Str>', which calls '::io::_println', which is not par-safe",
     R"pkn(import ::io;
fn g(x: int) { io::println(Str(x)); }
fn log(x: int) { g(x); }
fn main() -> int {
  parallel for i in 0..2 {
    log(i);
  }
  return 0;
}
)pkn"},
    {"virtual call another module may override",
     By::Sema, "error",
     "a 'parallel for' body cannot call 'Shape.area', which a class derived from 'Shape' may override",
     R"pkn(class Shape { view fn area() -> int { return 1; } }
impl Shape { fn __init__() { } }
fn total() -> int {
  s = 0;
  parallel for i in 0..2 {
    let shape = Shape();
    s += shape.area();
  }
  return s;
}
)pkn"},
};
// clang-format on

/// The diagnostics of @p row's program, as the compiler prints them.
std::string diagnosticsOf(const Row &row) {
  return row.Stage == By::Parser ? parseWithDiags(row.Program).Text
                                 : semaCheck(row.Program).Diagnostics;
}

// The design document is the repository's: a frontend plugin built against
// an installation runs the rows above with no document to compare them to
// (cmake/PaykanFrontendTests.cmake.in defines no PAYKAN_DESIGN_DOCS_DIR).
#ifdef PAYKAN_DESIGN_DOCS_DIR
/// One row of the document's tables: its situation, and each message of
/// its second cell (a text in backticks) with whether "warning: " stands
/// before it.
struct DocRow {
  std::string Situation;
  std::vector<std::pair<std::string, bool>> Messages;
};

/// The rows of the tables under "### In the language now" of the document's
/// section "## Diagnostics".  @p later gets every text in backticks of the
/// section's other tables, which list what the compiler does not print yet.
std::vector<DocRow> documentedRows(std::set<std::string> &later) {
  std::ifstream in(std::string(PAYKAN_DESIGN_DOCS_DIR) + "/concurrency.md");
  std::vector<DocRow> rows;
  bool inSection = false, now = false;
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("## ", 0) == 0) {
      inSection = line == "## Diagnostics";
      now = false;
      continue;
    }
    if (line.rfind("### ", 0) == 0) {
      now = line == "### In the language now";
      continue;
    }
    // A table's line, but for its heading and the rule under it.
    if (!inSection || line.empty() || line[0] != '|' ||
        line.rfind("|---", 0) == 0 || line.rfind("| Situation |", 0) == 0)
      continue;
    // The situation may quote code too: the messages are the cells after
    // it.
    const size_t from = line.find_first_not_of(' ', 1);
    const size_t cell = line.find(" | ", 1);
    DocRow row;
    if (from != std::string::npos && cell != std::string::npos && from < cell)
      row.Situation = line.substr(from, cell - from);
    else
      row.Situation = line; // no second cell: a row with no message
    for (size_t open = cell == std::string::npos ? cell : line.find('`', cell);
         open != std::string::npos;) {
      const size_t close = line.find('`', open + 1);
      if (close == std::string::npos)
        break;
      const std::string text = line.substr(open + 1, close - open - 1);
      const bool warning =
          open >= 9 && line.compare(open - 9, 9, "warning: ") == 0;
      if (now)
        row.Messages.emplace_back(text, warning);
      else
        later.insert(text);
      open = line.find('`', close + 1);
    }
    if (now)
      rows.push_back(std::move(row));
  }
  return rows;
}
#endif

} // namespace

TEST(DiagnosticsTable, EachRowIsWhatTheCompilerPrints) {
  for (const Row &row : kRows) {
    SCOPED_TRACE(row.Situation);
    const std::string text = diagnosticsOf(row);
    const std::string want = std::string(row.Kind) + ": " + row.Message + "\n";
    EXPECT_NE(text.find(want), std::string::npos)
        << "expected: " << want << "the compiler printed:\n"
        << text << "for:\n"
        << row.Program;
  }
}

#ifdef PAYKAN_DESIGN_DOCS_DIR
// The document's tables of what the language has now list each row here,
// under its situation and as the error or warning it is, and nothing else:
// neither is changed without the other.  Every row of them has a message,
// and no message of a row here is in the tables of what the compiler does
// not print yet.
TEST(DiagnosticsTable, TheDesignDocumentListsEachMessage) {
  std::set<std::string> later;
  const std::vector<DocRow> rows = documentedRows(later);
  ASSERT_FALSE(rows.empty()) << "no table under '### In the language now' in "
                             << PAYKAN_DESIGN_DOCS_DIR << "/concurrency.md";
  // Situation, kind, message.
  using Entry = std::tuple<std::string, std::string, std::string>;
  auto shown = [](const Entry &e) {
    return std::get<1>(e) + ": " + std::get<2>(e) + "\n  (" + std::get<0>(e) +
           ")";
  };
  std::set<Entry> documented, pinned;
  for (const DocRow &row : rows) {
    EXPECT_FALSE(row.Messages.empty())
        << "a row of the document's table has no message in backticks: "
        << row.Situation;
    for (const auto &[message, warning] : row.Messages)
      documented.emplace(row.Situation, warning ? "warning" : "error", message);
  }
  for (const Row &row : kRows) {
    pinned.emplace(row.Situation, row.Kind, row.Message);
    EXPECT_FALSE(later.count(row.Message))
        << "the document lists it as not printed yet: " << row.Message;
  }
  for (const Entry &e : pinned)
    EXPECT_TRUE(documented.count(e))
        << "not in the document's table: " << shown(e);
  for (const Entry &e : documented)
    EXPECT_TRUE(pinned.count(e))
        << "in the document's table, with no row here: " << shown(e);
}
#endif
