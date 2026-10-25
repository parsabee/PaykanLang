// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (docs/pir.md §12): a copy the lowering made only to hand
// a box argument over (`retain lend`, docs/pir.md §6) that the moves at
// last use left becomes a lent argument without its retain where the
// caller's own reference outlives the call, and every mark is cleared.
// Backend-independent; the driver runs it after the moves on every program
// and on the code of every module it writes to a `.pkm`, which so keeps no
// mark.

#pragma once

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
};

/// Lend the arguments of @p f's calls that its `retain lend` marks may lend
/// and clear every mark (docs/pir.md §12): the mark's value is read from a
/// local of @p f (the root), loaded or through unboxes and field and
/// element reads and addresses, in the block of the call, and is used by
/// the mark and by one box argument of that call alone, an argument with a
/// bit in the ownership mask of a lending callee (@p m, the module @p f is
/// in, declares the callees); and no statement from the root's load to the
/// call, nor one nested in those, stores into the root, takes its address,
/// releases a load of it, hands on a load of it that was not retained
/// first, or does anything but read through the pointer the root holds.
/// Then the copy's retain goes and the argument is `lent`.  Every other
/// mark is a plain `retain` from then on.
LendCounts lend(Function &f, const Module &m);

/// lend on every function the program defines, as a pass ("lending"),
/// counting "marks", "arguments lent" and "copies kept for the window".
std::unique_ptr<Pass> createLendingPass();

} // namespace paykan::pir
