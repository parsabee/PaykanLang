// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (docs/pir.md §12) on hand-written PIR: which copies of a
// box argument it lends, which it keeps because a statement from the read
// to the call, or one nested in those, may let the caller's object go (the
// window), and which it keeps because the copy is no read of a local the
// call could be lent; which box parameters it lends, where their bodies
// then ask the ownership mask, and which keep their prologue; and that the
// code passes change nothing on the pass's own output.

#include "paykan/pir/Lend.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/RCElimination.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <map>
#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::pir;

namespace {

// What every program below calls.  The callees lend (a box parameter, a
// definition, each starting with its parameters' prologue, docs/pir.md §3):
// @take and @g keep nothing, @two takes two boxes, @pair a box and a bool,
// @poke a pointer (an `inout` argument); Node's slot `look` takes a box
// too.  The runtime's `equals` does not lend.
const std::string kPrelude = R"(module "m"
extern fn @$rt.PaykanObject_equals(obj, box, i64) -> i64

class Node {
  field name: box
  field next: box
  vtable {
    destroy = @Node.destroy : (obj) -> void
    look = @Node.look : (obj, box) -> void
  }
}

fn @Node.destroy(%self: obj) -> void {
  %n = field.load %self, Node.name
  release %n
  %x = field.load %self, Node.next
  release %x
  free %self
  ret
}

fn @Node.look(%self: obj, %p: box) -> void {
  %po = owned %p
  %pl = not %po
  if %pl {
    retain %p
  }
  release %p
  ret
}

fn @take(%p: box) -> void {
  %po = owned %p
  %pl = not %po
  if %pl {
    retain %p
  }
  release %p
  ret
}

fn @two(%p: box, %q: box) -> void {
  %po = owned %p
  %pl = not %po
  if %pl {
    retain %p
  }
  %qo = owned %q
  %ql = not %qo
  if %ql {
    retain %q
  }
  release %p
  release %q
  ret
}

fn @pair(%p: box, %b: bool) -> void {
  %po = owned %p
  %pl = not %po
  if %pl {
    retain %p
  }
  release %p
  ret
}

fn @g(%p: box) -> bool {
  %po = owned %p
  %pl = not %po
  if %pl {
    retain %p
  }
  release %p
  ret true
}

fn @cond() -> bool {
  ret true
}

fn @poke(%p: ptr) -> void {
  ret
}

fn @main() -> i64 {
  ret 0
}
)";

struct Lent {
  Program P;
  MoveCounts Moves;
  LendCounts Counts;
};

/// The prelude and @p text parsed and verified; then, on the function @p
/// name, moveAtLastUse when @p moveFirst (the lowering's code, as the
/// driver runs the passes) and lend, and the result verified.
Lent lendIn(const std::string &text, bool moveFirst,
            const std::string &name = "f") {
  Lent l;
  ParseError err;
  auto p = parseProgram(kPrelude + text, err);
  EXPECT_TRUE(p) << err.str();
  if (!p)
    return l;
  l.P = std::move(*p);
  auto errors = verify(l.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  Module &m = l.P.Modules.front();
  for (Function &f : m.Functions) {
    if (f.Name != name)
      continue;
    if (moveFirst)
      l.Moves = moveAtLastUse(f);
    l.Counts = lendArguments(f, m);
  }
  errors = verify(l.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors) << toString(l.P);
  return l;
}

/// lend on code the moves at last use already ran on.
Lent lent(const std::string &text, const std::string &name = "f") {
  return lendIn(text, false, name);
}

/// The moves at last use, then lend: the lowering's code as the driver
/// passes it on.
Lent movedThenLent(const std::string &text, const std::string &name = "f") {
  return lendIn(text, true, name);
}

/// The printed function @p name of @p l.
std::string f(const Lent &l, const std::string &name = "f") {
  const std::string text = toString(l.P);
  const size_t start = text.find("fn @" + name + "(");
  if (start == std::string::npos)
    return "";
  return text.substr(start, text.find("\n}\n", start) + 3 - start);
}

size_t count(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size()))
    ++n;
  return n;
}

} // namespace

// -- What is lent

TEST(Lend, ACopyOfAVariableTheCallerKeepsIsLent) {
  // The copy's +1 was released by the callee, never the last reference:
  // x keeps the object until its release after the call.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(f(l), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  store %x.0, %a.1
  %v.2 = load %x.0
  call @take(lent %v.2)
  %w.3 = load %x.0
  release %w.3
  ret
}
)");
  EXPECT_EQ(l.Counts.Marks, 1u);
  EXPECT_EQ(l.Counts.Lent, 1u);
  EXPECT_EQ(l.Counts.KeptForWindow, 0u);
}

TEST(Lend, AFieldReadOfAVariableIsLent) {
  // x.name: the root is x, which nothing between its load and the call
  // touches; the read's other fields and calls that only read x are no
  // reason to keep the copy.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
  %x2 = load %x
  %o2 = unbox %x2
  %nx = field.load %o2, Node.next
  %none = cmp eq %nx, null box
  call @pair(%n, %none)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_NE(f(l).find("call @pair(lent %n."), std::string::npos) << f(l);
  EXPECT_EQ(count(f(l), "retain"), 0u) << f(l);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

TEST(Lend, AVirtualAndAnIndirectCallAreLentAsADirectOne) {
  // A vtable slot and a function value with a box parameter lend.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  %u = load %x
  %o = unbox %u
  vcall %o : Node [1] (%v)
  %fp = fn.addr @take
  %w = load %x
  retain lend %w
  icall %fp : (box) -> void (%w)
  %z = load %x
  release %z
  ret
}
)");
  EXPECT_NE(f(l).find("vcall %o.4 : Node [1] (lent %v.2)"), std::string::npos)
      << f(l);
  EXPECT_NE(f(l).find("(box) -> void (lent %w.6)"), std::string::npos) << f(l);
  EXPECT_EQ(count(f(l), "retain"), 0u) << f(l);
  EXPECT_EQ(l.Counts.Lent, 2u);
}

