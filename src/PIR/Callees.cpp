// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// A module's callees by name (see Callees.h).

#include "Callees.h"

namespace paykan::pir {

Callees::Callees(const Module &m) {
  for (const Function &f : m.Functions)
    Functions.emplace(f.Name, &f);
  for (const Class &c : m.Classes)
    Classes.emplace(c.Name, &c);
}

bool Callees::lendable(const Instr &call, size_t a) const {
  const Signature *sig = &call.Sig;
  bool lend = false;
  if (call.Op == Opcode::Call) {
    auto it = Functions.find(call.Callee);
    if (it == Functions.end())
      return false;
    sig = &it->second->Sig;
    lend = lends(*it->second);
  } else {
    if (call.Op == Opcode::VCall && !call.ClassName.empty()) {
      auto it = Classes.find(call.ClassName);
      if (it == Classes.end() || call.Slot >= it->second->VTable.size())
        return false;
      sig = &it->second->VTable[call.Slot].Sig;
    }
    lend = lendsType(*sig);
  }
  const size_t first = firstParamArg(call);
  if (!lend || a < first || call.ownership(a) != ArgOwnership::Owned)
    return false;
  const size_t p = a - first;
  return p < sig->Params.size() && sig->Params[p] == Type::Box &&
         boxIndex(*sig, p) < kOwnershipBits;
}

} // namespace paykan::pir
