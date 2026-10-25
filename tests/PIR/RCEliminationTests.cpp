// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The reference-count elimination pass (docs/pir.md §12) on hand-written
// PIR: the pairs it removes, by each of the three things that may keep an
// object alive, and the ones it must leave, the field-load hazard first.

#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Parser.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/RCElimination.h"
#include "paykan/pir/Verifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

using namespace paykan;
using namespace paykan::pir;

namespace {

// What the programs below call: @take and @count release their argument
// (an owned argument, as a user function's is), @cond is a test that may
// run any code, @Node.visit a method.
const std::string kPrelude = R"(module "m"
extern fn @$rt.Paykan_panic_index(i64, i64) -> void
extern fn @$rt.PaykanString_length(obj) -> i64

class Node {
  field l: box
  field v: i64
  vtable {
    destroy = @Node.destroy : (obj) -> void
    visit = @Node.visit : (obj) -> void
  }
}

fn @Node.destroy(%self: obj) -> void {
  %n = field.load %self, Node.l
  release %n
  free %self
  ret
}

fn @Node.visit(%self: obj) -> void {
  ret
}

fn @take(%p: box) -> void {
  release %p
  ret
}

fn @count(%p: box) -> i64 {
  release %p
  ret 1
}

fn @cond() -> bool {
  ret true
}

fn @main() -> i64 {
  ret 0
}
)";

struct Result {
  Program P;
  RCCounts Counts;
};

/// The prelude and @p text parsed and verified; then eliminateRedundantRC
/// run on the function @p name and the result verified.
Result eliminate(const std::string &text, const std::string &name = "f") {
  Result r;
  ParseError err;
  auto p = parseProgram(kPrelude + text, err);
  EXPECT_TRUE(p) << err.str();
  if (!p)
    return r;
  r.P = std::move(*p);
  auto errors = verify(r.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors);
  for (Function &f : r.P.Modules.front().Functions)
    if (f.Name == name)
      r.Counts = eliminateRedundantRC(f);
  errors = verify(r.P);
  EXPECT_TRUE(errors.empty()) << formatErrors(errors) << toString(r.P);
  return r;
}

/// The printed function @p name of @p r.
std::string f(const Result &r, const std::string &name = "f") {
  const std::string text = toString(r.P);
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

/// The match of the binary-trees benchmark, as the lowering makes it
/// (`match n.l { x: Node { c = c + count(x); } None { } }`): the subject's
/// retain, the binding's, and the argument's, which @count releases.
const std::string kTreeMatch = R"(
fn @f(%n: box) -> i64 {
  local %np: box
  local %c: i64
  local %x: box
  store %np, %n
  store %c, 1
  %n2 = load %np
  %no = unbox %n2
  %l = field.load %no, Node.l
  retain %l
  %so = unbox %l
  %none = cmp eq %so, null obj
  if %none {
  } else {
    retain %l
    store %x, %l
    %c1 = load %c
    %x1 = load %x
    retain %x1
    %k = call @count(%x1)
    %c2 = add %c1, %k
    store %c, %c2
    %x2 = load %x
    release %x2
  }
  release %l
  %c3 = load %c
  %n3 = load %np
  release %n3
  ret %c3
}
)";

/// @f with @p n field reads of %o, each a match subject (retained, then
/// released after a binding that is retained and released inside it), in a
/// function of about 14n statements.
std::string manySubjects(size_t n) {
  std::ostringstream os;
  os << "fn @f(%o: obj) -> void {\n";
  for (size_t i = 0; i < n; ++i)
    os << "  local %x" << i << ": box\n";
  for (size_t i = 0; i < n; ++i)
    os << "  %l" << i << " = field.load %o, Node.l\n  retain %l" << i
       << "\n  %s" << i << " = unbox %l" << i << "\n  %z" << i
       << " = cmp eq %s" << i << ", null obj\n  if %z" << i
       << " {\n  } else {\n    retain %l" << i << "\n    store %x" << i
       << ", %l" << i << "\n    %y" << i << " = load %x" << i
       << "\n    %yo" << i << " = unbox %y" << i << "\n    %v" << i
       << " = field.load %yo" << i << ", Node.v\n    %t" << i << " = load %x"
       << i << "\n    release %t" << i << "\n  }\n  release %l" << i << "\n";
  os << "  ret\n}\n";
  return os.str();
}

} // namespace

