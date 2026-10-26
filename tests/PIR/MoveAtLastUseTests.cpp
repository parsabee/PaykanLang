// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The moves-at-last-use pass (docs/pir.md §12) on hand-written PIR: each
// shape of copy it moves, each reason it leaves one alone, and the cleanup
// of the releases and null stores a move makes useless.

#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <ctime>
#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::pir;

namespace {

// What every program below calls: @take releases its argument (an owned
// argument, as a user function's is), @use borrows one, @cond is a test.
const std::string kPrelude = R"(module "m"
extern fn @$rt.Paykan_panic_index(i64, i64) -> void

class Node {
  field next: box
  vtable {
    destroy = @Node.destroy : (obj) -> void
  }
}

class fn.m.0 {
  field c0: box
  vtable {
    destroy = @fn.m.0.destroy : (obj) -> void
  }
}

fn @Node.destroy(%self: obj) -> void {
  %n = field.load %self, Node.next
  release %n
  free %self
  ret
}

fn @fn.m.0.destroy(%self: obj) -> void {
  %c = field.load %self, fn.m.0.c0
  release %c
  free %self
  ret
}

fn @take(%p: box) -> void {
  release %p
  ret
}

fn @use(%p: box) -> void {
  ret
}

fn @cond() -> bool {
  ret true
}

fn @main() -> i64 {
  ret 0
}
)";

struct Moved {
  Program P;
  MoveCounts Counts;
};

/// The prelude and @p text parsed and verified; then moveAtLastUse run on
/// the function @p name and the result verified.  Called as `::move` with a
/// std::string, which std::move would otherwise take.
Moved move(const std::string &text, const std::string &name = "f") {
  Moved m;
  ParseError err;
  auto p = parseProgram(kPrelude + text, err);
  EXPECT_TRUE(p) << err.str();
  if (!p)
    return m;
  m.P = std::move(*p);
  auto errors = verify(m.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  for (Function &f : m.P.Modules.front().Functions)
    if (f.Name == name)
      m.Counts = moveAtLastUse(f);
  errors = verify(m.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors) << toString(m.P);
  return m;
}

/// The printed function @p name of @p m.
std::string f(const Moved &m, const std::string &name = "f") {
  const std::string text = toString(m.P);
  const size_t start = text.find("fn @" + name + "(");
  if (start == std::string::npos)
    return "";
  return text.substr(start, text.find("\n}\n", start) + 3 - start);
}

/// @f with @p n box locals, each given a new object, handed to @take one
/// after the other at its last use, and released at the end: n copies in
/// a function of about 9n statements.
std::string manyCopies(size_t n) {
  std::ostringstream os;
  os << "fn @f() -> void {\n";
  for (size_t i = 0; i < n; ++i)
    os << "  local %x" << i << ": box\n";
  for (size_t i = 0; i < n; ++i)
    os << "  %o" << i << " = new Node\n  %b" << i << " = box %o" << i
       << "\n  store %x" << i << ", %b" << i << "\n";
  for (size_t i = 0; i < n; ++i)
    os << "  %v" << i << " = load %x" << i << "\n  retain %v" << i
       << "\n  call @take(%v" << i << ")\n";
  for (size_t i = n; i-- > 0;)
    os << "  %w" << i << " = load %x" << i << "\n  release %w" << i << "\n";
  os << "  ret\n}\n";
  return os.str();
}

/// @f, a method, handing a new object on along @p n box locals (`x1 = x0;
/// x2 = x1; ...`), then the last on to @p end (`ret` or `call @take`); the
/// locals are released, the last declared first, before `self` is read.
std::string chainUnderSelf(size_t n, const std::string &end) {
  std::ostringstream os;
  os << "fn @f(%self: obj) -> " << (end == "ret" ? "box" : "void") << " {\n"
     << "  local %s: obj\n";
  for (size_t i = 0; i < n; ++i)
    os << "  local %x" << i << ": box\n";
  os << "  store %s, %self\n  %o = new Node\n  %b = box %o\n"
     << "  store %x0, %b\n";
  for (size_t i = 1; i < n; ++i)
    os << "  %v" << i << " = load %x" << i - 1 << "\n  retain %v" << i
       << "\n  store %x" << i << ", %v" << i << "\n";
  os << "  %r = load %x" << n - 1 << "\n  retain %r\n";
  if (end != "ret")
    os << "  call @take(%r)\n";
  for (size_t i = n; i-- > 0;)
    os << "  %w" << i << " = load %x" << i << "\n  release %w" << i << "\n";
  os << "  %t = load %s\n  %k = field.load %t, Node.next\n"
     << (end == "ret" ? "  ret %r\n" : "  ret\n") << "}\n";
  return os.str();
}

size_t count(const std::string &text, const std::string &needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size()))
    ++n;
  return n;
}

} // namespace

