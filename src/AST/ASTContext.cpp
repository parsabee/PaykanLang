// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTContext.h"
#include "Names.h"

namespace paykan {
namespace ast {

ASTContext::ASTContext()
    : IntTy(nullptr), FloatTy(nullptr), BoolTy(nullptr), CharTy(nullptr),
      VoidTy(nullptr), ObjTy(nullptr), StrTy(nullptr), ArrayTy(nullptr),
      FileTy(nullptr), ErrorTy(nullptr), IntBoxTy(nullptr), FloatBoxTy(nullptr),
      BoolBoxTy(nullptr) {
  // Reserve enough capacity to avoid repeated reallocations during parsing.
  // The bootstrap phase alone creates ~150 nodes; a typical program adds
  // a few hundred more.  512 slots eliminates most reallocation churn.
  Pool.reserve(512);
  IntTy = make<BuiltinType>(SourceLocation(), BuiltinType::Int);
  FloatTy = make<BuiltinType>(SourceLocation(), BuiltinType::Float);
  BoolTy = make<BuiltinType>(SourceLocation(), BuiltinType::Bool);
  CharTy = make<BuiltinType>(SourceLocation(), BuiltinType::Char);
  VoidTy = make<BuiltinType>(SourceLocation(), BuiltinType::Void);
  // Pre-allocate Obj, Str, Array, File, Error, and boxed primitives so all
  // method signatures are correct from the start.
  ObjTy = make<ClassType>(SourceLocation(), intern(names::kObj), nullptr);
  StrTy = make<ClassType>(SourceLocation(), intern(names::kString), nullptr);
  ArrayTy = make<ClassType>(SourceLocation(), intern(names::kArray), nullptr);
  FileTy = make<ClassType>(SourceLocation(), intern(names::kFile), nullptr);
  ErrorTy = make<ClassType>(SourceLocation(), intern(names::kError), nullptr);
  IntBoxTy = make<ClassType>(SourceLocation(), intern(names::kIntBox), nullptr);
  FloatBoxTy =
      make<ClassType>(SourceLocation(), intern(names::kFloatBox), nullptr);
  BoolBoxTy =
      make<ClassType>(SourceLocation(), intern(names::kBoolBox), nullptr);
  // These are the compiler builtins: Sema rejects any user class, enum, or
  // function that would reuse one of their names.  Flagging them here, at the
  // single registration site, keeps that knowledge out of Sema.
  for (ClassType *builtin : {ObjTy, StrTy, ArrayTy, FileTy, ErrorTy, IntBoxTy,
                             FloatBoxTy, BoolBoxTy})
    builtin->setBuiltin();
  buildObjectType();
  buildStringType();
  buildArrayType();
  buildFileType();
  buildErrorType();
  buildBoxedIntType();
  buildBoxedFloatType();
  buildBoxedBoolType();
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
//                 length(new), concat(new — mutates self) ]
//   fields    : _data (void*), _len (int)
//
void ASTContext::buildStringType() {
  // StrTy was pre-allocated as an empty shell; wire it up to ObjTy now
  // (this also inherits Obj's vtable and == / != operators).
  StrTy->setSuperClass(ObjTy);

  // Str is non-inheritable (final). This flag is checked by Sema;
  // it will be reused for user-defined `final` classes in the future.
  StrTy->setFinal();

  // Populate the pre-allocated shell via ClassTypeBuilder.
  // All method types are correct from the start — no patching required.
  ClassTypeBuilder(*this, StrTy)
      .addOp(BinaryOpcode::Add)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)         // slot 0 — override
      .method(names::kMethodToString, StrTy)         // slot 1 — override
      .method(names::kMethodEquals, BoolTy, {ObjTy}) // slot 2 — override
      .method(names::kMethodLength, IntTy)           // new
      .method(names::kMethodConcat, VoidTy, {StrTy}) // new
      .build();
}

// -- Bootstrap Array -------------------------------------------------------
//
// Array is a subtype of Obj.  Its vtable mirrors PaykanArrayVTable:
//   vtable : [ destroy(override), toString(override), equals(override),
//              len(new) ]
//
void ASTContext::buildArrayType() {
  ArrayTy->setSuperClass(ObjTy);

  ClassTypeBuilder(*this, ArrayTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)         // slot 0 — override
      .method(names::kMethodToString, StrTy)         // slot 1 — override
      .method(names::kMethodEquals, BoolTy, {ObjTy}) // slot 2 — override
      .method(names::kLen, IntTy)                    // slot 3 — new
      .build();
}

// -- Bootstrap File --------------------------------------------------------
//
// File inherits Obj.
//   operators : == !=
//   vtable    : [ destroy(override), toString(override), equals(override) ]
//
void ASTContext::buildFileType() {
  FileTy->setSuperClass(ObjTy);

  ClassTypeBuilder(*this, FileTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)         // slot 0 — override
      .method(names::kMethodToString, StrTy)         // slot 1 — override
      .method(names::kMethodEquals, BoolTy, {ObjTy}) // slot 2 — override
      .method(names::kMethodWrite, VoidTy, {StrTy})  // slot 3 — new
      .method(names::kMethodReadln,
              ObjTy) // slot 4 — new: Str on success, None at EOF
      .method(names::kMethodReadBytes, ObjTy,
              {IntTy})                   // slot 5 — new: read up to n bytes
      .method(names::kMethodRead, ObjTy) // slot 6 — new: read all remaining
      .build();
}

// -- Bootstrap Error -------------------------------------------------------
//
// Error inherits Obj.  Returned by open() when fopen fails.
//   operators : == !=
//   vtable    : [ destroy(override), toString(override), equals(override) ]
//
void ASTContext::buildErrorType() {
  ErrorTy->setSuperClass(ObjTy);

  ClassTypeBuilder(*this, ErrorTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)         // slot 0 — override
      .method(names::kMethodToString, StrTy)         // slot 1 — override
      .method(names::kMethodEquals, BoolTy, {ObjTy}) // slot 2 — override
      .build();
}

// -- Bootstrap Int / Float / Bool (boxed primitives) -------------------------
//
// These class types mirror the C runtime's PaykanInt / PaykanFloat / PaykanBool
// structs.  They inherit Obj and override only the three base vtable slots
// (destroy / toString / equals).  Marked final so no user class may extend
// them.

void ASTContext::buildBoxedIntType() {
  IntBoxTy->setSuperClass(ObjTy);
  IntBoxTy->setFinal();
  ClassTypeBuilder(*this, IntBoxTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)
      .method(names::kMethodToString, StrTy)
      .method(names::kMethodEquals, BoolTy, {ObjTy})
      .build();
}

void ASTContext::buildBoxedFloatType() {
  FloatBoxTy->setSuperClass(ObjTy);
  FloatBoxTy->setFinal();
  ClassTypeBuilder(*this, FloatBoxTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)
      .method(names::kMethodToString, StrTy)
      .method(names::kMethodEquals, BoolTy, {ObjTy})
      .build();
}

void ASTContext::buildBoxedBoolType() {
  BoolBoxTy->setSuperClass(ObjTy);
  BoolBoxTy->setFinal();
  ClassTypeBuilder(*this, BoolBoxTy)
      .addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, VoidTy)
      .method(names::kMethodToString, StrTy)
      .method(names::kMethodEquals, BoolTy, {ObjTy})
      .build();
}

// -- Canonical array types ---------------------------------------------------
//
// One ArrayType per (canonical) element type.  The parser still allocates a
// source-located ArrayType for every `T[]` annotation so diagnostics can point
// at it, but Sema resolves each one to the instance returned here, so two uses
// of `int[][]` share a single node and pointer comparison is sufficient.  This
// also keeps the specialized-class cache below keyed correctly for nested
// arrays: the element type of `int[][]` is the canonical `int[]` node, so
// Array<int[]> is built once rather than once per use site.
//
ArrayType *ASTContext::getArrayType(Type *elemTy) {
  auto it = ArrayTypes.find(elemTy);
  if (it != ArrayTypes.end())
    return it->second;
  auto *at = make<ArrayType>(SourceLocation(), elemTy);
  ArrayTypes[elemTy] = at;
  return at;
}

// -- Specialized per-element array types ------------------------------------
//
// Lazily create Array<int>, Array<float>, etc.  Each specialized type is a
// subtype of ArrayTy and adds push(elemTy)->void and pop()->elemTy so that
// Sema can type-check calls with the correct element type.
// These types are NOT used for vtable dispatch; CodeGen emits direct calls to
// PaykanArray_push / PaykanArray_pop for those methods.
//
ClassType *ASTContext::getOrCreateSpecializedArrayType(Type *elemTy) {
  auto it = SpecializedArrayTypes.find(elemTy);
  if (it != SpecializedArrayTypes.end())
    return it->second;

  std::string name = std::string(names::kArray) + "<" + typeName(elemTy) + ">";
  auto *specTy = make<ClassType>(SourceLocation(), intern(name), ArrayTy);
  ClassTypeBuilder(*this, specTy)
      .method(names::kPush, VoidTy, {elemTy}) // slot 0 in specialized type
      .method(names::kPop, elemTy)            // slot 1 in specialized type
      .build();
  SpecializedArrayTypes[elemTy] = specTy;
  SpecializedArrayElemTypes[specTy] = elemTy;
  return specTy;
}

Type *ASTContext::getSpecializedArrayElemType(ClassType *ct) const {
  auto it = SpecializedArrayElemTypes.find(ct);
  return it != SpecializedArrayElemTypes.end() ? it->second : nullptr;
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
  auto *m = Ctx.make<MethodDecl>(SourceLocation(), Ctx.intern(name), retTy,
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

ASTContext::ClassTypeBuilder ASTContext::buildClassType(const std::string &name,
                                                        ClassType *superClass) {
  auto *ty = make<ClassType>(SourceLocation(), intern(name), superClass);
  return ClassTypeBuilder(*this, ty);
}

BuiltinType *ASTContext::getBuiltinType(BuiltinType::Kind k) const {
  switch (k) {
  case BuiltinType::Int:
    return IntTy;
  case BuiltinType::Float:
    return FloatTy;
  case BuiltinType::Bool:
    return BoolTy;
  case BuiltinType::Char:
    return CharTy;
  case BuiltinType::Void:
    return VoidTy;
  }
  return VoidTy;
}

void ASTContext::registerClassType(ClassType *ct) {
  ClassTypes[ct->getName()] = ct;
}

ClassType *ASTContext::preRegisterClassType(const std::string &name,
                                            ClassType *superClass) {
  auto *ty = make<ClassType>(SourceLocation(), intern(name), superClass);
  ClassTypes[name] = ty;
  return ty;
}

ClassType *ASTContext::lookupClassType(const std::string &name) const {
  auto it = ClassTypes.find(name);
  return it != ClassTypes.end() ? it->second : nullptr;
}

EnumType *ASTContext::registerEnumType(const std::string &name,
                                       SourceLocation loc) {
  if (EnumTypes.count(name))
    return nullptr;
  auto *ty = make<EnumType>(loc, intern(name));
  EnumTypes[name] = ty;
  return ty;
}

EnumType *ASTContext::lookupEnumType(const std::string &name) const {
  auto it = EnumTypes.find(name);
  return it != EnumTypes.end() ? it->second : nullptr;
}

void ASTContext::addClassTypeAlias(const std::string &alias, ClassType *ct) {
  ClassTypes.emplace(alias, ct); // no-op if already present
}

void ASTContext::addEnumTypeAlias(const std::string &alias, EnumType *et) {
  EnumTypes.emplace(alias, et); // no-op if already present
}

Type *ASTContext::lookupType(const std::string &name) const {
  if (name == names::kTypeInt)
    return IntTy;
  if (name == names::kTypeFloat)
    return FloatTy;
  if (name == names::kTypeBool)
    return BoolTy;
  if (name == names::kTypeChar)
    return CharTy;
  if (name == names::kTypeVoid)
    return VoidTy;
  if (auto *et = lookupEnumType(name))
    return et;
  return lookupClassType(name);
}

std::string typeName(Type *ty) {
  if (!ty)
    return "unknown";
  if (auto *bt = dyn_cast<BuiltinType>(ty)) {
    switch (bt->getTypeKind()) {
    case BuiltinType::Int:
      return names::kTypeInt;
    case BuiltinType::Float:
      return names::kTypeFloat;
    case BuiltinType::Bool:
      return names::kTypeBool;
    case BuiltinType::Char:
      return names::kTypeChar;
    case BuiltinType::Void:
      return names::kTypeVoid;
    }
  }
  if (auto *ct = dyn_cast<ClassType>(ty))
    return ct->getName();
  if (auto *et = dyn_cast<EnumType>(ty))
    return et->getName();
  if (auto *at = dyn_cast<ArrayType>(ty))
    return typeName(at->getElementType()) + "[]";
  return "unknown";
}

} // namespace ast
} // namespace paykan
