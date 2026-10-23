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
/// @f and the result verified.
Moved move(const std::string &text) {
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
    if (f.Name == "f")
      m.Counts = moveAtLastUse(f);
  errors = verify(m.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors) << toString(m.P);
  return m;
}

/// The printed @f of @p m.
std::string f(const Moved &m) {
  const std::string text = toString(m.P);
  const size_t start = text.find("fn @f(");
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
