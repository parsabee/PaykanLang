// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "AST.h"
#include "Names.h"

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
  case BinaryOpcode::And: return "&&";
  case BinaryOpcode::Or:  return "||";  
  case BinaryOpcode::Count: break;
  }
  __builtin_unreachable();
}

// -- UnaryExpr ---------------------------------------------------------------

const char *UnaryExpr::getOpcodeStr() const {
  switch (Op) {
  case UnaryOpcode::Neg: return "-";
  case UnaryOpcode::Not: return "!";
  case UnaryOpcode::Count: break;
  }
  __builtin_unreachable();
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
    addBinaryOp(BinaryOpcode::And);
    addBinaryOp(BinaryOpcode::Or);
    break;
  case Void:
    break;
  }
}

// -- ClassType ---------------------------------------------------------------

void ClassType::addMethod(MethodDecl *m) {
  // __init__ is a static constructor helper — it has no vtable slot.
  if (m->getName() == names::kMethodInit) {
    InitMethod = m;
    return;
  }
  // Check if this overrides an existing slot.
  for (size_t i = 0; i < VTable.size(); ++i) {
    if (VTable[i]->getName() == m->getName()) {
      VTable[i] = m; // override
      return;
    }
  }
  VTable.push_back(m); // new slot
}

int ClassType::getVTableIndex(const std::string &name) const {
  for (size_t i = 0; i < VTable.size(); ++i)
    if (VTable[i]->getName() == name)
      return static_cast<int>(i);
  return -1;
}

MethodDecl *ClassType::findMethod(const std::string &name) const {
  // __init__ lives outside the vtable.
  if (name == names::kMethodInit)
    return InitMethod; // nullptr if not declared in this class
  for (auto *m : VTable)
    if (m->getName() == name)
      return m;
  // Walk up the inheritance chain.
  if (SuperClass)
    return SuperClass->findMethod(name);
  return nullptr;
}

bool ClassType::isSubtypeOf(const ClassType *other) const {
  if (this == other)
    return true;
  return SuperClass ? SuperClass->isSubtypeOf(other) : false;
}

} // namespace ast
} // namespace paykan
