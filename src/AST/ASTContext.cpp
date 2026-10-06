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
      BoolBoxTy(nullptr), CharBoxTy(nullptr), TupleTy(nullptr) {
  Pool.reserve(512); // the bootstrap alone makes ~150 nodes
  IntTy = make<BuiltinType>(SourceLocation(), BuiltinType::Int);
  FloatTy = make<BuiltinType>(SourceLocation(), BuiltinType::Float);
  BoolTy = make<BuiltinType>(SourceLocation(), BuiltinType::Bool);
  CharTy = make<BuiltinType>(SourceLocation(), BuiltinType::Char);
  VoidTy = make<BuiltinType>(SourceLocation(), BuiltinType::Void);
  PoisonTy = make<PoisonType>(SourceLocation());
  // Every builtin class is allocated before any is populated, so a method
  // signature can name any of them.
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
  CharBoxTy =
      make<ClassType>(SourceLocation(), intern(names::kCharBox), nullptr);
  TupleTy = make<ClassType>(SourceLocation(), intern(names::kTuple), nullptr);
  // The compiler builtins: Sema rejects any user class, enum, or function
  // that would reuse one of their names, and the lowering dispatches their
  // methods to the C runtime.
  for (ClassType *builtin : {ObjTy, StrTy, ArrayTy, FileTy, ErrorTy, IntBoxTy,
                             FloatBoxTy, BoolBoxTy, CharBoxTy, TupleTy})
    builtin->setBuiltin();
  buildObjectType();
  buildStringType();
  buildArrayType();
  buildTupleType();
  buildFileType();
  buildErrorType();
  for (ClassType *box : {IntBoxTy, FloatBoxTy, BoolBoxTy, CharBoxTy})
    buildBoxedType(box);
}

// -- Bootstrap of the builtin classes
//
// Their vtables mirror the runtime's (Runtime.h, PAYKAN_SLOT_*): Obj's three
// slots first, then the class's own, in the slot order given there.

ASTContext::ClassTypeBuilder &ASTContext::ClassTypeBuilder::objectSlots() {
  return addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, Ctx.VoidTy)              // slot 0
      .method(names::kMethodToString, Ctx.StrTy)              // slot 1
      .method(names::kMethodEquals, Ctx.BoolTy, {Ctx.ObjTy}); // slot 2
}

void ASTContext::buildObjectType() {
  ClassTypeBuilder(*this, ObjTy).objectSlots().build();
}

// Str is final; concat mutates self.
void ASTContext::buildStringType() {
  StrTy->setSuperClass(ObjTy);
  StrTy->setFinal();
  ClassTypeBuilder(*this, StrTy)
      .addOp(BinaryOpcode::Add)
      .objectSlots()
      .method(names::kMethodLength, IntTy)           // slot 3
      .method(names::kMethodConcat, VoidTy, {StrTy}) // slot 4
      .build();
}

void ASTContext::buildArrayType() {
  ArrayTy->setSuperClass(ObjTy);
  ClassTypeBuilder(*this, ArrayTy)
      .objectSlots()
      .method(names::kLen, IntTy) // slot 3
      .build();
}

// Tuple is the final base class of every tuple value (one runtime object
// backs them all, Runtime/Tuple.c); it has no methods beyond Obj's.
void ASTContext::buildTupleType() {
  TupleTy->setSuperClass(ObjTy);
  TupleTy->setFinal();
  ClassTypeBuilder(*this, TupleTy).objectSlots().build();
}

void ASTContext::buildFileType() {
  FileTy->setSuperClass(ObjTy);
  ClassTypeBuilder(*this, FileTy)
      .objectSlots()
      .method(names::kMethodWrite, VoidTy, {StrTy}) // slot 3
      .method(names::kMethodReadln, ObjTy) // slot 4: Str, or None at EOF
      .method(names::kMethodReadBytes, ObjTy, {IntTy}) // slot 5: up to n bytes
      .method(names::kMethodRead, ObjTy)               // slot 6: the rest
      .build();
}

void ASTContext::buildErrorType() {
  ErrorTy->setSuperClass(ObjTy);
  ClassTypeBuilder(*this, ErrorTy).objectSlots().build();
}

