// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "paykan/pir/PIR.h"

#include "paykan/pir/Codes.h"

namespace paykan::pir {

const char *typeName(Type t) { return typeEntry(t).Name; }

const Function *Module::findFunction(const std::string &name) const {
  for (const Function &f : Functions)
    if (f.Name == name)
      return &f;
  return nullptr;
}

const Class *Module::findClass(const std::string &name) const {
  for (const Class &c : Classes)
    if (c.Name == name)
      return &c;
  return nullptr;
}

bool lendsType(const Signature &sig) {
  for (Type t : sig.Params)
    if (t == Type::Box)
      return true;
  return false;
}

bool lends(const Function &f) {
  return !(f.IsExtern && f.Module.empty()) && lendsType(f.Sig);
}

size_t boxIndex(const Signature &sig, size_t i) {
  size_t k = 0;
  for (size_t p = 0; p < i && p < sig.Params.size(); ++p)
    if (sig.Params[p] == Type::Box)
      ++k;
  return k;
}

Signature machineSignature(const Signature &sig, bool lend) {
  Signature m = sig;
  if (lend)
    m.Params.push_back(Type::I64);
  return m;
}

bool sameConstant(const Operand &a, const Operand &b) {
  if (a.V.index() != b.V.index())
    return false;
  if (const auto *x = std::get_if<int64_t>(&a.V))
    return *x == std::get<int64_t>(b.V);
  if (const auto *x = std::get_if<bool>(&a.V))
    return *x == std::get<bool>(b.V);
  if (const auto *x = std::get_if<char>(&a.V))
    return *x == std::get<char>(b.V);
  return false;
}

} // namespace paykan::pir