// -- What moves

TEST(MoveAtLastUse, AnArgumentAtItsLastUseIsHandedOn) {
  // The retain goes; the release at scope exit then has nothing to release
  // and goes too, and the slot is never read again.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(f(m), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  store %x.0, %a.1
  %v.2 = load %x.0
  call @take(%v.2)
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 1u);
  EXPECT_EQ(m.Counts.Retains, 1u);
  EXPECT_EQ(m.Counts.Releases, 1u);
}

// A lent argument is no hand-off (docs/pir.md §3): the callee keeps no
// reference of the caller's, so the copy made for it is not the slot's
// reference to move.  An argument passed `owned(%o)` is handed over when an
// owned one is, and a `retain lend` is a retain like any other.
TEST(MoveAtLastUse, ALentArgumentIsNoHandOffAndAPassedOneIs) {
  const std::string lent = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  call @use(lent %v)
  %w = load %x
  release %w
  ret
}
)";
  Moved m = move(lent);
  EXPECT_EQ(m.Counts.Transfers, 0u);
  EXPECT_EQ(count(f(m), "retain %v"), 1u) << f(m);
  Moved passed = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  %o = owned %a
  store %x, %a
  %v = load %x
  retain lend %v
  call @use(owned(%o) %v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(f(passed), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  %o.2 = owned %a.1
  store %x.0, %a.1
  %v.3 = load %x.0
  call @use(owned(%o.2) %v.3)
  ret
}
)");
  EXPECT_EQ(passed.Counts.Transfers, 1u);
}