// -- The field-load hazard and what is nested in it

TEST(RCElimination, TheTreeMatchKeepsItsSubjectAndLosesTheBinding) {
  // The subject is read from a field and @count may run any code, which
  // may write n.l: its retain stays.  The binding's retain and release are
  // nested in it and go; the argument's retain is the reference @count
  // releases, an owned argument's.
  Result r = eliminate(kTreeMatch);
  EXPECT_EQ(f(r), R"(fn @f(%n.1: box) -> i64 {
  local %np.0: box
  local %c.1: i64
  local %x.2: box
  store %np.0, %n.1
  store %c.1, 1
  %n2.2 = load %np.0
  %no.3 = unbox %n2.2
  %l.4 = field.load %no.3, Node.l
  retain %l.4
  %so.5 = unbox %l.4
  %none.6 = cmp eq %so.5, null obj
  if %none.6 {
  } else {
    store %x.2, %l.4
    %c1.7 = load %c.1
    %x1.8 = load %x.2
    retain %x1.8
    %k.9 = call @count(%x1.8)
    %c2.10 = add %c1.7, %k.9
    store %c.1, %c2.10
  }
  release %l.4
  %c3.12 = load %c.1
  %n3.13 = load %np.0
  release %n3.13
  ret %c3.12
}
)");
  EXPECT_EQ(r.Counts.Retains, 1u);
  EXPECT_EQ(r.Counts.Releases, 1u);
}

TEST(RCElimination, AfterTheMovesTheTreeMatchHasNothingLeftToRemove) {
  // The moves at last use hand the binding's reference to @count: that
  // retain is the argument's now, and the subject's stays for the call.
  ParseError err;
  auto p = parseProgram(kPrelude + kTreeMatch, err);
  ASSERT_TRUE(p) << err.str();
  PassManager pm(/*verifyEach=*/true);
  pm.add(createMovesAtLastUsePass());
  pm.add(createRCEliminationPass());
  Statistics stats;
  ASSERT_TRUE(pm.run(*p, stats));
  EXPECT_EQ(stats.get("moves-at-last-use", "transfers"), 1u);
  EXPECT_EQ(stats.get("rc-elimination", "retains removed"), 0u);
  Result r;
  r.P = std::move(*p);
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain"), 2u) << out;
  EXPECT_EQ(count(out, "release %l"), 1u) << out;
}

TEST(RCElimination, AFieldReadKeepsItsRetainAcrossACall) {
  // The hazard itself: @cond may set o.l to None and free what %l names.
  Result r = eliminate(R"(
fn @f(%o: obj) -> i64 {
  %l = field.load %o, Node.l
  retain %l
  %lo = unbox %l
  %c = call @cond()
  %v = field.load %lo, Node.v
  release %l
  ret %v
}
)");
  EXPECT_EQ(count(f(r), "retain %l"), 1u) << f(r);
  EXPECT_EQ(count(f(r), "release %l"), 1u) << f(r);
  EXPECT_EQ(r.Counts.Retains, 0u);
}

TEST(RCElimination, AFieldReadLosesItsRetainWhenNothingCanLetGo) {
  // Field reads, an unbox, a comparison and a runtime string helper: no
  // code that can write o.l runs, so the field keeps the object.
  Result r = eliminate(R"(
fn @f(%o: obj) -> i64 {
  %l = field.load %o, Node.l
  retain %l
  %lo = unbox %l
  %z = cmp eq %lo, null obj
  %n = call @$rt.PaykanString_length(%lo)
  %v = field.load %lo, Node.v
  release %l
  ret %v
}
)");
  EXPECT_EQ(count(f(r), "retain"), 0u) << f(r);
  EXPECT_EQ(count(f(r), "release"), 0u) << f(r);
  EXPECT_EQ(r.Counts.Retains, 1u);
  EXPECT_EQ(r.Counts.Releases, 1u);
}

