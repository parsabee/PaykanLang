// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "AST.h"

namespace paykan {
namespace ast {

// -- BinaryExpr --------------------------------------------------------------

const char *BinaryExpr::getOpcodeStr() const {
  switch (Op) {
  case BinaryOpcode::Add: return "+";
  case BinaryOpcode::Sub: return "-";
  case BinaryOpcode::Mul: return "*";
  case BinaryOpcode::Div: return "/";
  case BinaryOpcode::Mod: return "%";
  case BinaryOpcode::Lt:  return "<";
  case BinaryOpcode::Gt:  return ">";
  case BinaryOpcode::Le:  return "<=";
  case BinaryOpcode::Ge:  return ">=";
  case BinaryOpcode::Eq:  return "==";
  case BinaryOpcode::Ne:  return "!=";
  }
  return "?";
}

// -- UnaryExpr ---------------------------------------------------------------

const char *UnaryExpr::getOpcodeStr() const {
  switch (Op) {
  case UnaryOpcode::Neg: return "-";
  case UnaryOpcode::Not: return "!";
  }
  return "?";
}

// -- BuiltinType -------------------------------------------------------------

void BuiltinType::initOps() {
  switch (TypeKind) {
  case Int:
  case Float:
    addUnaryOp(UnaryOpcode::Neg);
    addBinaryOp(BinaryOpcode::Add);
    addBinaryOp(BinaryOpcode::Sub);
    addBinaryOp(BinaryOpcode::Mul);
    addBinaryOp(BinaryOpcode::Div);
    addBinaryOp(BinaryOpcode::Mod);
    addBinaryOp(BinaryOpcode::Lt);
    addBinaryOp(BinaryOpcode::Gt);
    addBinaryOp(BinaryOpcode::Le);
    addBinaryOp(BinaryOpcode::Ge);
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    break;
  case Bool:
    addUnaryOp(UnaryOpcode::Not);
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    break;
  case Void:
    break;
  }
}

// -- ClassType ---------------------------------------------------------------

void ClassType::addMethod(MethodDecl *m) {
  Methods.push_back(m);
  if (m->isVirtual()) {
    // Check if this overrides a parent vtable slot.
    bool overridden = false;
    for (size_t i = 0; i < VTable.size(); ++i) {
      if (VTable[i]->getName() == m->getName()) {
        VTable[i] = m; // override
        overridden = true;
        break;
      }
    }
    if (!overridden)
      VTable.push_back(m); // new slot
  }
}

int ClassType::getVTableIndex(const std::string &name) const {
  for (size_t i = 0; i < VTable.size(); ++i)
    if (VTable[i]->getName() == name)
      return static_cast<int>(i);
  return -1;
}

MethodDecl *ClassType::findMethod(const std::string &name) const {
  for (auto *m : Methods)
    if (m->getName() == name)
      return m;
  return SuperClass ? SuperClass->findMethod(name) : nullptr;
}

bool ClassType::isSubtypeOf(const ClassType *other) const {
  if (this == other)
    return true;
  return SuperClass ? SuperClass->isSubtypeOf(other) : false;
}

} // namespace ast
} // namespace paykan
