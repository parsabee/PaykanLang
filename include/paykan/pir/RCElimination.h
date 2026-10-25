// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The reference-count elimination pass (docs/pir.md §12): a `retain` and
// the `release` that ends what it made are removed together where nothing
// between them can let the object go, because something else keeps it
// alive.  Backend-independent; the driver runs it after the moves at last
// use on every program and on the code of every module it writes to a
// `.pkm`.

#pragma once

#include "paykan/pir/PIR.h"
#include "paykan/pir/Pass.h"

#include <cstdint>
#include <memory>

namespace paykan::pir {

/// What eliminateRedundantRC did to a function.
struct RCCounts {
  /// Retains removed.
  uint64_t Retains = 0;
  /// Releases removed with them: one per path out of each retain's range.
  uint64_t Releases = 0;
};

/// Remove from @p f each `retain` and the releases that end what it made
/// where the object is kept alive across that range without it.  A retain
/// makes a reference, held by the value retained or, when it is stored
/// right away into a box local stored nowhere else, by that local; on every
/// path from the retain, its *range* runs to the first `release` of the
/// value (of the local's value), every such release ending it.  The pair is
/// removed when the range is *balanced*: every path from the retain reaches
/// exactly one of those releases, or ends in `unreachable`, before it
/// leaves the block that defines the value (the end of the block, `ret`, a
/// `break` or `continue` out of it), none of them is reached by a path that
/// did not pass the retain, and no path reaches the retain again inside its
/// range; and when, at every point of the range, one of these keeps the
/// object alive:
///
/// * an *enclosing reference*: another reference the function holds on the
///   same value (a match subject's retain, a `weak.load`, a call's result, a
///   box parameter), not released or handed on inside the range;
/// * a *local*: the value was loaded from a box local whose every store
///   holds a reference of its own and that is not stored, released, handed
///   on or copied (retained and handed on, which the moves at last use may
///   make a hand-off) from the load to the end of the range, nor
///   addressed;
/// * *nothing that can let go*: the range holds no `release` but the
///   value's own, no `free`, no `vcall` or `icall`, no call but the
///   runtime's string, hash, rounding, allocation, disposal and panic
///   helpers, and no store that may overwrite a reference (`field.store`
///   or `ptr.store` of a box, `mem.copy`, `mem.zero`).
///
/// A box read from a field or an element has no reference of its own, a
/// `stable` one neither (the mark holds for its statement only), and no
/// local keeping it, so only the last can remove its first retain: with a
/// call in the range it stays, since the callee may write that field and
/// free the object (the field-load hazard), and so does every hold the
/// lowering makes; the retains nested in it go.  A local whose retain went
/// holds a borrowed reference from then on.  The whole runs again until it
/// removes nothing.
RCCounts eliminateRedundantRC(Function &f);

/// eliminateRedundantRC on every function the program defines, as a pass
/// ("rc-elimination"), counting "retains removed" and "releases removed".
std::unique_ptr<Pass> createRCEliminationPass();

} // namespace paykan::pir
