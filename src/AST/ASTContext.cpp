// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT

#include "ASTContext.h"
#include "Names.h"

namespace paykan {
namespace ast {

ASTContext::ASTContext()
    : IntTy(nullptr), FloatTy(nullptr), BoolTy(nullptr), CharTy(nullptr),
      VoidTy(nullptr), ObjTy(nullptr), StrTy(nullptr), FileTy(nullptr),
      ErrorTy(nullptr) {
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
  FileTy = make<ClassType>(SourceLocation(), intern(names::kFile), nullptr);
  ErrorTy = make<ClassType>(SourceLocation(), intern(names::kError), nullptr);
  // The compiler builtins: Sema rejects any user class, enum, or function
  // that would reuse one of their names, and the lowering dispatches their
  // methods to the C runtime.
  for (ClassType *builtin : {ObjTy, StrTy, FileTy, ErrorTy})
    builtin->setBuiltin();
  buildObjectType();
  buildStringType();
  buildFileType();
  buildErrorType();
}

// -- Bootstrap of the builtin classes
//
// Their vtables mirror the runtime's (Runtime.h, PAYKAN_SLOT_*): Obj's three
// slots first, then the class's own, in the slot order given there.  The
// methods that only read are marked View, callable through a `view`.

ASTContext::ClassTypeBuilder &ASTContext::ClassTypeBuilder::objectSlots() {
  return addOp(BinaryOpcode::Eq)
      .addOp(BinaryOpcode::Ne)
      .method(names::kMethodDestroy, Ctx.VoidTy) // slot 0
      .method(names::kMethodToString, Ctx.StrTy, {},
              MethodDecl::View) // slot 1
      .method(names::kMethodEquals, Ctx.BoolTy, {Ctx.ObjTy},
              MethodDecl::View); // slot 2
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
      .method(names::kMethodLength, IntTy, {}, MethodDecl::View) // slot 3
      .method(names::kMethodConcat, VoidTy, {StrTy})             // slot 4
      .build();
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

// -- Canonical array and slice types

// One FixedArrayType per (canonical) element type and length.  The parser
// allocates a source-located node for every annotation, but Sema resolves
// each one to the instance returned here, so these types compare by pointer.
FixedArrayType *ASTContext::getFixedArrayType(Type *elemTy, int64_t length) {
  auto key = std::make_pair(elemTy, length);
  auto it = FixedArrayTypes.find(key);
  if (it != FixedArrayTypes.end())
    return it->second;
  auto *at = make<FixedArrayType>(SourceLocation(), elemTy, length);
  FixedArrayTypes[key] = at;
  return at;
}

// One SliceType per (canonical) element type, as for getFixedArrayType.
SliceType *ASTContext::getSliceType(Type *elemTy) {
  auto it = SliceTypes.find(elemTy);
  if (it != SliceTypes.end())
    return it->second;
  auto *st = make<SliceType>(SourceLocation(), elemTy);
  SliceTypes[elemTy] = st;
  return st;
}

// -- Canonical optional types
//
// One OptionalType per (canonical) inner type, as for getFixedArrayType: the
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

// -- Canonical tuple types
//
// One TupleType per (canonical) element-type list, as for getFixedArrayType:
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
                                     std::vector<Type *> params, uint8_t flags,
                                     ParamModes modes) {
  auto *m = Ctx.make<MethodDecl>(SourceLocation(), Ctx.intern(name), retTy,
                                 std::move(params), flags);
  m->setParamModes(std::move(modes));
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

StructType *ASTContext::registerStructType(const std::string &name,
                                           SourceLocation loc) {
  if (StructTypes.count(name))
    return nullptr;
  auto *ty = make<StructType>(loc, intern(name));
  StructTypes[name] = ty;
  return ty;
}

StructType *ASTContext::lookupStructType(const std::string &name) const {
  auto it = StructTypes.find(name);
  return it != StructTypes.end() ? it->second : nullptr;
}

void ASTContext::addStructTypeAlias(const std::string &alias, StructType *st) {
  StructTypes.emplace(alias, st); // no-op if already present
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
  if (auto *st = lookupStructType(name))
    return st;
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
  // Before TupleType, which a StructType is: a struct goes by its name.
  if (auto *st = dyn_cast<StructType>(ty))
    return st->getName();
  if (auto *at = dyn_cast<ArrayType>(ty))
    return typeName(at->getElementType()) + "[]";
  if (auto *ft = dyn_cast<FixedArrayType>(ty)) {
    // An array of arrays is written with its first index's length first, as
    // it is parsed: two `int[3]`s are `int[2][3]`.
    std::string lengths;
    Type *elem = ft;
    for (auto *f = ft; f; f = dyn_cast<FixedArrayType>(f->getElementType())) {
      lengths += "[" +
                 (f->isInferred() ? std::string("_")
                                  : std::to_string(f->getLength())) +
                 "]";
      elem = f->getElementType();
    }
    return typeName(elem) + lengths;
  }
  if (auto *ot = dyn_cast<OptionalType>(ty))
    return typeName(ot->getInnerType()) + "?";
  // A slice is spelled as its parameter's type is, its mode aside.
  if (auto *st = dyn_cast<SliceType>(ty))
    return typeName(st->getElementType()) + "[]";
  if (auto *gt = dyn_cast<GenericType>(ty)) {
    // An annotation not resolved yet: `Box<int>`, a bound `Into<int>`.
    std::string s = gt->getName() + "<";
    for (size_t i = 0; i < gt->getNumArgs(); ++i) {
      if (i)
        s += ", ";
      s += typeName(gt->getArgs()[i]);
    }
    return s + ">";
  }
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