TEST(Lend, ACopyInALoopBodyIsLent) {
  // The window is in the body: each iteration reads x, lends it and keeps
  // its reference to the next.
  Lent l = movedThenLent(R"(
fn @f(%a: box) -> void {
  local %x: box
  local %i: i64
  store %x, %a
  store %i, 0
  while {
    %n = load %i
    %more = cmp lt %n, 3
    cond %more
  } {
    %v = load %x
    retain lend %v
    call @take(%v)
    %n2 = load %i
    %n3 = add %n2, 1
    store %i, %n3
  }
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_NE(f(l).find("call @take(lent %v."), std::string::npos) << f(l);
  EXPECT_EQ(count(f(l), "retain"), 0u) << f(l);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

TEST(Lend, WhatAnInoutNameNamesIsLentWhileItsPointerIsOnlyReadThrough) {
  // `n: inout Node`: the root is the slot of the pointer; the caller keeps
  // what it points to alive, and nothing here writes it.
  Lent l = lent(R"(
fn @f(%p: ptr) -> void {
  local %n: ptr
  store %n, %p
  %q = load %n
  %b = ptr.load box, %q
  retain lend %b
  call @take(%b)
  ret
}
)");
  EXPECT_NE(f(l).find("call @take(lent %b."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

TEST(Lend, AnElementReadThroughAStructMethodsSelfIsLent) {
  // A struct method's `self` is a pointer: `self.n` is read through a
  // `ptr.add` of it.
  Lent l = lent(R"(
fn @f(%self: ptr) -> void {
  local %s: ptr
  store %s, %self
  %sa = load %s
  %ea = ptr.add %sa, 8
  %e = ptr.load box, %ea
  retain lend %e
  call @take(%e)
  ret
}
)");
  EXPECT_NE(f(l).find("call @take(lent %e."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

TEST(Lend, TwoCopiesOfOneVariableInOneCallAreBothLent) {
  // f(x, x) where x is read again: both are copies, x outlives the call.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  %u = load %x
  retain lend %u
  call @two(%v, %u)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(f(l), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  store %x.0, %a.1
  %v.2 = load %x.0
  %u.3 = load %x.0
  call @two(lent %v.2, lent %u.3)
  %w.4 = load %x.0
  release %w.4
  ret
}
)");
  EXPECT_EQ(l.Counts.Lent, 2u);
}

// -- The window: what keeps a copy

TEST(Lend, FXXAtTheLastUseKeepsTheFirstCopy) {
  // f(x, x) at x's last use: the moves hand x's own reference over as the
  // second argument, so the first copy is the only reference f is sure of.
  Lent l = movedThenLent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  %u = load %x
  retain lend %u
  call @two(%v, %u)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(l.Moves.Transfers, 1u);
  EXPECT_EQ(f(l), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  store %x.0, %a.1
  %v.2 = load %x.0
  retain %v.2
  %u.3 = load %x.0
  call @two(%v.2, %u.3)
  ret
}
)");
  EXPECT_EQ(l.Counts.Marks, 1u);
  EXPECT_EQ(l.Counts.Lent, 0u);
  EXPECT_EQ(l.Counts.KeptForWindow, 1u);
}

TEST(Lend, AFieldReadBesideItsRootHandedOverIsKept) {
  // f(x.name, x) at x's last use: f may let x go, and x.name with it.
  Lent l = movedThenLent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
  %x2 = load %x
  retain lend %x2
  call @two(%n, %x2)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_NE(f(l).find("retain %n."), std::string::npos) << f(l);
  EXPECT_NE(f(l).find("call @two(%n.4, %x2.5)"), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.KeptForWindow, 1u);
  // With x read again after the call, both are lent.
  Lent copy = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
  %x2 = load %x
  retain lend %x2
  call @two(%n, %x2)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_NE(f(copy).find("call @two(lent %n.4, lent %x2.5)"), std::string::npos)
      << f(copy);
  EXPECT_EQ(copy.Counts.Lent, 2u);
}

TEST(Lend, AFieldReadBesideACallTheRootIsHandedToIsKept) {
  // f(x.next, g(x)) at x's last use: g may drop x before f is called.
  const std::string text = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.next
  retain lend %n
  %x2 = load %x
  retain lend %x2
  %c = call @g(%x2)
  call @pair(%n, %c)
  %w = load %x
  release %w
  ret
}
)";
  Lent l = movedThenLent(text);
  EXPECT_NE(f(l).find("retain %n."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.KeptForWindow, 1u);
  // g given a copy (x read on afterwards): both lent.
  Lent copy = lent(text);
  EXPECT_EQ(count(f(copy), "retain"), 0u) << f(copy);
  EXPECT_EQ(copy.Counts.Lent, 2u);
}

/// @f of `f(x.name, <e>)` at x's last use, where <e> hands x to @g in a
/// region nested in the window: @p region is the region's code, with
/// `{x}` where @g is called on a copy of x into %r (stored into %t).
std::string nested(const std::string &region) {
  std::string body = region;
  const std::string call = "    %x2 = load %x\n    retain lend %x2\n"
                           "    %r = call @g(%x2)\n    store %t, %r\n";
  for (size_t at = body.find("{x}"); at != std::string::npos;
       at = body.find("{x}"))
    body.replace(at, 3, call);
  return R"(
fn @f(%a: box) -> void {
  local %x: box
  local %t: bool
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
  %c = call @cond()
)" + body +
         R"(  %b = load %t
  call @pair(%n, %b)
  %w = load %x
  release %w
  ret
}
)";
}

TEST(Lend, AHandOffInsideAnAndAnOrOrATernaryKeepsTheCopy) {
  // f(x.name, c && g(x)), f(x.name, c || g(x)), f(x.name, c ? g(x) :
  // False) and a match: the move of x into g is inside the `if` (or the
  // `switch`) the operator lowers to, in the window though not in its
  // block (crit/c1_andand_window).
  const std::string shapes[] = {
      "  if %c {\n{x}  } else {\n    store %t, false\n  }\n",
      "  if %c {\n    store %t, true\n  } else {\n{x}  }\n",
      R"(  %k = select %c, 1, 0
  switch %k {
    case 0 {
      store %t, false
    }
    default {
{x}    }
  }
)",
      "  while {\n    cond %c\n  } {\n{x}    break\n  }\n",
  };
  for (const std::string &shape : shapes) {
    Lent l = movedThenLent(nested(shape));
    EXPECT_EQ(l.Moves.Transfers, 1u) << shape;
    EXPECT_NE(f(l).find("retain %n."), std::string::npos) << shape << f(l);
    EXPECT_NE(f(l).find("call @pair(%n.4,"), std::string::npos)
        << shape << f(l);
    EXPECT_EQ(l.Counts.KeptForWindow, 1u) << shape;
    EXPECT_EQ(l.Counts.Lent, 0u) << shape;
  }
}

TEST(Lend, AVariableCopiedBeforeAnAndThatHandsItOnIsKept) {
  // f(x, c && g(x)) at x's last use (crit/c1b_andand_var): the first
  // argument is a copy of x itself, and x goes to g inside the `&&`.
  Lent l = movedThenLent(R"(
fn @f(%a: box) -> void {
  local %x: box
  local %t: bool
  store %x, %a
  %v = load %x
  retain lend %v
  %c = call @cond()
  if %c {
    %x2 = load %x
    retain lend %x2
    %r = call @g(%x2)
    store %t, %r
  } else {
    store %t, false
  }
  %b = load %t
  call @pair(%v, %b)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_NE(f(l).find("retain %v."), std::string::npos) << f(l);
  EXPECT_NE(f(l).find("call @pair(%v.2,"), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.KeptForWindow, 1u);
}

TEST(Lend, ACopyInANestedRegionIsNoHandOff) {
  // f(x.name, c && g(x)) with x read after the call: g is given a copy, a
  // reference of its own, and x keeps the object; both are lent.
  Lent l = lent(nested("  if %c {\n{x}  } else {\n    store %t, false\n  }\n"));
  EXPECT_EQ(count(f(l), "retain"), 0u) << f(l);
  EXPECT_NE(f(l).find("call @pair(lent %n.4,"), std::string::npos) << f(l);
  EXPECT_NE(f(l).find("call @g(lent %x2."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Lent, 2u);
}

TEST(Lend, AnyWriteOrHandOffOfTheRootInTheWindowKeepsTheCopy) {
  // What may let the root's object go between the read and the call, each
  // on its own: a store into the root (a reassignment, the moves' null),
  // its address taken, a release of a load of it, its box cast, and a
  // load of it handed on without a retain (a store, a call in a nested
  // arm).
  const std::string between[] = {
      "  %y = load %x\n  release %y\n  store %x, null box\n",
      "  %ad = local.addr %x\n  call @poke(%ad)\n",
      "  %y = load %x\n  release %y\n",
      "  %y = load %x\n  %yi = cast %y to i64\n",
      "  %y = load %x\n  store %other, %y\n",
      R"(  %c = call @cond()
  if %c {
    %y = load %x
    call @take(%y)
  }
)",
  };
  for (const std::string &code : between) {
    Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  local %other: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
)" + code + R"(  call @take(%n)
  ret
}
)");
    EXPECT_NE(f(l).find("retain %n."), std::string::npos) << code << f(l);
    EXPECT_EQ(l.Counts.KeptForWindow, 1u) << code;
  }
}

TEST(Lend, AWriteThroughOrAHandOffOfThePointerRootKeepsTheCopy) {
  // An `inout` name: a call given its pointer (an `inout` argument again),
  // a `ptr.store` through it, may replace what it names.
  const std::string between[] = {
      "  %q2 = load %n\n  call @poke(%q2)\n",
      "  %q2 = load %n\n  ptr.store %q2, null box\n",
      "  %q2 = load %n\n  %q3 = ptr.add %q2, 0\n  call @poke(%q3)\n",
  };
  for (const std::string &code : between) {
    Lent l = lent(R"(
fn @f(%p: ptr) -> void {
  local %n: ptr
  store %n, %p
  %q = load %n
  %b = ptr.load box, %q
  retain lend %b
)" + code + R"(  call @take(%b)
  ret
}
)");
    EXPECT_NE(f(l).find("retain %b."), std::string::npos) << code << f(l);
    EXPECT_EQ(l.Counts.KeptForWindow, 1u) << code;
  }
}

TEST(Lend, AStoreIntoAFieldTheReadReadsKeepsTheCopy) {
  // The code of the caller itself writing the field read, of any object.
  Lent l = lent(R"(
fn @f(%a: box, %b: box) -> void {
  local %x: box
  store %x, %a
  %x1 = load %x
  %o = unbox %x1
  %n = field.load %o, Node.name
  retain lend %n
  %bo = unbox %b
  field.store %bo, Node.name, null box
  call @take(%n)
  ret
}
)");
  EXPECT_NE(f(l).find("retain %n."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.KeptForWindow, 1u);
}

TEST(Lend, AnythingAfterTheCallIsOutsideTheWindow) {
  // The callee has returned: x may be written and handed on after it.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  call @take(%v)
  %y = load %x
  call @take(%y)
  store %x, null box
  ret
}
)");
  EXPECT_NE(f(l).find("call @take(lent %v."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

// -- What is no copy to lend

TEST(Lend, AMarkThatIsNotACopyForALendingCallsBoxParameterStays) {
  // Each is kept and cleared, with no window to check: a runtime callee
  // (no mask), a value used again after the call, a read whose root is
  // in another block, a read through a `select`, a call in a nested
  // block.
  const std::string bodies[] = {
      R"(  %v = load %x
  retain lend %v
  %u = load %x
  %uo = unbox %u
  %e = call @$rt.PaykanObject_equals(%uo, %v, 1)
)",
      R"(  %v = load %x
  retain lend %v
  call @take(%v)
  %vo = unbox %v
)",
      R"(  %x1 = load %x
  %o = unbox %x1
  %c = call @cond()
  if %c {
    %v = field.load %o, Node.name
    retain lend %v
    call @take(%v)
  }
)",
      R"(  %v = load %x
  %vo = unbox %v
  %n = field.load %vo, Node.next
  %no = unbox %n
  %m = field.load %no, Node.name
  %s = select true, %m, %m
  retain lend %s
  call @take(%s)
)",
      R"(  %v = load %x
  retain lend %v
  %c = call @cond()
  if %c {
    call @take(%v)
  }
)",
  };
  for (const std::string &body : bodies) {
    Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
)" + body + R"(  %w = load %x
  release %w
  ret
}
)");
    EXPECT_EQ(count(f(l), "lend"), 0u) << body << f(l);
    EXPECT_EQ(count(f(l), "lent"), 0u) << body << f(l);
    EXPECT_EQ(count(f(l), "retain"), 1u) << body << f(l);
    EXPECT_EQ(l.Counts.Marks, 1u) << body;
    EXPECT_EQ(l.Counts.Lent, 0u) << body;
    EXPECT_EQ(l.Counts.KeptForWindow, 0u) << body;
  }
}

