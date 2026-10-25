// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (docs/pir.md §12) on hand-written PIR: which copies of a
// box argument it lends, which it keeps because a statement from the read
// to the call, or one nested in those, may let the caller's object go (the
// window), and which it keeps because the copy is no read of a local the
// call could be lent; and that the moves at last use and the pass change
// nothing on the pass's own output.

#include "paykan/pir/Lend.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::pir;

namespace {

// What every program below calls.  The callees lend (a box parameter, a
// definition): @take and @g keep nothing, @two takes two boxes, @pair a box
// and a bool, @poke a pointer (an `inout` argument); Node's slot `look`
// takes a box too.  The runtime's `equals` does not lend.
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
  release %p
  ret
}

fn @take(%p: box) -> void {
  release %p
  ret
}

fn @two(%p: box, %q: box) -> void {
  release %p
  release %q
  ret
}

fn @pair(%p: box, %b: bool) -> void {
  release %p
  ret
}

fn @g(%p: box) -> bool {
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
    l.Counts = lend(f, m);
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
      const LendCounts c = lend(fn, m);
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

// -- The pass

TEST(Lend, ThePassLendsEveryFunctionAndCountsWhatItDid) {
  ParseError err;
  auto p = parseProgram(kPrelude + R"(
fn @f(%a: box) -> void {
  local %x: box
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
