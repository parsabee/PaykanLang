// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTContext.h"

namespace paykan {
namespace ast {

ASTContext::ASTContext()
    : IntTy(make<BuiltinType>(SourceLocation(), BuiltinType::Int)),
      FloatTy(make<BuiltinType>(SourceLocation(), BuiltinType::Float)),
      BoolTy(make<BuiltinType>(SourceLocation(), BuiltinType::Bool)),
      VoidTy(make<BuiltinType>(SourceLocation(), BuiltinType::Void)),
      ObjectTy(nullptr), StringTy(nullptr) {
  buildObjectType();
  buildStringType();
}

// -- Bootstrap Object -------------------------------------------------------
//
// Object is the root of the class hierarchy.
//   operators : == !=
//   vtable    : [ toString, equals ]
//
// Note: toString's return type is ObjectTy (a placeholder) because StringTy
// has not been created yet.  It could be patched later if needed.
//
void ASTContext::buildObjectType() {
  ObjectTy = buildClassType("Object")
                 .addOp(BinaryOpcode::Eq)
                 .addOp(BinaryOpcode::Ne)
                 .method("toString", /*retTy=*/nullptr) // patched below
                 .method("equals", BoolTy, {/*ObjectTy*/nullptr})
                 .build();

  // Patch forward references that needed ObjectTy itself.
  // toString() -> ObjectTy  (placeholder for StringTy)
  // equals(Object) param[0] -> ObjectTy
  for (auto *m : ObjectTy->getMethods()) {
    if (m->getName() == "toString")
      m->setReturnType(ObjectTy);
    if (m->getName() == "equals")
      m->setParamType(0, ObjectTy);
  }
}

// -- Bootstrap String -------------------------------------------------------
//
// String inherits Object.
//   operators : + == !=
//   vtable    : [ toString(override), equals(override),
//                 length(new), concat(new) ]
//   fields    : _data (void*), _len (int)
//
void ASTContext::buildStringType() {
  StringTy = buildClassType("String", ObjectTy)
                 .addOp(BinaryOpcode::Add)
                 .addOp(BinaryOpcode::Eq)
                 .addOp(BinaryOpcode::Ne)
                 .method("toString", StringTy)                  // override
                 .method("equals", BoolTy, {ObjectTy})          // override
                 .method("length", IntTy)                       // new
                 .method("concat", StringTy, {StringTy})        // new
                 .field("_data", VoidTy)
                 .field("_len", IntTy)
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

ClassType *ASTContext::lookupClassType(const std::string &name) const {
  auto it = ClassTypes.find(name);
  return it != ClassTypes.end() ? it->second : nullptr;
}

Type *ASTContext::lookupType(const std::string &name) const {
  if (name == "int")   return IntTy;
  if (name == "float") return FloatTy;
  if (name == "bool")  return BoolTy;
  if (name == "void")  return VoidTy;
  return lookupClassType(name);
}

} // namespace ast
} // namespace paykan