TEST(Lend, TheSixtyFifthBoxParameterIsAlwaysHandedOver) {
  // It has no bit in the ownership mask: its copy stays; the 64th's goes.
  std::ostringstream os;
  os << "fn @wide(";
  for (int i = 0; i < 65; ++i)
    os << (i ? ", " : "") << "%p" << i << ": box";
  os << ") -> void {\n  ret\n}\n\nfn @f(%a: box) -> void {\n"
     << "  local %x: box\n  store %x, %a\n";
  for (int i = 0; i < 65; ++i)
    os << "  %v" << i << " = load %x\n"
       << (i >= 63 ? "  retain lend %v" + std::to_string(i) + "\n"
                   : "  retain %v" + std::to_string(i) + "\n");
  os << "  call @wide(";
  for (int i = 0; i < 65; ++i)
    os << (i ? ", " : "") << "%v" << i;
  os << ")\n  %w = load %x\n  release %w\n  ret\n}\n";
  Lent l = lent(os.str());
  EXPECT_NE(f(l).find("lent %v63."), std::string::npos) << f(l);
  EXPECT_EQ(f(l).find("lent %v64."), std::string::npos) << f(l);
  EXPECT_NE(f(l).find("retain %v64."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Marks, 2u);
  EXPECT_EQ(l.Counts.Lent, 1u);
}

TEST(Lend, NoTerminatorComesBetweenAReadAndItsCall) {
  // A statement after a `ret` in its block is not PIR (the verifier says
  // so), so no read is lent across an early return: the lowering releases
  // a copy on a path that leaves before the call, which is then one more
  // use of it.  Such code, should a pass see it, keeps its copy.
  const std::string text = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  ret
  call @take(%v)
  ret
}
)";
  ParseError err;
  auto p = parseProgram(kPrelude + text, err);
  ASSERT_TRUE(p) << err.str();
  auto errors = verify(*p);
  ASSERT_FALSE(errors.empty());
  EXPECT_NE(formatErrors(errors).find("follows a terminator"),
            std::string::npos)
      << formatErrors(errors);
  Module &m = p->Modules.front();
  for (Function &fn : m.Functions)
    if (fn.Name == "f") {
      const LendCounts c = lendArguments(fn, m);
      EXPECT_EQ(c.Marks, 1u);
      EXPECT_EQ(c.Lent, 0u);
    }
  EXPECT_EQ(count(toString(*p), "retain %v"), 1u) << toString(*p);
  EXPECT_EQ(count(toString(*p), "lent"), 0u) << toString(*p);
  // A path out of the window in a nested arm releases the copy there.
  Lent l = lent(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain lend %v
  %c = call @cond()
  if %c {
    release %v
    %w = load %x
    release %w
    ret
  }
  call @take(%v)
  %z = load %x
  release %z
  ret
}
)");
  EXPECT_NE(f(l).find("retain %v."), std::string::npos) << f(l);
  EXPECT_EQ(l.Counts.Lent, 0u);
}

