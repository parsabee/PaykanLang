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

} // namespace paykan::pir
