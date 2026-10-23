// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Passes over a PIR program and the manager that runs them (docs/pir.md
// §12).  A pass rewrites the program in place; the manager runs its passes
// in order and, when asked, verifies the program after each one, so a pass
// that breaks it is named.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <memory>
#include <string_view>
#include <vector>

namespace paykan::pir {

class Pass {
public:
  virtual ~Pass() = default;
  /// The name a failure is reported under ("dead-code").
  virtual std::string_view name() const = 0;
  virtual Status run(Program &program) = 0;
};

class PassManager {
public:
  /// With @p verifyEach, the program is verified after every pass.
  explicit PassManager(bool verifyEach = false) : VerifyEach(verifyEach) {}

  void add(std::unique_ptr<Pass> pass) { Passes.push_back(std::move(pass)); }

  /// Run every pass on @p program, in the order added; the first failure
  /// stops the run.
  Status run(Program &program) const;

private:
  std::vector<std::unique_ptr<Pass>> Passes;
  bool VerifyEach;
};

} // namespace paykan::pir