TEST(RCElimination, AVirtualCallKeepsTheSubjectAndNotTheBinding) {
  Result r = eliminate(R"(
fn @f(%o: obj) -> void {
  local %x: box
  %l = field.load %o, Node.l
  retain %l
  retain %l
  store %x, %l
  %x1 = load %x
  %xo = unbox %x1
  vcall %xo : (obj) -> void [1] ()
  %t = load %x
  release %t
  release %l
  ret
}
)");
  EXPECT_EQ(f(r), R"(fn @f(%o.1: obj) -> void {
  local %x.0: box
  %l.2 = field.load %o.1, Node.l
  retain %l.2
  store %x.0, %l.2
  %x1.3 = load %x.0
  %xo.4 = unbox %x1.3
  vcall %xo.4 : (obj) -> void [1] ()
  release %l.2
  ret
}
)");
}

TEST(RCElimination, AnIndirectCallAReleaseOrAFreeLetsGo) {
  // Each may free what the field held.
  for (const char *between :
       {"  icall %fn : () -> void ()\n",
        "  %w = field.load %o, Node.l\n  release %w\n",
        "  %q = new Node\n  free %q\n"}) {
    Result r = eliminate(std::string(R"(
fn @f(%o: obj, %fn: ptr) -> void {
  %l = field.load %o, Node.l
  retain %l
)") + between + R"(  release %l
  ret
}
)");
    EXPECT_EQ(count(f(r), "retain %l"), 1u) << between << f(r);
  }
}

// -- Stores

TEST(RCElimination, AStoreThatMayOverwriteAReferenceLetsGo) {
  // Into a field of references, an element (memory), or bytes copied or
  // zeroed over them: o.l, or the element, may be what %l came from.
  for (const char *store :
       {"  field.store %o, Node.l, %w\n", "  ptr.store %p, %w\n",
        "  mem.copy %p, %q, 8\n", "  mem.zero %p, 8\n"}) {
    Result r = eliminate(std::string(R"(
fn @f(%o: obj, %p: ptr, %q: ptr, %w: box) -> void {
  %l = field.load %o, Node.l
  %e = ptr.load box, %p
  retain %l
  retain %e
)") + store + R"(  release %e
  release %l
  ret
}
)");
    EXPECT_EQ(count(f(r), "retain"), 2u) << store << f(r);
    EXPECT_EQ(r.Counts.Retains, 0u) << store;
  }
}

TEST(RCElimination, AStoreOfANumberOverwritesNoReference) {
  Result r = eliminate(R"(
fn @f(%o: obj, %p: ptr) -> void {
  local %k: box
  %e = ptr.load box, %p
  retain %e
  field.store %o, Node.v, 7
  ptr.store %p, 3
  %b = new Node
  %bb = box %b
  store %k, %bb
  release %e
  %t = load %k
  release %t
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 0u) << f(r);
  EXPECT_EQ(r.Counts.Retains, 1u);
}

// -- Locals

TEST(RCElimination, ALocalNotReassignedKeepsWhatItHoldsAcrossCalls) {
  Result r = eliminate(R"(
fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %vo = unbox %v
  vcall %vo : (obj) -> void [1] ()
  %c = call @cond()
  release %v
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 0u) << f(r);
  EXPECT_EQ(count(f(r), "release"), 1u) << f(r);
  EXPECT_EQ(r.Counts.Retains, 1u);
}

TEST(RCElimination, ALocalReassignedInTheRangeKeepsNothing) {
  // `x = other` releases what %v names: %v's retain is what keeps it.
  Result r = eliminate(R"(
fn @f(%a: box, %b: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %old = load %x
  release %old
  store %x, %b
  %vo = unbox %v
  vcall %vo : (obj) -> void [1] ()
  release %v
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain %v"), 1u) << f(r);
}

TEST(RCElimination, ALocalWhoseAddressIsTakenKeepsNothing) {
  // A callee given its address may store into it.
  Result r = eliminate(R"(
fn @reset(%p: ptr) -> void {
  ret
}

fn @f(%a: box) -> void {
  local %x: box
  store %x, %a
  %v = load %x
  retain %v
  %px = local.addr %x
  call @reset(%px)
  release %v
  %w = load %x
  release %w
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain %v"), 1u) << f(r);
}

TEST(RCElimination, AParameterHandedOnAtItsLastUseKeepsNothing) {
  // A move at last use (#186): the parameter's own reference goes to @take,
  // which may free the object while %a still names it.
  Result r = eliminate(R"(
fn @f(%n: box) -> i64 {
  local %np: box
  store %np, %n
  %a = load %np
  retain %a
  %b = load %np
  call @take(%b)
  %ao = unbox %a
  %v = field.load %ao, Node.v
  release %a
  ret %v
}
)");
  EXPECT_EQ(count(f(r), "retain %a"), 1u) << f(r);
}

TEST(RCElimination, AParameterCopiedKeepsWhatItHolds) {
  // The same with a copy: the parameter keeps its own reference until its
  // release at the end.
  Result r = eliminate(R"(
fn @f(%n: box) -> i64 {
  local %np: box
  store %np, %n
  %a = load %np
  retain %a
  %b = load %np
  retain %b
  call @take(%b)
  %ao = unbox %a
  %v = field.load %ao, Node.v
  release %a
  %e = load %np
  release %e
  ret %v
}
)");
  EXPECT_EQ(count(f(r), "retain %a"), 0u) << f(r);
  EXPECT_EQ(count(f(r), "retain %b"), 1u) << f(r);
}

TEST(RCElimination, AMoveIntoAParameterInsideTheSubjectsRangeKeepsTheSubject) {
  // `match n.l { x: Node { take(n); ... x.v ... } }` after the moves: @take
  // may free n and n.l with it, which only the subject's retain then holds.
  Result r = eliminate(R"(
fn @f(%n: box) -> i64 {
  local %np: box
  local %x: box
  store %np, %n
  %n1 = load %np
  %no = unbox %n1
  %l = field.load %no, Node.l
  retain %l
  %so = unbox %l
  %none = cmp eq %so, null obj
  if %none {
    %n2 = load %np
    call @take(%n2)
  } else {
    retain %l
    store %x, %l
    %n3 = load %np
    call @take(%n3)
    %x1 = load %x
    %xo = unbox %x1
    %v = field.load %xo, Node.v
    %t = load %x
    release %t
  }
  release %l
  ret 0
}
)");
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain %l"), 1u) << out;
  EXPECT_EQ(count(out, "release %l"), 1u) << out;
  EXPECT_EQ(count(out, "release %t"), 0u) << out;
  EXPECT_EQ(r.Counts.Retains, 1u);
}

// -- Paths

TEST(RCElimination, BreakAndContinueCarryTheirReleases) {
  // A loop over a list: the binding's release on each way out of the arm,
  // the subject's on each way out of the body.  Nothing in the body can let
  // go, so both pairs go, with every release.
  Result r = eliminate(R"(
fn @f(%a: box) -> i64 {
  local %cur: box
  local %n: box
  local %s: i64
  store %cur, %a
  store %s, 0
  while {
    %go = call @cond()
    cond %go
  } {
    %c0 = load %cur
    %co = unbox %c0
    %nx = field.load %co, Node.l
    retain %nx
    %so = unbox %nx
    %none = cmp eq %so, null obj
    if %none {
      release %nx
      break
    } else {
      retain %nx
      store %n, %nx
      %n1 = load %n
      %no = unbox %n1
      %v = field.load %no, Node.v
      %three = cmp eq %v, 3
      if %three {
        %t1 = load %n
        release %t1
        release %nx
        break
      }
      %two = cmp eq %v, 2
      if %two {
        %t2 = load %n
        release %t2
        release %nx
        continue
      }
      %s0 = load %s
      %s1 = add %s0, %v
      store %s, %s1
      %t3 = load %n
      release %t3
    }
    release %nx
  }
  %r = load %s
  %c9 = load %cur
  release %c9
  ret %r
}
)");
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out; // %cur's
  EXPECT_EQ(r.Counts.Retains, 2u);
  EXPECT_EQ(r.Counts.Releases, 7u);
}

TEST(RCElimination, AnEarlyReturnFromAMatchCarriesItsReleases) {
  Result r = eliminate(R"(
fn @f(%a: box) -> i64 {
  local %ap: box
  local %x: box
  store %ap, %a
  %a1 = load %ap
  %ao = unbox %a1
  %l = field.load %ao, Node.l
  retain %l
  %so = unbox %l
  %none = cmp eq %so, null obj
  if %none {
  } else {
    retain %l
    store %x, %l
    %x1 = load %x
    %xo = unbox %x1
    %v = field.load %xo, Node.v
    %pos = cmp gt %v, 0
    if %pos {
      %t1 = load %x
      release %t1
      release %l
      %a2 = load %ap
      release %a2
      ret %v
    }
    %t2 = load %x
    release %t2
  }
  release %l
  %a3 = load %ap
  release %a3
  ret 0
}
)");
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain"), 0u) << out;
  EXPECT_EQ(count(out, "release %a"), 2u) << out; // the parameter's
  EXPECT_EQ(count(out, "release"), 2u) << out;
  EXPECT_EQ(r.Counts.Retains, 2u);
  EXPECT_EQ(r.Counts.Releases, 4u);
}

TEST(RCElimination, AnEarlyReturnPastACallKeepsTheSubjectOnEveryPath) {
  Result r = eliminate(R"(
fn @f(%o: obj) -> i64 {
  local %x: box
  %l = field.load %o, Node.l
  retain %l
  retain %l
  store %x, %l
  %c = call @cond()
  if %c {
    %t1 = load %x
    release %t1
    release %l
    ret 1
  }
  %t2 = load %x
  release %t2
  release %l
  ret 0
}
)");
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain %l"), 1u) << out;
  EXPECT_EQ(count(out, "release %l"), 2u) << out;
  EXPECT_EQ(count(out, "release %t"), 0u) << out;
}

TEST(RCElimination, AReleaseOnOnlySomePathsIsNotARange) {
  // The pair would go from the path that does not retain.
  Result r = eliminate(R"(
fn @f(%o: obj) -> void {
  %l = field.load %o, Node.l
  %c = call @cond()
  if %c {
    retain %l
  }
  release %l
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 1u) << f(r);
  EXPECT_EQ(count(f(r), "release"), 1u) << f(r);
}

TEST(RCElimination, ARangeLeftOpenOnAPathIsNotBalanced) {
  // On the `then` path nothing releases what the retain made: it is handed
  // to @take instead, and removing the retain would hand on what o.l holds.
  Result r = eliminate(R"(
fn @f(%o: obj) -> void {
  %l = field.load %o, Node.l
  retain %l
  %c = call @cond()
  if %c {
    call @take(%l)
    ret
  }
  release %l
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 1u) << f(r);
}

TEST(RCElimination, ARetainInALoopReleasedAfterItIsKept) {
  Result r = eliminate(R"(
fn @f(%o: obj) -> void {
  %l = field.load %o, Node.l
  retain %l
  while {
    %go = call @cond()
    cond %go
  } {
    retain %l
  }
  release %l
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 2u) << f(r);
}

TEST(RCElimination, APairInsideALoopIterationGoes) {
  Result r = eliminate(R"(
fn @f(%o: obj) -> void {
  while {
    %go = call @cond()
    cond %go
  } {
    %l = field.load %o, Node.l
    retain %l
    %lo = unbox %l
    %v = field.load %lo, Node.v
    release %l
  }
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain"), 0u) << f(r);
  EXPECT_EQ(count(f(r), "release"), 0u) << f(r);
}

TEST(RCElimination, APanicPathNeedsNoRelease) {
  // `unreachable` ends a path: the range is closed on the others.
  Result r = eliminate(R"(
fn @f(%o: obj, %i: i64) -> i64 {
  %l = field.load %o, Node.l
  retain %l
  %bad = cmp lt %i, 0
  if %bad {
    call @$rt.Paykan_panic_index(%i, 0)
    unreachable
  }
  %lo = unbox %l
  %v = field.load %lo, Node.v
  release %l
  ret %v
}
)");
  EXPECT_EQ(count(f(r), "retain"), 0u) << f(r);
  EXPECT_EQ(count(f(r), "release"), 0u) << f(r);
}

// -- Enclosing references

TEST(RCElimination, AFreshReferenceKeepsWhatIsNestedInItAcrossCalls) {
  // A weak.load's box (a match on a weak field): the binding goes whatever
  // the arm calls.
  Result r = eliminate(R"(
fn @f(%w: weak) -> void {
  local %x: box
  %s = weak.load %w
  %so = unbox %s
  %none = cmp eq %so, null obj
  if %none {
  } else {
    retain %s
    store %x, %s
    %x1 = load %x
    retain %x1
    call @take(%x1)
    %c = call @cond()
    %t = load %x
    release %t
  }
  release %s
  ret
}
)");
  const std::string out = f(r);
  EXPECT_EQ(count(out, "retain %s"), 0u) << out;
  EXPECT_EQ(count(out, "retain %x1"), 1u) << out;
  EXPECT_EQ(count(out, "release"), 1u) << out; // the weak.load's
}

TEST(RCElimination, AnEnclosingReferenceHandedOnInTheRangeKeepsNothing) {
  // %s's own reference goes to @take inside the binding's range: the
  // binding's retain is what holds the object then.
  Result r = eliminate(R"(
fn @f(%w: weak) -> void {
  local %x: box
  %s = weak.load %w
  retain %s
  store %x, %s
  call @take(%s)
  %x1 = load %x
  %xo = unbox %x1
  vcall %xo : (obj) -> void [1] ()
  %t = load %x
  release %t
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain %s"), 1u) << f(r);
}

TEST(RCElimination, ALocalHandedOnIsNoRangeOfItsOwn) {
  // The binding's reference goes to @take (a move): no release of the
  // local ends a range.
  Result r = eliminate(R"(
fn @f(%w: weak) -> void {
  local %x: box
  %s = weak.load %w
  retain %s
  store %x, %s
  %x1 = load %x
  call @take(%x1)
  release %s
  ret
}
)");
  EXPECT_EQ(count(f(r), "retain %s"), 1u) << f(r);
}

// -- The pass

TEST(RCElimination, ItChangesNothingOnItsOwnOutput) {
  for (const std::string &text : {kTreeMatch, manySubjects(3)}) {
    Result once = eliminate(text);
    const std::string first = toString(once.P);
    for (Function &g : once.P.Modules.front().Functions)
      if (g.Name == "f") {
        const RCCounts again = eliminateRedundantRC(g);
        EXPECT_EQ(again.Retains, 0u);
        EXPECT_EQ(again.Releases, 0u);
      }
    EXPECT_EQ(toString(once.P), first);
  }
}

TEST(RCElimination, ThePassCountsWhatItDid) {
  ParseError err;
  auto p = parseProgram(kPrelude + manySubjects(2), err);
  ASSERT_TRUE(p) << err.str();
  PassManager pm(/*verifyEach=*/true);
  pm.add(createRCEliminationPass());
  Statistics stats;
  ASSERT_TRUE(pm.run(*p, stats));
  EXPECT_EQ(stats.get("rc-elimination", "retains removed"), 4u);
  EXPECT_EQ(stats.get("rc-elimination", "releases removed"), 4u);
  std::ostringstream os;
  stats.print(os);
  EXPECT_EQ(os.str(), "paykan pass stats:\n"
                      "  rc-elimination retains removed : 4\n"
                      "  rc-elimination releases removed : 4\n");
}

TEST(RCElimination, ItsTimeGrowsWithTheFunction) {
  // Each walk runs from a value's definition to its last use: four times
  // the subjects in a function four times the size take about four times
  // as long.
  auto seconds = [](size_t n) {
    double best = 0;
    for (int run = 0; run < 3; ++run) {
      ParseError err;
      auto p = parseProgram(kPrelude + manySubjects(n), err);
      EXPECT_TRUE(p) << err.str();
      if (!p)
        return 0.0;
      Function *fn = nullptr;
      for (Function &g : p->Modules.front().Functions)
        if (g.Name == "f")
          fn = &g;
      const auto start = std::chrono::steady_clock::now();
      const RCCounts c = eliminateRedundantRC(*fn);
      const std::chrono::duration<double> took =
          std::chrono::steady_clock::now() - start;
      EXPECT_EQ(c.Retains, 2 * n);
      best = run == 0 ? took.count() : std::min(best, took.count());
    }
    return best;
  };
  const double small = seconds(500);
  const double large = seconds(2000);
  EXPECT_LT(large, 8 * small) << small << " s, then " << large << " s";
}