TEST(MoveAtLastUse, OnlyTheSecondOfTwoUsesInOneCallMoves) {
  // f(a, a): the first copy is followed by a read of the slot, the second
  // by its release.
  Moved m = move(R"(
fn @two(%p: box, %q: box) -> void {
  release %p
  release %q
  ret
}

fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %u = load %x
  retain %u
  call @two(%v, %u)
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 1u) << out;
  EXPECT_NE(out.find("retain %v."), std::string::npos) << out;
  EXPECT_EQ(count(out, "release"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 1u);
}

TEST(MoveAtLastUse, AStoreIntoALocalAFieldOrMemoryIsAConsumer) {
  Moved m = move(R"(
fn @f(%a: box, %b: box, %c: box, %o: obj, %p: ptr) -> void {
  local %x: box
  local %y: box
  local %z: box
  local %t: box
  store %x, %a
  store %y, %b
  store %z, %c
  %v = load %x
  retain %v
  store %t, %v
  %u = load %y
  retain %u
  field.store %o, Node.next, %u
  %s = load %z
  retain %s
  ptr.store %p, %s
  %r1 = load %z
  release %r1
  %r2 = load %y
  release %r2
  %r3 = load %x
  release %r3
  %r4 = load %t
  release %r4
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  // Only %t's own release is left.
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_NE(out.find("= load %t."), std::string::npos) << out;
  EXPECT_EQ(m.Counts.Transfers, 3u);
}

TEST(MoveAtLastUse, ReturningALocalHandsItsReferenceToTheCaller) {
  Moved m = move(R"(
fn @f(%a: box) -> box {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %w = load %x
  release %w
  ret %v
}
)");
  EXPECT_EQ(f(m), R"(fn @f(%a.1: box) -> box {
  local %x.0: box
  store %x.0, %a.1
  %v.2 = load %x.0
  ret %v.2
}
)");
}

TEST(MoveAtLastUse, AReassignmentReleasesFirstSoTheCopyBeforeItMoves) {
  // x = f(x) and the copy of the old value before `x = other`: the release
  // of an assignment is the old value's last event.
  Moved m = move(R"(
fn @g(%p: box) -> box {
  ret %p
}

fn @f(%a: box, %b: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %r = call @g(%v)
  %old = load %x
  release %old
  store %x, %r
  %v2 = load %x
  retain %v2
  call @take(%v2)
  %old2 = load %x
  release %old2
  store %x, %b
  %u = load %x
  call @use(%u)
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  // The last value's release stays.
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 2u);
}

// -- Paths

TEST(MoveAtLastUse, ALastUseInEachArmMovesInEach) {
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = call @cond()
  if %c {
    %v = load %x
    retain %v
    call @take(%v)
  } else {
    %u = load %x
    retain %u
    call @take(%u)
  }
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 0u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 2u);
}

TEST(MoveAtLastUse, AMoveOnOnePathPutsTheReleaseOnTheOther) {
  // Moved in the then arm only: the release after the if goes into the
  // else arm (made for it), where it runs at the same moment as before.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = call @cond()
  if %c {
    %v = load %x
    retain %v
    call @take(%v)
  }
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(f(m), R"(fn @f(%a.1: box) -> void {
  local %x.0: box
  store %x.0, %a.1
  %c.2 = call @cond()
  if %c.2 {
    %v.3 = load %x.0
    call @take(%v.3)
  } else {
    %w.4 = load %x.0
    release %w.4
  }
  ret
}
)");
}

TEST(MoveAtLastUse, OnlyOneArmGetsAReleaseOtherwiseTheSlotIsCleared) {
  // Moved in one case of three: the release stays after the switch (one
  // release, not two), and the moving case clears the slot so it releases
  // nothing there.
  Moved m = move(R"(
fn @f(%a: box, %k: i64) -> void {
  local %x: box
  store %x, %a
  switch %k {
    case 0 {
      %v = load %x
      retain %v
      call @take(%v)
    }
    case 1 {
      %u = load %x
      call @use(%u)
    }
  }
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_EQ(count(out, "store %x.0, null box"), 1u) << out;
  EXPECT_LT(out.find("store %x.0, null box"), out.find("case 1")) << out;
  EXPECT_EQ(m.Counts.Releases, 0u);
}

TEST(MoveAtLastUse, AUseInALoopBodyStaysACopyAndOneAfterTheLoopMoves) {
  // In the body, the next iteration reads the slot again; after the loop,
  // only the scope exit's release follows.
  Moved m = move(R"(
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
    retain %v
    call @take(%v)
    %n2 = load %i
    %n3 = add %n2, 1
    store %i, %n3
  }
  %u = load %x
  retain %u
  call @take(%u)
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 1u) << out;
  EXPECT_NE(out.find("retain %v."), std::string::npos) << out;
  EXPECT_EQ(count(out, "release"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 1u);
}

TEST(MoveAtLastUse, AVariableReassignedBeforeEveryNextReadMovesInALoop) {
  // Each iteration hands the value on, then gives the variable a new one.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  while {
    %c = call @cond()
    cond %c
  } {
    %v = load %x
    retain %v
    call @take(%v)
    %o = new Node
    %b = box %o
    %old = load %x
    release %old
    store %x, %b
  }
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
}

TEST(MoveAtLastUse, ACopyBeforeABreakMovesAndOneBeforeAContinueDoesNot) {
  // After `break` the loop is left and the release after it comes next;
  // after `continue` the next iteration reads the slot again, unless the
  // path reassigns it first.
  const std::string loop = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  while {
    %c = call @cond()
    cond %c
  } {
    %v = load %x
    retain %v
    call @take(%v)
    LEAVE
  }
  %w = load %x
  release %w
  ret
}
)";
  auto with = [&](const std::string &leave) {
    std::string text = loop;
    text.replace(text.find("LEAVE"), 5, leave);
    return ::move(text).Counts.Transfers;
  };
  EXPECT_EQ(with("break"), 1u);
  EXPECT_EQ(with("continue"), 0u);
  EXPECT_EQ(with("%o = new Node\n    %b = box %o\n    %old = load %x\n"
                 "    release %old\n    store %x, %b\n    continue"),
            1u);
}

TEST(MoveAtLastUse, ACopyInANestedLoopFollowsBothLoopsPaths) {
  // The inner loop's copy is left by `break`, then the outer loop
  // reassigns the variable before its next iteration reads it: a move.
  // Without the reassignment the outer loop's next iteration reads it.
  const std::string text = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  while {
    %c = call @cond()
    cond %c
  } {
    while {
      %d = call @cond()
      cond %d
    } {
      %v = load %x
      retain %v
      call @take(%v)
      break
    }
    REASSIGN
  }
  %w = load %x
  release %w
  ret
}
)";
  std::string reassigned = text;
  reassigned.replace(reassigned.find("REASSIGN"), 8,
                     "%o = new Node\n    %b = box %o\n    %old = load %x\n"
                     "    release %old\n    store %x, %b");
  Moved m = ::move(reassigned);
  EXPECT_EQ(m.Counts.Transfers, 1u) << f(m);
  EXPECT_EQ(count(f(m), "retain"), 0u) << f(m);
  std::string kept = text;
  kept.replace(kept.find("REASSIGN"), 8, "%u = load %x\n    call @use(%u)");
  EXPECT_EQ(::move(kept).Counts.Transfers, 0u);
}

