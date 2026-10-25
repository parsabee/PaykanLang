// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (docs/pir.md §12): it counts the copies the lowering
// made only to hand a box argument over (`retain lend`, docs/pir.md §6)
// that the moves at last use left, and clears their mark.
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
};

/// Count the `retain lend` marks of @p f and clear them: each is a plain
/// `retain` from then on.  A mark the moves at last use met was a copy
/// they made a move, whose retain went with its mark; the ones left are
/// copies of a reference the caller keeps for the call.
LendCounts lend(Function &f);

/// lend on every function the program defines, as a pass ("lending"),
/// counting "marks".
std::unique_ptr<Pass> createLendingPass();

} // namespace paykan::pir