// -- The callee half: which parameters are lent

/// What lend did to each function, by name.
struct Every {
  Program P;
  std::map<std::string, LendCounts> By;
  std::string Error;
};

/// The prelude and @p text parsed and verified; then, on every function,
/// moveAtLastUse when @p moveFirst and lend (both halves); and the result
/// verified.  Error: what lend reported.
Every lendEvery(const std::string &text, bool moveFirst = false) {
  Every e;
  ParseError err;
  auto p = parseProgram(kPrelude + text, err);
  EXPECT_TRUE(p) << err.str();
  if (!p)
    return e;
  e.P = std::move(*p);
  auto errors = verify(e.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  Module &m = e.P.Modules.front();
  for (Function &fn : m.Functions) {
    if (moveFirst)
      moveAtLastUse(fn);
    StatusOr<LendCounts> c = lend(fn, m);
    if (!c) {
      e.Error = c.status().message();
      return e;
    }
    e.By[fn.Name] = *c;
  }
  errors = verify(e.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors) << toString(e.P);
  return e;
}

/// The printed function @p name of @p e.
std::string printed(const Every &e, const std::string &name = "f") {
  const std::string text = toString(e.P);
  const size_t start = text.find("fn @" + name + "(");
  if (start == std::string::npos)
    return "";
  return text.substr(start, text.find("\n}\n", start) + 3 - start);
}

/// The prologue of box parameter %@p p: `%<p>o = owned %p`, `%<p>l = not
/// %<p>o`, `if %<p>l { retain %p }`.
std::string prologue(const std::string &p) {
  return "  %" + p + "o = owned %" + p + "\n  %" + p + "l = not %" + p +
         "o\n  if %" + p + "l {\n    retain %" + p + "\n  }\n";
}

/// A class Cell with one box field, v.
const std::string kCell = R"(
class Cell {
  field v: box
  vtable {
    destroy = @Cell.destroy : (obj) -> void
  }
}

fn @Cell.destroy(%self: obj) -> void {
  %v = field.load %self, Cell.v
  release %v
  free %self
  ret
}
)";