TEST(MoveAtLastUse, ALastUseInEveryCaseOfASwitchMovesInEach) {
  // A bool's two cases leave no path to an absent default; the cases of an
  // i64 do, so the release moves into a default made for it.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = call @cond()
  switch %c {
    case true {
      %v = load %x
      retain %v
      call @take(%v)
    }
    case false {
      %u = load %x
      retain %u
      call @take(%u)
    }
  }
  %w = load %x
  release %w
  ret
}
)");
  std::string out = f(m);
  EXPECT_EQ(m.Counts.Transfers, 2u) << out;
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 0u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(out.find("default"), std::string::npos) << out;

  m = move(R"(
fn @f(%a: box, %k: i64) -> void {
  local %x: box
  store %x, %a
  switch %k {
    case 0 {
      %v = load %x
      retain %v
      call @take(%v)
    }
    case 1 {
      %u = load %x
      retain %u
      call @take(%u)
    }
  }
  %w = load %x
  release %w
  ret
}
)");
  out = f(m);
  EXPECT_EQ(m.Counts.Transfers, 2u) << out;
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_LT(out.find("default {"), out.find("release")) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
}

TEST(MoveAtLastUse, ABoolSwitchsOtherCaseGetsTheRelease) {
  // No path skips both cases: the release goes into the one that keeps the
  // variable, and the moving one stores no null.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = call @cond()
  switch %c {
    case true {
      %v = load %x
      retain %v
      call @take(%v)
    }
    case false {
      %u = load %x
      call @use(%u)
    }
  }
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_GT(out.find("release"), out.find("case false")) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
}

TEST(MoveAtLastUse, ACopyTakenEarlierIsAReferenceOfItsOwnWhereverItIsUsed) {
  // %a1 holds its own reference: the copy in the arm may hand the slot's
  // on although %a1 is used after the if.  The slot is cleared there, so
  // the release after the if releases it on the other path only.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %a1 = load %x
  retain %a1
  %c = call @cond()
  if %c {
    %a2 = load %x
    retain %a2
    call @take(%a2)
  }
  call @take(%a1)
  %w = load %x
  release %w
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(m.Counts.Transfers, 1u) << out;
  EXPECT_EQ(count(out, "retain"), 1u) << out;
  EXPECT_NE(out.find("retain %a1."), std::string::npos) << out;
  EXPECT_LT(out.find("store %x.0, null box"), out.find("call @take(%a2."))
      << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
}

