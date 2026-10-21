// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Switch formation, the last step of the shared lowering: a chain of scf.if
// comparing one integer value with constants -- what `match` on an enum or
// on integer or char literals lowers to (src/Lowering/LoweringMatch.cpp) --
// becomes one scf.index_switch.  Both targets lower it to their own switch:
// the LLVM path to an LLVM `switch` (cf.switch), the C path to a C `switch`
// (emitc.switch).

#pragma once

#include <mlir/IR/Operation.h>
#include <mlir/Pass/Pass.h>

#include <memory>

namespace paykan::mlir_backend {

/// Turn every chain of two or more distinct cases in @p root into an
/// scf.index_switch.
void formSwitches(mlir::Operation *root);

/// formSwitches() as a pass.
std::unique_ptr<mlir::Pass> createSwitchFormationPass();

} // namespace paykan::mlir_backend