TEST(LendParams, ANeverWrittenParameterIsReleasedOnlyWhenItWasHandedOver) {
  // The prologue's retain goes; each exit's release of the slot asks the
  // bit.  A lent argument it lends on, and a borrow, change nothing.
  Every e = lendEvery(R"(
fn @f(%a: box) -> void {
  local %x: box
)" + prologue("a") + R"(  store %x, %a
  %x1 = load %x
  call @take(lent %x1)
  %c = call @cond()
  if %c {
    %w = load %x
    release %w
    ret
  }
  %z = load %x
  %zo = unbox %z
  release %z
  ret
}
)");
  EXPECT_EQ(printed(e), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  %ao.2 = owned %a.1
  store %x.0, %a.1
  %x1.4 = load %x.0
  call @take(lent %x1.4)
  %c.5 = call @cond()
  if %c.5 {
    %w.6 = load %x.0
    if %ao.2 {
      release %w.6
    }
    ret
  }
  %z.7 = load %x.0
  %zo.8 = unbox %z.7
  if %ao.2 {
    release %z.7
  }
  ret
}
)");
  const LendCounts &c = e.By["f"];
  EXPECT_EQ(c.ParamsLent, 1u);
  EXPECT_EQ(c.ParamsHandedOn, 0u);
  EXPECT_EQ(c.ParamsOwnedOnEntry, 0u);
  EXPECT_EQ(c.RetainsAtKeeps, 0u);
  // The prelude's @take releases what it was handed over only.
  EXPECT_NE(printed(e, "take").find("  %po.2 = owned %p.1\n"
                                    "  if %po.2 {\n"
                                    "    release %p.1\n"
                                    "  }\n"),
            std::string::npos)
      << printed(e, "take");
}

TEST(LendParams, AHandOffPassesTheBitOnAndTheLastKeeperRetains) {
  // push -> pushFront -> Cell -> Cell.__init__ -> the field: each hands its
  // parameter on with its own bit (owned(%o)); only the field's store
  // retains, and only when the outermost caller lent.  A function value's
  // code hands its parameter to its target the same way.
  Every e = lendEvery(kCell + R"(
fn @push(%s: ptr, %v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  call @pushFront(%s, %v1)
  ret
}

fn @pushFront(%s: ptr, %v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  %n = call @Cell(%v1)
  ptr.store %s, %n
  ret
}

fn @Cell(%v: box) -> box {
)" + prologue("v") + R"(  %o = new Cell
  %b = box %o
  call @Cell.__init__(%o, %v)
  ret %b
}

fn @Cell.__init__(%self: obj, %v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  field.store %self, Cell.v, %v1
  ret
}

fn @fn.ref.m.take.call(%closure: obj, %p: box) -> void {
)" + prologue("p") + R"(  call @take(%p)
  ret
}
)");
  EXPECT_NE(
      printed(e, "push").find("call @pushFront(%s.1, owned(%vo.3) %v1.5)"),
      std::string::npos)
      << printed(e, "push");
  EXPECT_NE(printed(e, "pushFront").find("call @Cell(owned(%vo.3) %v1.5)"),
            std::string::npos)
      << printed(e, "pushFront");
  EXPECT_NE(
      printed(e, "Cell").find("call @Cell.__init__(%o.4, owned(%vo.2) %v.1)"),
      std::string::npos)
      << printed(e, "Cell");
  EXPECT_NE(printed(e, "Cell.__init__")
                .find("  %v1.5 = load %vp.0\n"
                      "  if %vl.4 {\n"
                      "    retain %v1.5\n"
                      "  }\n"
                      "  field.store %self.1, Cell.v, %v1.5\n"),
            std::string::npos)
      << printed(e, "Cell.__init__");
  EXPECT_NE(
      printed(e, "fn.ref.m.take.call").find("call @take(owned(%po.3) %p.2)"),
      std::string::npos)
      << printed(e, "fn.ref.m.take.call");
  for (const char *f : {"push", "pushFront", "Cell", "fn.ref.m.take.call"}) {
    EXPECT_EQ(e.By[f].ParamsHandedOn, 1u) << f;
    EXPECT_EQ(e.By[f].RetainsAtKeeps, 0u) << f;
    EXPECT_EQ(count(printed(e, f), "retain"), 0u) << printed(e, f);
  }
  EXPECT_EQ(e.By["Cell.__init__"].ParamsLent, 1u);
  EXPECT_EQ(e.By["Cell.__init__"].RetainsAtKeeps, 1u);
}

