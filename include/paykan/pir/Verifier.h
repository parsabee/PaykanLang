// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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

} // namespace paykan::pir
