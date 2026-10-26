// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// PIR verifier (docs/pir.md §9).  A backend may assume everything the
// verifier checks; the driver verifies the lowering's output before any
// backend sees it, and tests verify the programs they build by hand.

#pragma once

#include "paykan/pir/PIR.h"

#include <string>
#include <vector>

namespace paykan::pir {

struct VerifyError {
  /// Where the error is: "<module>" or "<module>:@<function>".
  std::string Where;
  std::string Message;

  /// "where: message".
  std::string str() const;
};

/// Verify a whole program, including the cross-module references (an
/// `extern fn ... module "m"` must match a function defined in module m, and
/// likewise for extern classes).  Empty result = valid.
std::vector<VerifyError> verify(const Program &p);

/// Verify one module on its own.  References to other modules are only
/// checked for well-formedness, not resolved.
std::vector<VerifyError> verify(const Module &m);

/// One line per error.
std::string formatErrors(const std::vector<VerifyError> &errors);

/// Two module-less externs of a program that name one C symbol with
/// different signatures: a native function's (`$c.<symbol>`) and another
/// module's, or a native function's and the runtime's (`$rt.<symbol>`).
/// A C function has one signature, and a backend declares it once, so
/// verify() rejects such a program.
struct CSymbolClash {
  std::string Symbol;
  /// The first extern that names the symbol, and its module.
  const Module *FirstModule = nullptr;
  const Function *First = nullptr;
  /// A later one with another signature, and its module.
  const Module *OtherModule = nullptr;
  const Function *Other = nullptr;
};

/// Every module-less extern of @p p that names the C symbol of an earlier
/// one with another signature, in the order of the modules.
std::vector<CSymbolClash> cSymbolClashes(const Program &p);

} // namespace paykan::pir
