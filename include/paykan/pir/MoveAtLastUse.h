// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The moves-at-last-use pass (docs/pir.md §12): a copy of a local at its
// last use becomes a move.  The copy's retain goes, and the local's own
// reference is handed to the destination instead, the local's slot cleared
// so that its release does nothing.  Backend-independent; the driver runs it
// on every program and on the code of every module it writes to a `.pkm`.

#pragma once

#include "paykan/pir/PIR.h"
#include "paykan/pir/Pass.h"

#include <cstdint>
#include <memory>

namespace paykan::pir {

/// What moveAtLastUse did to a function.
struct MoveCounts {
  /// Copies turned into moves: a box handed on, or a value held in memory
  /// copied whole.
  uint64_t Transfers = 0;
  /// The retains those copies made, removed.
  uint64_t Retains = 0;
  /// The releases removed because the slot they release is null on every
  /// path to them.
  uint64_t Releases = 0;
};

/// Turn every copy at a local's last use in @p f into a move.  A copy is a
/// `retain` of a `box` local's value handed to a consumer (a call's
/// argument, which the callee releases; a `store` into a local, a
/// `field.store` or a `ptr.store`; a `ret`), or the `mem.copy` of a whole
/// `mem` local followed or preceded by the retains of the references it
/// duplicates.  It is a last use when, on every path from it, the next thing
/// that touches the local's reference is the `release` of that reference
/// (scope exit, or the release before an assignment), with no read of the
/// local and no use of a value derived from it in between; and when, from
/// where the destination holds the reference to that release, nothing that
/// may release a reference comes while a borrowed reference (a field read in
/// place, `self`, what an `inout` parameter points at) is in use across it,
/// since the local may be what keeps that reference's object alive.  The
/// retain then goes and `null` is stored into the slot: the release, still
/// there for the paths that did not move, releases nothing on the paths
/// that did.  Releases of a slot that is null on every path are removed; a
/// release after an `if` or a `switch` whose arms all moved but one goes
/// into that arm; null stores nothing reads are removed; and the whole runs
/// again until it moves nothing more.  Never moved: `self`, `inout` and
/// other addressed slots, a `view`'s slot, fields, array elements, parts of
/// a held value, `weak` locals, and a closure's captures.
MoveCounts moveAtLastUse(Function &f);

/// moveAtLastUse on every function the program defines, as a pass
/// ("moves-at-last-use"), counting "transfers", "retains removed" and
/// "releases removed".
std::unique_ptr<Pass> createMovesAtLastUsePass();

} // namespace paykan::pir
