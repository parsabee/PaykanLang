// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// A module's callees by name, as the lending pass (Lend.cpp) and the
// reference-count elimination (RCElimination.cpp) look up whether a call's
// argument has a bit in its callee's ownership mask (docs/pir.md §3).

#pragma once

#include "paykan/pir/PIR.h"

#include <cstddef>
#include <string_view>
#include <unordered_map>

namespace paykan::pir {

/// The functions and classes a module declares, by name.
class Callees {
public:
  explicit Callees(const Module &m);

  /// Whether argument @p a of the call @p call may be lent: a box parameter
  /// of a lending callee (docs/pir.md §3) with a bit in its ownership mask,
  /// handed over.
  bool lendable(const Instr &call, size_t a) const;

private:
  std::unordered_map<std::string_view, const Function *> Functions;
  std::unordered_map<std::string_view, const Class *> Classes;
};

} // namespace paykan::pir