// A boxed primitive (Int, Float, Bool, Char) is final and has Obj's slots.
void ASTContext::buildBoxedType(ClassType *ty) {
  ty->setSuperClass(ObjTy);
  ty->setFinal();
  ClassTypeBuilder(*this, ty).objectSlots().build();
}

// -- Canonical array types
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

// -- Canonical optional types
//
// One OptionalType per (canonical) inner type, mirroring getArrayType: the
// parser's source-located `T?` nodes are resolved by Sema to the instance
// returned here so that `Node?` in a field, a parameter, and a match subject
// is one node and compares by pointer.
//
OptionalType *ASTContext::getOptionalType(Type *innerTy) {
  auto it = OptionalTypes.find(innerTy);
  if (it != OptionalTypes.end())
    return it->second;
  auto *ot = make<OptionalType>(SourceLocation(), innerTy);
  OptionalTypes[innerTy] = ot;
  return ot;
}

// -- Specialized per-element array types
//
// Lazily create Array<int>, Array<float>, etc.  Each specialized type is a
// subtype of ArrayTy and adds push(elemTy)->void and pop()->elemTy so that
// Sema can type-check calls with the correct element type.
// These types are not used for vtable dispatch: the lowering emits direct
// calls to PaykanArray_push / PaykanArray_pop for those methods.
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

// -- Canonical tuple types
//
// One TupleType per (canonical) element-type list, exactly like getArrayType:
// the parser allocates a source-located TupleType for every `(T1, T2)`
// annotation, Sema resolves each to the instance returned here, and nested
// tuples share their inner nodes, so resolved tuple types compare by pointer.
//
TupleType *ASTContext::getTupleType(std::vector<Type *> elemTys) {
  auto it = TupleTypes.find(elemTys);
  if (it != TupleTypes.end())
    return it->second;
  auto *tt = make<TupleType>(SourceLocation(), elemTys);
  TupleTypes[std::move(elemTys)] = tt;
  return tt;
}

// -- Specialized per-element-list tuple types
//
// Lazily create `Tuple<int, Str>` etc.  Each is a subtype of TupleTy adding no
// methods; the specialization only gives a tuple-typed receiver a ClassType
// to resolve `toString` / `equals` against (and `==` / `!=`, which lower to
// `equals`).  Every specialization shares the single runtime vtable
// PaykanTuple_vtable — the element kinds live in the object, not the vtable.
// Flagged builtin so the lowering dispatches its methods with the runtime ABI.
//
ClassType *ASTContext::getOrCreateSpecializedTupleType(TupleType *tt) {
  auto it = SpecializedTupleTypes.find(tt);
  if (it != SpecializedTupleTypes.end())
    return it->second;

  std::string name = std::string(names::kTuple) + "<";
  for (size_t i = 0; i < tt->getArity(); ++i) {
    if (i)
      name += ", ";
    name += typeName(tt->getElementType(i));
  }
  name += ">";
  auto *specTy = make<ClassType>(SourceLocation(), intern(name), TupleTy);
  specTy->setBuiltin();
  specTy->setFinal();
  ClassTypeBuilder(*this, specTy).build();
  SpecializedTupleTypes[tt] = specTy;
  SpecializedTupleElemTypes[specTy] = tt;
  return specTy;
}

TupleType *ASTContext::getSpecializedTupleElemType(ClassType *ct) const {
  auto it = SpecializedTupleElemTypes.find(ct);
  return it != SpecializedTupleElemTypes.end() ? it->second : nullptr;
}

// -- ClassTypeBuilder

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

// -- ASTContext

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
  if (auto *ot = dyn_cast<OptionalType>(ty))
    return typeName(ot->getInnerType()) + "?";
  if (auto *tt = dyn_cast<TupleType>(ty)) {
    // "(int, Str)" — the same spelling the parser accepts, so the serialised
    // form round-trips through module export (SemaImport::resolveExportedType
    // parses it back, nesting and trailing "[]" included).
    std::string s = "(";
    for (size_t i = 0; i < tt->getArity(); ++i) {
      if (i)
        s += ", ";
      s += typeName(tt->getElementType(i));
    }
    return s + ")";
  }
  // Never shown for a poisoned binder (its uses are not diagnosed); spelled
  // so that it cannot be mistaken for a source type if it ever is.
  if (isa<PoisonType>(ty))
    return "<error>";
  return "unknown";
}

} // namespace ast
} // namespace paykan
