// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Passes over a PIR program and the manager that runs them (docs/pir.md
// §12).  A pass rewrites the program in place; the manager runs its passes
// in order and, when asked, verifies the program after each one, so a pass
// that breaks it is named.

#pragma once

#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::pir {

/// What the passes of one compilation did, as named counters: each pass
/// adds to its own ("moves-at-last-use" "transfers"), and `paykan --stats`
/// prints them.
class Statistics {
public:
  /// Add @p n to the counter @p counter of pass @p pass.  A counter is made
  /// the first time a pass adds to it, at 0 too: a pass that ran and found
  /// nothing to do still shows.
  void add(std::string_view pass, std::string_view counter, uint64_t n);
  /// The counter's value; 0 when no pass made it.
  uint64_t get(std::string_view pass, std::string_view counter) const;
  bool empty() const { return Counters.empty(); }
  /// "paykan pass stats:", then "  <pass> <counter> : <n>" for each
  /// counter, in the order they were made.
  void print(std::ostream &os) const;

private:
  struct Counter {
    std::string Pass;
    std::string Name;
    uint64_t N = 0;
  };
  std::vector<Counter> Counters;
};

class Pass {
public:
  virtual ~Pass() = default;
  /// The name a failure is reported under ("dead-code").
  virtual std::string_view name() const = 0;
  /// Rewrite @p program; what the pass did is added to @p stats.
  virtual Status run(Program &program, Statistics &stats) = 0;
};

class PassManager {
public:
  /// With @p verifyEach, the program is verified after every pass.
  explicit PassManager(bool verifyEach = false) : VerifyEach(verifyEach) {}

  void add(std::unique_ptr<Pass> pass) { Passes.push_back(std::move(pass)); }

  /// Run every pass on @p program, in the order added, each adding to
  /// @p stats; the first failure stops the run.
  Status run(Program &program, Statistics &stats) const;
  /// run, its statistics dropped.
  Status run(Program &program) const;

private:
  std::vector<std::unique_ptr<Pass>> Passes;
  bool VerifyEach;
};

} // namespace paykan::pir
