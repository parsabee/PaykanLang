// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

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
