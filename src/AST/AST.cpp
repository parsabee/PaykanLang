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
  case Char:
    addBinaryOp(BinaryOpcode::Eq);
    addBinaryOp(BinaryOpcode::Ne);
    addBinaryOp(BinaryOpcode::Lt);
    addBinaryOp(BinaryOpcode::Gt);
    addBinaryOp(BinaryOpcode::Le);
    addBinaryOp(BinaryOpcode::Ge);
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
  auto it = VTableIndex.find(m->getName());
  if (it != VTableIndex.end()) {
    VTable[it->second] = m; // override, slot index unchanged
    return;
  }
  // New slot.
  VTableIndex[m->getName()] = static_cast<int>(VTable.size());
  VTable.push_back(m);
}

int ClassType::getVTableIndex(const std::string &name) const {
  auto it = VTableIndex.find(name);
  return it != VTableIndex.end() ? it->second : -1;
}

MethodDecl *ClassType::findMethod(const std::string &name) const {
  // __init__ lives outside the vtable.
  if (name == names::kMethodInit)
    return InitMethod; // nullptr if not declared in this class
  auto it = VTableIndex.find(name);
  if (it != VTableIndex.end())
    return VTable[it->second];
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