TEST(LendParams, AKeepRetainsWhenTheParameterWasLent) {
  // What takes the parameter's reference without asking its bit: a field,
  // another local, memory, `ret`, and a callee without the parameter's
  // bit (the runtime's `equals`, which releases the box it is given, and
  // a 65th box parameter).  Each is preceded by a retain when the
  // parameter was lent; the copies the body makes stay as they are.
  std::ostringstream wide;
  wide << "fn @wide(";
  for (int i = 0; i < 65; ++i)
    wide << (i ? ", " : "") << "%w" << i << ": box";
  wide << ") -> void {\n  ret\n}\n";
  Every e = lendEvery(kCell + wide.str() + R"(
fn @field(%self: obj, %v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %c = load %vp
  retain %c
  call @take(%c)
  %v1 = load %vp
  field.store %self, Cell.v, %v1
  ret
}

fn @local(%v: box) -> void {
  local %vp: box
  local %y: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  store %y, %v1
  %y1 = load %y
  release %y1
  ret
}

fn @memory(%v: box, %out: ptr) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  ptr.store %out, %v1
  ret
}

fn @id(%v: box) -> box {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  ret %v1
}

fn @equals(%self: obj, %v: box) -> i64 {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  %r = call @$rt.PaykanObject_equals(%self, %v1, 1)
  ret %r
}

fn @last(%v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  call @wide()" + [] {
    std::string args;
    for (int i = 0; i < 64; ++i)
      args += "null box, ";
    return args;
  }() + R"(%v1)
  ret
}
)");
  auto kept = [&](const std::string &f, const std::string &v,
                  const std::string &consumer) {
    const std::string text = printed(e, f);
    const std::string guarded = "    retain " + v + "\n  }\n  " + consumer;
    EXPECT_NE(text.find(guarded), std::string::npos) << guarded << "\n" << text;
    EXPECT_EQ(count(text, "if %vl."), 1u) << text;
    EXPECT_EQ(count(text, "retain %v."), 0u) << text;
    EXPECT_EQ(e.By[f].ParamsLent, 1u) << f;
    EXPECT_EQ(e.By[f].RetainsAtKeeps, 1u) << f;
  };
  kept("field", "%v1.6", "field.store %self.1, Cell.v, %v1.6\n");
  kept("local", "%v1.4", "store %y.1, %v1.4\n");
  kept("memory", "%v1.5", "ptr.store %out.2, %v1.5\n");
  kept("id", "%v1.4", "ret %v1.4\n");
  kept("equals", "%v1.5",
       "%r.6 = call @$rt.PaykanObject_equals(%self.1, %v1.5, 1)\n");
  kept("last", "%v1.4", "call @wide(");
  // The copy into @take stays a copy.
  EXPECT_NE(printed(e, "field").find("  retain %c.5\n  call @take(%c.5)\n"),
            std::string::npos)
      << printed(e, "field");
  // The local the parameter is kept in releases what it holds, as before.
  EXPECT_NE(printed(e, "local").find("  %y1.5 = load %y.1\n  release %y1.5\n"),
            std::string::npos)
      << printed(e, "local");
}

TEST(LendParams, AParameterTheBodyWritesKeepsItsPrologue) {
  // Each way w1_written's callees write a parameter, as the lowering
  // writes it: a value stored into the slot (an assignment, `s += "!"`,
  // destructuring), the slot's address (an `inout` argument, `r: inout =
  // n`, a match arm's `inout` binding).  The body owns the parameter from
  // its entry, so nothing changes.  `n = None` stores null: no write; the
  // release of what it replaced asks the bit.
  Every e = lendEvery(R"(
fn @mk() -> box {
  ret null box
}

fn @assign(%a: box) -> void {
  local %x: box
)" + prologue("a") + R"(  store %x, %a
  %n = call @mk()
  %old = load %x
  release %old
  store %x, %n
  %z = load %x
  release %z
  ret
}

fn @viaInout(%a: box) -> void {
  local %x: box
)" + prologue("a") + R"(  store %x, %a
  %xa = local.addr %x
  call @poke(%xa)
  %z = load %x
  release %z
  ret
}

fn @toNone(%a: box) -> void {
  local %x: box
)" + prologue("a") + R"(  store %x, %a
  %old = load %x
  release %old
  store %x, null box
  %z = load %x
  release %z
  ret
}
)");
  for (const char *f : {"assign", "viaInout"}) {
    EXPECT_NE(printed(e, f).find("  %ao.2 = owned %a.1\n"
                                 "  %al.3 = not %ao.2\n"
                                 "  if %al.3 {\n"
                                 "    retain %a.1\n"
                                 "  }\n"),
              std::string::npos)
        << printed(e, f);
    EXPECT_EQ(printed(e, f).find("if %ao"), std::string::npos) << printed(e, f);
    EXPECT_EQ(e.By[f].ParamsOwnedOnEntry, 1u) << f;
    EXPECT_EQ(e.By[f].ParamsLent, 0u) << f;
  }
  EXPECT_EQ(printed(e, "toNone"), R"(fn @toNone(%a.1: box) -> void {
  local %x.0: box
  %ao.2 = owned %a.1
  store %x.0, %a.1
  %old.4 = load %x.0
  if %ao.2 {
    release %old.4
  }
  store %x.0, null box
  %z.5 = load %x.0
  if %ao.2 {
    release %z.5
  }
  ret
}
)");
  EXPECT_EQ(e.By["toNone"].ParamsLent, 1u);
}

