// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// What a program's entry point cannot reach, dropped from the `pir` dialect
// before anything is lowered: a binary needs none of it.  (A `.pkm` keeps all
// of its module's code, since an importer may use any of it; this runs on a
// whole program only.)

#pragma once

#include "paykan/Status.h"

#include <mlir/IR/BuiltinOps.h>

namespace paykan::mlir_backend {

/// Erase from @p program (importProgram's: a nested module per PIR module)
/// every function, class and global the main module's `main` cannot reach.
/// MLIR's symbol DCE does the work, run until nothing changes.  Two things
/// it cannot see are given to it between runs: a definition that another
/// module declares (by name, in the declaration's `module` attribute) is
/// public while that declaration lives, and a class (named by a string,
/// not a symbol) is erased once no op names it, its vtable's functions
/// with it.  Each symbol keeps the visibility the import gave it.
Status dropUnreachable(mlir::ModuleOp program);

} // namespace paykan::mlir_backend
