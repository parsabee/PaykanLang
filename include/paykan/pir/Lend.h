// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (docs/pir.md §12).  Its caller half lends a box argument
// where the caller's own reference outlives the call: a copy the lowering
// made only to hand the argument over (`retain lend`, docs/pir.md §6) that
// the moves at last use left loses its retain, and every mark is cleared.
// Its callee half lends a box parameter the body never writes: the retain
// of its prologue (docs/pir.md §3) goes, and the body asks the parameter's
// bit of the ownership mask wherever it lets go of the parameter's own
// reference.  Backend-independent; the driver runs it after the moves on
// every program and on the code of every module it writes to a `.pkm`,
// which so keeps no mark.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"
#include "paykan/pir/Pass.h"

#include <cstdint>
#include <memory>

namespace paykan::pir {

/// What lend did to a function.
struct LendCounts {
  /// The `retain lend` marks found, each a copy the lowering made only to
  /// hand an argument of a lending callee over.
  uint64_t Marks = 0;
  /// The marks whose copy went: the argument is lent.
  uint64_t Lent = 0;
  /// The marks whose copy stayed because a statement from the read to the
  /// call may let the object the caller keeps go (the window check).
  uint64_t KeptForWindow = 0;
  /// The box parameters whose prologue's retain went and that the body
  /// hands to no lending callee's box parameter.
  uint64_t ParamsLent = 0;
  /// Those whose prologue's retain went and that the body hands on to a
  /// lending callee's box parameter with their own bit (`owned(%o)`).
  uint64_t ParamsHandedOn = 0;
  /// Those whose prologue stays: the body writes the parameter (stores a
  /// value into its slot, or takes the slot's address) or uses it in a way
  /// the pass does not follow, so it owns the parameter from its entry.
  uint64_t ParamsOwnedOnEntry = 0;
  /// The retains a lent parameter's keeps make (`if %l { retain %v }`):
  /// one before each store, field.store, ptr.store, `ret` and argument of
  /// a callee without the parameter's bit that takes its reference.
  uint64_t RetainsAtKeeps = 0;

  LendCounts &operator+=(const LendCounts &o) {
    Marks += o.Marks;
    Lent += o.Lent;
    KeptForWindow += o.KeptForWindow;
    ParamsLent += o.ParamsLent;
    ParamsHandedOn += o.ParamsHandedOn;
    ParamsOwnedOnEntry += o.ParamsOwnedOnEntry;
    RetainsAtKeeps += o.RetainsAtKeeps;
    return *this;
  }
};

/// The caller half: lend the arguments of @p f's calls that its `retain
/// lend` marks may lend and clear every mark (docs/pir.md §12): the mark's
/// value is read from a local of @p f (the root), loaded or through unboxes
/// and field and element reads and addresses, in the block of the call,
/// and is used by the mark and by one box argument of that call alone, an
/// argument with a bit in the ownership mask of a lending callee (@p m, the
/// module @p f is in, declares the callees); and no statement from the
/// root's load to the call, nor one nested in those, stores into the root,
/// takes its address, releases a load of it, hands on a load of it that was
/// not retained first, or does anything but read through the pointer the
/// root holds.  Then the copy's retain goes and the argument is `lent`.
/// Every other mark is a plain `retain` from then on.
LendCounts lendArguments(Function &f, const Module &m);

/// The callee half: lend each box parameter of @p f with a bit in its
/// ownership mask whose prologue (docs/pir.md §3) is at the top of the body
/// and that the body never writes, where every use of its own reference
/// (the parameter, or a load of its slot not paid for by a retain) is one
/// the pass follows (docs/pir.md §12).  The prologue's `if` goes, its
/// `owned` stays, and each place the body lets go of that reference asks
/// the parameter's bit: a `release` happens only when it was handed over,
/// an argument of a lending callee's box parameter with a bit hands it on
/// with it (`owned(%o)`), and a store, field.store, ptr.store, `ret` or any
/// other argument that keeps it is preceded by a retain when it was lent.
/// A parameter the body writes keeps its prologue.  An internal compiler
/// error when a box parameter with a bit has neither its prologue nor the
/// `owned` of one the pass lent and the body takes its reference: called
/// with it lent, the body would release it.  @p m declares the callees.
StatusOr<LendCounts> lendParameters(Function &f, const Module &m);

/// lendArguments, then lendParameters: both halves, as the pass runs them.
StatusOr<LendCounts> lend(Function &f, const Module &m);

/// lend on every function the program defines, as a pass ("lending"),
/// counting "marks", "arguments lent", "copies kept for the window",
/// "parameters lent", "parameters handed on", "parameters owned on entry"
/// and "retains at keeps".
std::unique_ptr<Pass> createLendingPass();

} // namespace paykan::pir