TEST(LendParams, EveryWayOutOfTheBodyAsksTheBit) {
  // An early return, a `break` and a `continue` out of a loop, the arm a
  // release was moved into beside the arm that hands the parameter on:
  // each path lets go of the parameter's reference once, and each place
  // that does asks the bit.
  Every e = lendEvery(R"(
fn @f(%a: box) -> void {
  local %x: box
)" + prologue("a") + R"(  store %x, %a
  while {
    %c = call @cond()
    cond %c
  } {
    %d = call @cond()
    if %d {
      %w = load %x
      release %w
      ret
    }
    %e = call @cond()
    if %e {
      break
    }
    continue
  }
  %k = call @cond()
  if %k {
    %y = load %x
    store %x, null box
    call @take(%y)
  } else {
    %z = load %x
    release %z
  }
  ret
}
)");
  const std::string out = printed(e);
  EXPECT_NE(out.find("      %w.6 = load %x.0\n"
                     "      if %ao.2 {\n"
                     "        release %w.6\n"
                     "      }\n"
                     "      ret\n"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("    call @take(owned(%ao.2) %y.9)\n"), std::string::npos)
      << out;
  EXPECT_NE(out.find("    %z.10 = load %x.0\n"
                     "    if %ao.2 {\n"
                     "      release %z.10\n"
                     "    }\n"),
            std::string::npos)
      << out;
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(e.By["f"].ParamsHandedOn, 1u);
}

TEST(LendParams, AParameterWithoutASlotIsLentWhereItIsUsed) {
  // A constructor's parameter is handed straight to `__init__`; a native
  // function's body unboxes its parameter for C and releases it: neither
  // stores it in a slot.
  Every e = lendEvery(R"(
extern fn @$c.put(obj) -> i64

fn @put(%s: box) -> i64 {
)" + prologue("s") + R"(  %o = unbox %s
  %r = call @$c.put(%o)
  release %s
  ret %r
}
)");
  EXPECT_EQ(printed(e, "put"), R"(fn @put(%s.1: box) -> i64 {
  %so.2 = owned %s.1
  %o.4 = unbox %s.1
  %r.5 = call @$c.put(%o.4)
  if %so.2 {
    release %s.1
  }
  ret %r.5
}
)");
  EXPECT_EQ(e.By["put"].ParamsLent, 1u);
}

TEST(LendParams, ABoxLentToCThroughATemporaryIsNoKeep) {
  // A native function's argument passed by address: the body stores the
  // parameter into a local only C reads, through its address, and then
  // releases its own reference.  The store keeps nothing.
  Every e = lendEvery(R"(
extern fn @$c.set(ptr) -> void

fn @set(%v: box) -> void {
  local %vp: box view
  local %vv: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  store %vv, %v1
  %va = local.addr %vv
  call @$c.set(%va)
  %z = load %vp
  release %z
  ret
}
)");
  const std::string out = printed(e, "set");
  EXPECT_NE(out.find("  store %vv.1, %v1.4\n"), std::string::npos) << out;
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_NE(out.find("  if %vo.2 {\n    release %z.6\n  }\n"),
            std::string::npos)
      << out;
  EXPECT_EQ(e.By["set"].ParamsLent, 1u);
}

TEST(LendParams, AReferenceLetGoOfTwiceOnAPathKeepsThePrologue) {
  // Each path lets go of the parameter's reference once in a body the
  // lowering made; where what the pass takes for that does not add up
  // (here a store into a local that is never released, handed to a
  // Paykan function by address, and the slot's release on the same path),
  // the pass does not know which one does, and the parameter is owned
  // from its entry, as before.
  Every e = lendEvery(R"(
fn @f(%v: box) -> void {
  local %vp: box
  local %t: box
)" + prologue("v") + R"(  store %vp, %v
  %v1 = load %vp
  store %t, %v1
  %ta = local.addr %t
  call @poke(%ta)
  %z = load %vp
  release %z
  ret
}

fn @g2(%v: box) -> void {
  local %vp: box
)" + prologue("v") + R"(  store %vp, %v
  %c = call @cond()
  if %c {
    %y = load %vp
    call @take(%y)
  }
  ret
}
)");
  for (const char *f : {"f", "g2"}) {
    EXPECT_NE(printed(e, f).find("    retain %v.1\n"), std::string::npos)
        << printed(e, f);
    EXPECT_EQ(e.By[f].ParamsOwnedOnEntry, 1u) << f;
    EXPECT_EQ(count(printed(e, f), "owned("), 0u) << printed(e, f);
  }
}

TEST(LendParams, ABodyWithoutItsPrologueIsAnInternalError) {
  // Lent, a body that releases its parameter without asking its bit would
  // release the caller's reference: the lowering starts every body with
  // its prologues, and the pass says when one does not.  A parameter the
  // body only borrows, and one the pass lent already (its `owned` stays),
  // are fine; so is the 65th box parameter, which has no bit.
  Every bad = lendEvery(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %z = load %x
  release %z
  ret
}
)");
  EXPECT_NE(bad.Error.find("internal compiler error: @f takes its box "
                           "parameter %a.1 as its own"),
            std::string::npos)
      << bad.Error;
  std::ostringstream wide;
  wide << "fn @wide(";
  for (int i = 0; i < 65; ++i)
    wide << (i ? ", " : "") << "%w" << i << ": box";
  wide << ") -> void {\n  release %w64\n  ret\n}\n";
  Every ok = lendEvery(wide.str() + R"(
fn @borrows(%a: box) -> bool {
  %o = unbox %a
  %n = cmp eq %o, null obj
  ret %n
}

fn @lent(%a: box) -> void {
  %ao = owned %a
  if %ao {
    release %a
  }
  ret
}
)");
  EXPECT_EQ(ok.Error, "");
  EXPECT_EQ(printed(ok, "lent"), R"(fn @lent(%a.1: box) -> void {
  %ao.2 = owned %a.1
  if %ao.2 {
    release %a.1
  }
  ret
}
)");
}

