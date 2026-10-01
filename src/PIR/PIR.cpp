// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "paykan/pir/PIR.h"

namespace paykan::pir {

const char *typeName(Type t) {
  switch (t) {
  case Type::Void:
    return "void";
  case Type::I64:
    return "i64";
  case Type::F64:
    return "f64";
  case Type::Bool:
    return "bool";
  case Type::Char:
    return "char";
  case Type::Box:
    return "box";
  case Type::Obj:
    return "obj";
  case Type::Ptr:
    return "ptr";
  }
  return "?";
}

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
