// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTContext.h"
#include "Names.h"

namespace paykan {
namespace ast {

ASTContext::ASTContext()
    : IntTy(make<BuiltinType>(SourceLocation(), BuiltinType::Int)),
      FloatTy(make<BuiltinType>(SourceLocation(), BuiltinType::Float)),
      BoolTy(make<BuiltinType>(SourceLocation(), BuiltinType::Bool)),
      VoidTy(make<BuiltinType>(SourceLocation(), BuiltinType::Void)),
      ObjTy(nullptr), StrTy(nullptr) {
  // Pre-allocate both ObjTy and StrTy so that Obj's own methods can
  // reference them directly — no post-hoc patching required.
  ObjTy = make<ClassType>(SourceLocation(), names::kObj,    nullptr);
  StrTy = make<ClassType>(SourceLocation(), names::kString, nullptr);
  buildObjectType();
  buildStringType();
}

// -- Bootstrap Obj ----------------------------------------------------------
//
// Obj is the root of the class hierarchy.
//   operators : == !=
//   vtable    : [ toString, equals ]
//
//
void ASTContext::buildObjectType() {
  // ObjTy and StrTy are both pre-allocated before this call, so all
  // method signatures are correct from the start — no patching required.
  ClassTypeBuilder(*this, ObjTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)         // slot 0
      .method(names::kMethodToString, StrTy)         // slot 1
      .method(names::kMethodEquals, BoolTy, {ObjTy}) // slot 2
      .build();
}

// -- Bootstrap Str ---------------------------------------------------------
//
// Str inherits Obj.
//   operators : + == !=
//   vtable    : [ toString(override), equals(override),
//                 length(new), concat(new) ]
//   fields    : _data (void*), _len (int)
//
void ASTContext::buildStringType() {
  // StrTy was pre-allocated as an empty shell; wire it up to ObjTy now
  // (this also inherits Obj's vtable and == / != operators).
  StrTy->setSuperClass(ObjTy);

  // Populate the pre-allocated shell via ClassTypeBuilder.
  // All method types are correct from the start — no patching required.
  ClassTypeBuilder(*this, StrTy)
      .addOp(BinaryOpcode::Add)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)            // slot 0 — override
      .method(names::kMethodToString, StrTy)            // slot 1 — override
      .method(names::kMethodEquals, BoolTy, {ObjTy})    // slot 2 — override
      .method(names::kMethodLength, IntTy)              // new
      .method(names::kMethodConcat, StrTy, {StrTy})     // new
      .field(names::kFieldData, VoidTy)
      .field(names::kFieldLen, IntTy)
      .build();
}

// -- ClassTypeBuilder --------------------------------------------------------

ASTContext::ClassTypeBuilder::ClassTypeBuilder(ASTContext &ctx, ClassType *ty)
    : Ctx(ctx), Ty(ty) {}

ASTContext::ClassTypeBuilder &
ASTContext::ClassTypeBuilder::addOp(BinaryOpcode op) {
  Ty->addBinaryOp(op);
  return *this;
}

ASTContext::ClassTypeBuilder &
ASTContext::ClassTypeBuilder::addOp(UnaryOpcode op) {
  Ty->addUnaryOp(op);
  return *this;
}

ASTContext::ClassTypeBuilder &
ASTContext::ClassTypeBuilder::method(const std::string &name, Type *retTy,
                                     std::vector<Type *> params,
                                     uint8_t flags) {
  auto *m = Ctx.make<MethodDecl>(SourceLocation(), name, retTy,
                                 std::move(params), flags);
  Ty->addMethod(m);
  return *this;
}

ASTContext::ClassTypeBuilder &
ASTContext::ClassTypeBuilder::field(const std::string &name, Type *ty) {
  Ty->addField(name, ty);
  return *this;
}

ClassType *ASTContext::ClassTypeBuilder::build() {
  Ctx.registerClassType(Ty);
  return Ty;
}

// -- ASTContext --------------------------------------------------------------

ASTContext::ClassTypeBuilder
ASTContext::buildClassType(const std::string &name,
                           ClassType *superClass) {
  auto *ty = make<ClassType>(SourceLocation(), name, superClass);
  return ClassTypeBuilder(*this, ty);
}

BuiltinType *ASTContext::getBuiltinType(BuiltinType::Kind k) const {
  switch (k) {
  case BuiltinType::Int:   return IntTy;
  case BuiltinType::Float: return FloatTy;
  case BuiltinType::Bool:  return BoolTy;
  case BuiltinType::Void:  return VoidTy;
  }
  return VoidTy;
}

void ASTContext::registerClassType(ClassType *ct) {
  ClassTypes[ct->getName()] = ct;
}

ClassType *ASTContext::preRegisterClassType(const std::string &name,
                                            ClassType *superClass) {
  auto *ty = make<ClassType>(SourceLocation(), name, superClass);
  ClassTypes[name] = ty;
  return ty;
}

ClassType *ASTContext::lookupClassType(const std::string &name) const {
  auto it = ClassTypes.find(name);
  return it != ClassTypes.end() ? it->second : nullptr;
}

void ASTContext::addClassTypeAlias(const std::string &alias, ClassType *ct) {
  ClassTypes.emplace(alias, ct); // no-op if already present
}

Type *ASTContext::lookupType(const std::string &name) const {
  if (name == names::kTypeInt)   return IntTy;
  if (name == names::kTypeFloat) return FloatTy;
  if (name == names::kTypeBool)  return BoolTy;
  if (name == names::kTypeVoid)  return VoidTy;
  return lookupClassType(name);
}

} // namespace ast
} // namespace paykan