TEST(LendParams, TheCodePassesChangeNothingOnTheirOutput) {
  // A module's code, lent before its `.pkm` is written, goes through the
  // program's passes again: the moves find no copy in a keep's
  // conditional retain and no release to move to in a conditional one,
  // the lending finds no prologue to lend and no mark, and the
  // elimination finds no pair.
  const std::string text = kPrelude + kCell + R"(
fn @f(%a: box, %b: box, %s: obj) -> void {
  local %x: box
  local %y: box
)" + prologue("a") + prologue("b") +
                           R"(  store %x, %a
  store %y, %b
  %x1 = load %x
  retain lend %x1
  call @take(%x1)
  %x2 = load %x
  retain %x2
  call @take(%x2)
  %y1 = load %y
  field.store %s, Cell.v, %y1
  %c = call @cond()
  if %c {
    %x3 = load %x
    call @take(%x3)
    ret
  }
  %x4 = load %x
  release %x4
  ret
}
)";
  ParseError err;
  auto p = parseProgram(text, err);
  ASSERT_TRUE(p) << err.str();
  PassManager once(/*verifyEach=*/true);
  once.add(createMovesAtLastUsePass());
  once.add(createLendingPass());
  once.add(createRCEliminationPass());
  Statistics first;
  ASSERT_TRUE(once.run(*p, first));
  EXPECT_GT(first.get("lending", "parameters handed on"), 0u);
  EXPECT_EQ(first.get("lending", "retains at keeps"), 1u);
  const std::string after = toString(*p);
  PassManager again(/*verifyEach=*/true);
  again.add(createMovesAtLastUsePass());
  again.add(createLendingPass());
  again.add(createRCEliminationPass());
  again.add(createLendingPass());
  Statistics second;
  ASSERT_TRUE(again.run(*p, second));
  EXPECT_EQ(toString(*p), after);
  EXPECT_EQ(second.get("moves-at-last-use", "transfers"), 0u);
  EXPECT_EQ(second.get("lending", "parameters lent"), 0u);
  EXPECT_EQ(second.get("lending", "parameters handed on"), 0u);
  EXPECT_EQ(second.get("lending", "retains at keeps"), 0u);
  EXPECT_EQ(second.get("rc-elimination", "retains removed"), 0u);
}

// -- The pass

TEST(Lend, ThePassLendsEveryFunctionAndCountsWhatItDid) {
  ParseError err;
  auto p = parseProgram(kPrelude + R"(
fn @f(%a: box) -> void {
  local %x: box
  %ao = owned %a
  %al = not %ao
  if %al {
    retain %a
  }
  store %x, %a
  %v = load %x
  retain lend %v
  call @take(%v)
  %v2 = load %x
  retain lend %v2
  %u = load %x
  call @two(%v2, %u)
  ret
}
)",
                        err);
  ASSERT_TRUE(p) << err.str();
  PassManager passes(/*verifyEach=*/true);
  passes.add(createLendingPass());
  Statistics stats;
  ASSERT_TRUE(passes.run(*p, stats));
  EXPECT_EQ(stats.get("lending", "marks"), 2u);
  EXPECT_EQ(stats.get("lending", "arguments lent"), 1u);
  EXPECT_EQ(stats.get("lending", "copies kept for the window"), 1u);
  EXPECT_EQ(count(toString(*p), " lend "), 0u) << toString(*p);
}

TEST(Lend, TheMovesAndTheLendingChangeNothingOnTheirOutput) {
  // lend(lend(x)) == lend(x): no mark is left.  moves(lend(moves(x))) ==
  // lend(moves(x)): a lent argument is no copy the moves could make a
  // hand-off, so a module's code, lent before its `.pkm` is written, goes
  // through the program's passes unchanged.
  const std::string text = kPrelude + R"(
fn @f(%a: box, %p: ptr) -> void {
  local %x: box
  local %t: bool
  local %n: ptr
  %ao = owned %a
  %al = not %ao
  if %al {
    retain %a
  }
  store %x, %a
  store %n, %p
  %v = load %x
  retain lend %v
  call @take(%v)
  %x1 = load %x
  %o = unbox %x1
  %nm = field.load %o, Node.name
  retain lend %nm
  %c = call @cond()
  if %c {
    %x2 = load %x
    retain lend %x2
    %r = call @g(%x2)
    store %t, %r
  } else {
    store %t, false
  }
  %b = load %t
  call @pair(%nm, %b)
  %q = load %n
  %e = ptr.load box, %q
  retain lend %e
  call @take(%e)
  %y = load %x
  retain lend %y
  %z = load %x
  retain lend %z
  call @two(%y, %z)
  %w = load %x
  release %w
  ret
}
)";
  ParseError err;
  auto p = parseProgram(text, err);
  ASSERT_TRUE(p) << err.str();
  PassManager once(/*verifyEach=*/true);
  once.add(createMovesAtLastUsePass());
  once.add(createLendingPass());
  Statistics first;
  ASSERT_TRUE(once.run(*p, first));
  EXPECT_GT(first.get("lending", "arguments lent"), 0u);
  EXPECT_EQ(first.get("lending", "copies kept for the window"), 1u);
  const std::string after = toString(*p);
  PassManager again(/*verifyEach=*/true);
  again.add(createLendingPass());
  again.add(createMovesAtLastUsePass());
  again.add(createLendingPass());
  Statistics second;
  ASSERT_TRUE(again.run(*p, second));
  EXPECT_EQ(toString(*p), after);
  EXPECT_EQ(second.get("lending", "marks"), 0u);
  EXPECT_EQ(second.get("moves-at-last-use", "transfers"), 0u);
}