TEST(MoveAtLastUse, APathThatStopsNeedsNoRelease) {
  // A panic between the copy and its consumer ends that path.
  Moved m = move(R"(
fn @f(%a: box, %i: i64) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %bad = cmp lt %i, 0
  if %bad {
    call @$rt.Paykan_panic_index(%i, 0)
    unreachable
  }
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(count(f(m), "retain"), 0u) << f(m);
}

// -- What does not move

TEST(MoveAtLastUse, ASlotReadAfterwardsIsNotMoved) {
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  %u = load %x
  call @use(%u)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
  EXPECT_EQ(count(f(m), "retain"), 1u);
}

TEST(MoveAtLastUse, ASlotThatDoesNotOwnItsValueIsNotMoved) {
  // No release on the way out (a borrow): the copy is the callee's only
  // reference of its own.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AnAliasUsedAfterTheCopyKeepsItACopy) {
  // %o, unboxed from the slot before the copy, is the receiver the callee
  // gets (as a method's `self`): the caller's reference keeps it alive.
  Moved m = move(R"(
fn @m2(%self: obj, %p: box) -> void {
  release %p
  ret
}

fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %b = load %x
  %o = unbox %b
  %v = load %x
  retain %v
  call @m2(%o, %v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AWeakReferenceReadLaterKeepsItACopy) {
  // The weak reference made from the variable is read after the copy: a
  // move could let it see the object gone sooner than the variable's scope
  // end.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  local %w: weak
  store %x, %a
  %b = load %x
  %wk = weak.new %b
  store %w, %wk
  %v = load %x
  retain %v
  call @take(%v)
  %wl = load %w
  %s = weak.load %wl
  release %s
  %r = load %x
  release %r
  %wr = load %w
  weak.release %wr
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AnAddressedSlotIsNotMoved) {
  // An `inout` argument: the callee may read or write the slot.
  Moved m = move(R"(
fn @io(%p: ptr) -> void {
  ret
}

fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %p = local.addr %x
  call @io(%p)
  %v = load %x
  retain %v
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, ACaptureIsAlwaysACopy) {
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = new fn.m.0
  %v = load %x
  retain %v
  field.store %c, fn.m.0.c0, %v
  %cb = box %c
  release %cb
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AWeakAssignmentIsNotAConsumer) {
  // The retained box is released right after its weak reference is made.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box
  local %w: weak
  store %x, %a
  %v = load %x
  retain %v
  %wk = weak.new %v
  store %w, %wk
  release %v
  %r = load %x
  release %r
  %wr = load %w
  weak.release %wr
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AViewIsNeverHandedOn) {
  // A `view` parameter's slot: it names what the caller's variable holds,
  // which lives as long as the function viewing it.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %x: box view
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
  EXPECT_NE(f(m).find("local %x.0: box view"), std::string::npos) << f(m);
}

// -- Borrowed references: the object may die sooner after a move

TEST(MoveAtLastUse, AFieldReadUsedAcrossALaterCallKeepsItACopy) {
  // `x = h.next; take(x); s = h.next.next + other(h)`: the field read, held
  // across a call that may assign h.next, may have been kept alive by x.
  // Read and used with no call in between, it is no reason for a copy.
  const std::string text = R"(
fn @f(%a: box, %h: box) -> void {
  local %x: box
  local %y: box
  store %x, %a
  store %y, %h
  %v = load %x
  retain %v
  call @take(%v)
  %yb = load %y
  %yo = unbox %yb
  %n = field.load %yo, Node.next
  %c = call @cond()
  %z = cmp eq %n, null box
  %w = load %x
  release %w
  %u = load %y
  release %u
  ret
}
)";
  Moved m = move(text);
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
  std::string used = text;
  used.replace(used.find("  %c = call @cond()\n"), 20, "");
  EXPECT_EQ(::move(used).Counts.Transfers, 1u);
}

TEST(MoveAtLastUse, AFieldReadBeforeTheCopyAndUsedAfterTheCallKeepsItACopy) {
  // The call given the reference may let it go before the field read is
  // used.
  Moved m = move(R"(
fn @f(%a: box, %h: box) -> void {
  local %x: box
  local %y: box
  store %x, %a
  store %y, %h
  %yb = load %y
  %yo = unbox %yb
  %n = field.load %yo, Node.next
  %v = load %x
  retain %v
  call @take(%v)
  %z = cmp eq %n, null box
  %w = load %x
  release %w
  %u = load %y
  release %u
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AReceiverReadFromAFieldKeepsTheCopyItIsGivenWith) {
  // `h.next.m(x)`: the receiver has no reference of its own, and the
  // method may let x go while it runs.
  Moved m = move(R"(
fn @m2(%self: obj, %p: box) -> void {
  release %p
  ret
}

fn @f(%a: box, %h: box) -> void {
  local %x: box
  local %y: box
  store %x, %a
  store %y, %h
  %yb = load %y
  %yo = unbox %yb
  %r = field.load %yo, Node.next
  %ro = unbox %r
  %v = load %x
  retain %v
  call @m2(%ro, %v)
  %w = load %x
  release %w
  %u = load %y
  release %u
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AStableReceiverReadIsNoReasonForACopy) {
  // `self.list.push(x)` in a class's method and in a struct's: the lowering
  // marks the read `stable` when the call cannot let go of the slot's
  // reference (docs/pir.md §7), so the slot keeps the receiver alive
  // whatever x's destination does, and x is handed on.  Unmarked, the
  // receiver is borrowed across the call and x stays a copy.
  auto method = [](const std::string &self, const std::string &read) {
    return R"(
fn @m2(%self: obj, %p: box) -> void {
  release %p
  ret
}

fn @f(%self: )" +
           self + R"(, %a: box) -> void {
  local %s: )" +
           self + R"(
  local %x: box
  store %s, %self
  store %x, %a
  %o = load %s
  %r = )" + read +
           R"(
  %ro = unbox %r
  %v = load %x
  retain %v
  call @m2(%ro, %v)
  %w = load %x
  release %w
  ret
}
)";
  };
  for (const auto &[self, read] :
       {std::pair<std::string, std::string>{"obj", "field.load %o, Node.next"},
        {"ptr", "ptr.load box, %o"}}) {
    Moved m = ::move(method(self, read + " stable"));
    EXPECT_EQ(m.Counts.Transfers, 1u) << f(m);
    EXPECT_NE(f(m).find(" stable\n"), std::string::npos) << f(m);
    EXPECT_EQ(::move(method(self, read)).Counts.Transfers, 0u) << read;
  }
}

TEST(MoveAtLastUse, SelfUsedAfterACallKeepsAParametersCopy) {
  // The caller may have no reference of its own to `self` (a receiver read
  // from a field), and the parameter may be what keeps it alive.  An
  // `__init__`'s `self` is held by its constructor.
  const std::string body = R"((%self: obj, %a: box) -> void {
  local %s: obj
  local %x: box
  store %s, %self
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  %o = load %s
  %n = field.load %o, Node.next
  %c = cmp eq %n, null box
  %w = load %x
  release %w
  ret
}
)";
  Moved m = ::move("fn @f" + body);
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
  EXPECT_EQ(
      ::move("fn @Node.__init__" + body, "Node.__init__").Counts.Transfers, 1u);
}

TEST(MoveAtLastUse, AChainOfHandOffsWhileSelfIsInUseMovesWhole) {
  // Each hand-off's window holds the release of the local it hands on to,
  // where `self` is in use; that release releases nothing once the next
  // hand-off moves, so every one moves.  When the last one stays a copy (a
  // call given the reference while `self` is in use), the release of each
  // local releases its object again, and every hand-off stays a copy.
  Moved m = ::move(chainUnderSelf(4, "ret"));
  EXPECT_EQ(m.Counts.Transfers, 4u) << f(m);
  EXPECT_EQ(m.Counts.Releases, 4u) << f(m);
  EXPECT_EQ(count(f(m), "retain"), 0u) << f(m);
  Moved kept = ::move(chainUnderSelf(4, "take"));
  EXPECT_EQ(kept.Counts.Transfers, 0u) << f(kept);
}

TEST(MoveAtLastUse, ARoundAfterAReleaseMovedIntoAnArmMovesMore) {
  // y's hand-off in the `then` arm moves; x's is kept a copy by y's release
  // after the `if`, where `self` is in use and the `else` arm still holds
  // y.  The cleanup moves that release into the `else` arm, out of x's
  // window: the next round moves x too.
  Moved m = move(R"(
fn @f(%self: obj, %a: box, %c: bool) -> void {
  local %s: obj
  local %x: box
  local %y: box
  local %z: box
  local %w: box
  store %s, %self
  store %x, %a
  %o = new Node
  %b = box %o
  store %y, %b
  store %z, null box
  store %w, null box
  if %c {
    %vy = load %y
    retain %vy
    store %z, %vy
    %vx = load %x
    retain %vx
    store %w, %vx
  }
  %ry = load %y
  release %ry
  %rx = load %x
  release %rx
  %t = load %s
  %k = field.load %t, Node.next
  %rz = load %z
  release %rz
  %rw = load %w
  release %rw
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 2u) << f(m);
  EXPECT_EQ(count(f(m), "retain"), 0u) << f(m);
}

TEST(MoveAtLastUse, AChainUnderABorrowedReferenceTakesTimeInProportion) {
  // One round moves the whole chain: four times the hand-offs take about
  // four times as long, where a round per hand-off would take sixteen.  The
  // time measured is the processor time the test spends, which a parallel
  // ctest preempting it does not stretch.
  auto seconds = [](size_t n) {
    double best = 0;
    for (int run = 0; run < 3; ++run) {
      ParseError err;
      auto p = parseProgram(kPrelude + chainUnderSelf(n, "ret"), err);
      EXPECT_TRUE(p) << err.str();
      if (!p)
        return 0.0;
      Function *fn = nullptr;
      for (Function &g : p->Modules.front().Functions)
        if (g.Name == "f")
          fn = &g;
      const std::clock_t start = std::clock();
      const MoveCounts c = moveAtLastUse(*fn);
      const double took =
          static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
      EXPECT_EQ(c.Transfers, n);
      best = run == 0 ? took : std::min(best, took);
    }
    return best;
  };
  const double small = seconds(250);
  const double large = seconds(1000);
  EXPECT_LT(large, 8 * small) << small << " s, then " << large << " s";
}

TEST(MoveAtLastUse, ABorrowedReferenceRetainedFirstIsAReferenceOfItsOwn) {
  // A field read retained before anything else uses it (a match subject's)
  // holds its object, call or no call.
  Moved m = move(R"(
fn @f(%a: box, %h: box) -> void {
  local %x: box
  local %y: box
  store %x, %a
  store %y, %h
  %yb = load %y
  %yo = unbox %yb
  %n = field.load %yo, Node.next
  retain %n
  %v = load %x
  retain %v
  call @take(%v)
  call @use(%n)
  release %n
  %w = load %x
  release %w
  %u = load %y
  release %u
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 1u) << f(m);
}

// -- Values held in memory

TEST(MoveAtLastUse, AWholeTupleCopiedAtItsLastUseMoves) {
  // `q = t`: the copy's retains go and the release of t's reference with
  // them; q owns what t did.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %t: mem(16, 8)
  local %q: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, 7
  %qa = local.addr %q
  %tb = local.addr %t
  mem.copy %qa, %tb, 16
  %e = ptr.load box, %qa
  retain %e
  %qr = local.addr %q
  %qe = ptr.load box, %qr
  release %qe
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 1u);
  EXPECT_EQ(m.Counts.Retains, 1u);
}

TEST(MoveAtLastUse, EveryReferenceOfAHeldValueMovesTogether) {
  // Two references: the release of one at scope exit is no use of the
  // other.
  Moved m = move(R"(
fn @f(%a: box, %b: box) -> void {
  local %t: mem(16, 8)
  local %q: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, %b
  %qa = local.addr %q
  mem.copy %qa, %ta, 16
  %e0 = ptr.load box, %qa
  retain %e0
  %q1 = ptr.add %qa, 8
  %e1 = ptr.load box, %q1
  retain %e1
  %qr = local.addr %q
  %qe0 = ptr.load box, %qr
  release %qe0
  %qr1 = ptr.add %qr, 8
  %qe1 = ptr.load box, %qr1
  release %qe1
  %tr = local.addr %t
  %te0 = ptr.load box, %tr
  release %te0
  %tr1 = ptr.add %tr, 8
  %te1 = ptr.load box, %tr1
  release %te1
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 2u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 1u);
  EXPECT_EQ(m.Counts.Retains, 2u);
  EXPECT_EQ(m.Counts.Releases, 2u);
}

TEST(MoveAtLastUse, AnAssignmentOfAWholeValueMoves) {
  // `o.t = t` (a field held in memory): the new references are taken
  // before the old ones go.
  Moved m = move(R"(
fn @f(%a: box, %dst: ptr) -> void {
  local %t: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, 7
  %tb = local.addr %t
  %e = ptr.load box, %tb
  retain %e
  %old = ptr.load box, %dst
  release %old
  mem.copy %dst, %tb, 16
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out;
  EXPECT_NE(out.find("release %old."), std::string::npos) << out;
}

// A drop function (docs/pir.md §7) releases the references of the whole
// local whose address it is given, so a call of one at scope exit is the
// release a copy before it moves to: `return t` hands t's references on,
// and the drop that follows releases the nulls left in t's slots.
const char *const kDropPair = R"(
fn @.drop.Pair(%at: ptr) -> void {
  %e0 = ptr.load box, %at
  release %e0
  %a1 = ptr.add %at, 8
  %e1 = ptr.load box, %a1
  release %e1
  ret
}
)";

TEST(MoveAtLastUse, AValueItsDropFunctionReleasesMovesAtItsLastUse) {
  Moved m = ::move(std::string(kDropPair) + R"(
fn @f(%a: box, %b: box, %dst: ptr) -> void {
  local %t: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, %b
  mem.copy %dst, %ta, 16
  %e0 = ptr.load box, %dst
  retain %e0
  %d1 = ptr.add %dst, 8
  %e1 = ptr.load box, %d1
  retain %e1
  %tr = local.addr %t
  call @.drop.Pair(%tr)
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "null box"), 2u) << out;
  EXPECT_EQ(count(out, "call @.drop.Pair("), 1u) << out;
  EXPECT_EQ(m.Counts.Transfers, 1u);
  EXPECT_EQ(m.Counts.Retains, 2u);
}

// A read of the local after the copy, before its drop, keeps the copy one.
TEST(MoveAtLastUse, AValueReadBeforeItsDropIsNotMoved) {
  Moved m = ::move(std::string(kDropPair) + R"(
fn @f(%a: box, %b: box, %dst: ptr) -> void {
  local %t: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, %b
  mem.copy %dst, %ta, 16
  %e0 = ptr.load box, %dst
  retain %e0
  %d1 = ptr.add %dst, 8
  %e1 = ptr.load box, %d1
  retain %e1
  %tr = local.addr %t
  %x = ptr.load box, %tr
  call @use(%x)
  call @.drop.Pair(%tr)
  ret
}
)");
  const std::string out = f(m);
  EXPECT_EQ(count(out, "retain"), 2u) << out;
  EXPECT_EQ(count(out, "null box"), 0u) << out;
  EXPECT_EQ(m.Counts.Transfers, 0u);
}

TEST(MoveAtLastUse, AnIfBetweenAHeldValuesLoadsAndItsCopyKeepsItACopy) {
  // The retain is taken before the copy, but the if between its load and
  // the copy may write the slot: the bytes copied need not be the
  // reference retained, and what the if does is not looked into.
  Moved m = move(R"(
fn @f(%a: box, %b: box, %dst: ptr) -> void {
  local %t: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, 7
  %tb = local.addr %t
  %e = ptr.load box, %tb
  %c = call @cond()
  if %c {
    ptr.store %tb, %b
  }
  retain %e
  %old = ptr.load box, %dst
  release %old
  mem.copy %dst, %tb, 16
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, APartOfAHeldValueIsNotMoved) {
  // The second element alone, copied out: a member, not the variable.
  Moved m = move(R"(
fn @f(%a: box, %b: box) -> void {
  local %t: mem(16, 8)
  local %q: mem(8, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %t1 = ptr.add %ta, 8
  ptr.store %t1, %b
  %qa = local.addr %q
  mem.copy %qa, %t1, 8
  %e = ptr.load box, %qa
  retain %e
  %qr = local.addr %q
  %qe = ptr.load box, %qr
  release %qe
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  %tr1 = ptr.add %tr, 8
  %te1 = ptr.load box, %tr1
  release %te1
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AHeldValueReadAfterTheCopyIsNotMoved) {
  // Its reference is read again (printed, say) after the copy; a scalar
  // part read would not have mattered.
  Moved m = move(R"(
fn @f(%a: box) -> void {
  local %t: mem(16, 8)
  local %q: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %qa = local.addr %q
  mem.copy %qa, %ta, 16
  %e = ptr.load box, %qa
  retain %e
  %again = ptr.load box, %ta
  call @use(%again)
  %qr = local.addr %q
  %qe = ptr.load box, %qr
  release %qe
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

TEST(MoveAtLastUse, AHeldValueCapturedByAClosureIsNotMoved) {
  Moved m = move(R"(
class fn.m.1 {
  field c0: mem(16, 8)
  vtable {
    destroy = @fn.m.1.destroy : (obj) -> void
  }
}

fn @fn.m.1.destroy(%self: obj) -> void {
  free %self
  ret
}

fn @f(%a: box) -> void {
  local %t: mem(16, 8)
  %ta = local.addr %t
  ptr.store %ta, %a
  %c = new fn.m.1
  %ca = field.addr %c, fn.m.1.c0
  mem.copy %ca, %ta, 16
  %e = ptr.load box, %ca
  retain %e
  %cb = box %c
  release %cb
  %tr = local.addr %t
  %te = ptr.load box, %tr
  release %te
  ret
}
)");
  EXPECT_EQ(m.Counts.Transfers, 0u) << f(m);
}

// -- The pass

TEST(MoveAtLastUse, RunningTwiceChangesNothingMore) {
  const std::string text = R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %c = call @cond()
  switch %c {
    case true {
      %v = load %x
      retain %v
      call @take(%v)
    }
    case false {
      %u = load %x
      call @use(%u)
    }
  }
  %w = load %x
  release %w
  ret
}
)";
  Moved once = move(text);
  const std::string first = toString(once.P);
  for (Function &fn : once.P.Modules.front().Functions)
    if (fn.Name == "f") {
      EXPECT_EQ(moveAtLastUse(fn).Transfers, 0u);
    }
  EXPECT_EQ(toString(once.P), first);
}

TEST(MoveAtLastUse, ItsTimeGrowsWithTheFunctionNotWithCopiesTimesItsSize) {
  // Each copy's last use, and each release and null store after the
  // moves, is found by one walk of the function for all of them.  Four
  // times the copies in a function four times the size take about four
  // times as long; a walk per copy would take sixteen.  The time measured
  // is the processor time the test spends, as above.
  auto seconds = [](size_t n) {
    double best = 0;
    for (int run = 0; run < 3; ++run) {
      ParseError err;
      auto p = parseProgram(kPrelude + manyCopies(n), err);
      EXPECT_TRUE(p) << err.str();
      if (!p)
        return 0.0;
      Function *fn = nullptr;
      for (Function &g : p->Modules.front().Functions)
        if (g.Name == "f")
          fn = &g;
      const std::clock_t start = std::clock();
      const MoveCounts c = moveAtLastUse(*fn);
      const double took =
          static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC;
      EXPECT_EQ(c.Transfers, n);
      EXPECT_EQ(c.Releases, n);
      best = run == 0 ? took : std::min(best, took);
    }
    return best;
  };
  const double small = seconds(1000);
  const double large = seconds(4000);
  EXPECT_LT(large, 8 * small) << small << " s, then " << large << " s";
}

TEST(MoveAtLastUse, ThePassCountsWhatItDid) {
  ParseError err;
  auto p = parseProgram(kPrelude + R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  call @take(%v)
  %w = load %x
  release %w
  ret
}
)",
                        err);
  ASSERT_TRUE(p) << err.str();
  PassManager pm(/*verifyEach=*/true);
  pm.add(createMovesAtLastUsePass());
  Statistics stats;
  ASSERT_TRUE(pm.run(*p, stats));
  EXPECT_EQ(stats.get("moves-at-last-use", "transfers"), 1u);
  EXPECT_EQ(stats.get("moves-at-last-use", "retains removed"), 1u);
  EXPECT_EQ(stats.get("moves-at-last-use", "releases removed"), 1u);
  std::ostringstream os;
  stats.print(os);
  EXPECT_EQ(os.str(), "paykan pass stats:\n"
                      "  moves-at-last-use transfers : 1\n"
                      "  moves-at-last-use retains removed : 1\n"
                      "  moves-at-last-use releases removed : 1\n");
}
