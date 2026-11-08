// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0

#include "ASTContext.h"
#include "Names.h"

namespace paykan {
namespace ast {

ASTContext::ASTContext()
    : IntTy(nullptr), FloatTy(nullptr), BoolTy(nullptr), CharTy(nullptr),
      VoidTy(nullptr), ObjTy(nullptr), StrTy(nullptr) {
  Pool.reserve(512); // the bootstrap alone makes ~150 nodes
  for (unsigned k = 0; k < BuiltinType::kNumKinds; ++k)
    BuiltinTys[k] = make<BuiltinType>(SourceLocation(), BuiltinType::Kind(k));
  IntTy = BuiltinTys[BuiltinType::Int];
  FloatTy = BuiltinTys[BuiltinType::Float];
  BoolTy = BuiltinTys[BuiltinType::Bool];
  CharTy = BuiltinTys[BuiltinType::Char];
  VoidTy = BuiltinTys[BuiltinType::Void];
  PoisonTy = make<PoisonType>(SourceLocation());
  // Every builtin class is allocated before any is populated, so a method
  // signature can name any of them.
  ObjTy = make<ClassType>(SourceLocation(), intern(names::kObj), nullptr);
  StrTy = make<ClassType>(SourceLocation(), intern(names::kString), nullptr);
  // The compiler builtins: Sema rejects any user class, enum, or function
  // that would reuse one of their names, and the lowering dispatches their
  // methods to the C runtime.
  for (ClassType *builtin : {ObjTy, StrTy})
    builtin->setBuiltin();
  buildObjectType();
  buildStringType();
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
              MethodDecl::View) // slot 2
      .method(names::kMethodWalk, Ctx.VoidTy, {},
              MethodDecl::View); // slot 3 (the concurrency prototype)
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

// -- Canonical array and slice types

// One FixedArrayType per (canonical) element type and length.  The parser
// allocates a source-located node for every annotation, but Sema resolves
// each one to the instance returned here, so these types compare by pointer.
FixedArrayType *ASTContext::getFixedArrayType(Type *elemTy, int64_t length,
                                              Qualifier elemQual) {
  auto key = std::make_tuple(elemTy, length, elemQual.code());
  auto it = FixedArrayTypes.find(key);
  if (it != FixedArrayTypes.end())
    return it->second;
  auto *at =
      make<FixedArrayType>(SourceLocation(), elemTy, length, elemQual.form());
  FixedArrayTypes[key] = at;
  return at;
}

// One SliceType per (canonical) element type, as for getFixedArrayType.
SliceType *ASTContext::getSliceType(Type *elemTy, Qualifier elemQual) {
  auto key = std::make_pair(elemTy, elemQual.code());
  auto it = SliceTypes.find(key);
  if (it != SliceTypes.end())
    return it->second;
  auto *st = make<SliceType>(SourceLocation(), elemTy, elemQual.form());
  SliceTypes[key] = st;
  return st;
}

Type *ASTContext::getQualifiedType(Qualifier q, Type *innerTy) {
  if (q.isUnique())
    return innerTy;
  auto key = std::make_pair(q.code(), innerTy);
  auto it = QualifiedTypes.find(key);
  if (it != QualifiedTypes.end())
    return it->second;
  auto *qt = make<QualifiedType>(SourceLocation(), q.form(), innerTy,
                                 /*canonical=*/true);
  QualifiedTypes[key] = qt;
  return qt;
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

AtomicType *ASTContext::getAtomicType(Type *innerTy) {
  auto it = AtomicTypes.find(innerTy);
  if (it != AtomicTypes.end())
    return it->second;
  auto *at = make<AtomicType>(SourceLocation(), innerTy);
  AtomicTypes[innerTy] = at;
  return at;
}

TaskType *ASTContext::getTaskType(Type *innerTy) {
  auto it = TaskTypes.find(innerTy);
  if (it != TaskTypes.end())
    return it->second;
  auto *tt = make<TaskType>(SourceLocation(), innerTy);
  TaskTypes[innerTy] = tt;
  return tt;
}

// -- Canonical tuple types
//
// One TupleType per (canonical) element-type list, as for getFixedArrayType:
// the parser allocates a source-located TupleType for every `(T1, T2)`
// annotation, Sema resolves each to the instance returned here, and nested
// tuples share their inner nodes, so resolved tuple types compare by pointer.
//
namespace {

/// @p quals as forms (Sema's mark dropped), or none when every one is
/// unique (the same canonical type).
std::vector<Qualifier> normalQuals(std::vector<Qualifier> quals) {
  bool any = false;
  for (Qualifier &q : quals) {
    q = q.form();
    any |= !q.isUnique();
  }
  if (!any)
    quals.clear();
  return quals;
}

} // namespace

TupleType *ASTContext::getTupleType(std::vector<Type *> elemTys,
                                    std::vector<Qualifier> elemQuals) {
  elemQuals = normalQuals(std::move(elemQuals));
  if (!elemQuals.empty())
    elemQuals.resize(elemTys.size());
  auto key = std::make_pair(elemTys, qualifierCodes(elemQuals));
  auto it = TupleTypes.find(key);
  if (it != TupleTypes.end())
    return it->second;
  auto *tt = make<TupleType>(SourceLocation(), elemTys, std::move(elemQuals));
  TupleTypes[std::move(key)] = tt;
  return tt;
}

FunctionType *ASTContext::getFunctionType(std::vector<Type *> paramTys,
                                          std::vector<ParamMode> modes,
                                          Type *retTy,
                                          std::vector<Qualifier> paramQuals,
                                          Qualifier resultQual) {
  // A part's qualifier is the type's only when a caller sees it (#368): the
  // shared forms of a by-value parameter and of the result.  `mut` is the
  // function's own business (a unique value gains it by `move`, and `mut T`
  // flows into `T`), and a borrow takes the object whatever its form.
  paramQuals.resize(paramTys.size());
  for (size_t i = 0; i < paramQuals.size(); ++i)
    if (paramQuals[i].Form == QualForm::Unique ||
        (i < modes.size() && modes[i] != ParamMode::Value))
      paramQuals[i] = {};
  if (resultQual.Form == QualForm::Unique)
    resultQual = {};
  paramQuals = normalQuals(std::move(paramQuals));
  if (!paramQuals.empty())
    paramQuals.resize(paramTys.size());
  auto key = std::make_tuple(paramTys, modes, retTy, qualifierCodes(paramQuals),
                             resultQual.code());
  auto it = FunctionTypes.find(key);
  if (it != FunctionTypes.end())
    return it->second;
  auto *ft = make<FunctionType>(SourceLocation(), std::move(paramTys),
                                std::move(modes), retTy, std::move(paramQuals),
                                resultQual.form());
  FunctionTypes[std::move(key)] = ft;
  return ft;
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
ASTContext::ClassTypeBuilder::field(const std::string &name, Type *ty,
                                    bool weak, Qualifier qual) {
  Ty->addField(name, ty, weak, qual);
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
  return unsigned(k) < BuiltinType::kNumKinds ? BuiltinTys[k] : VoidTy;
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
  BuiltinType::Kind kind;
  if (builtinKindOfName(name, kind))
    return BuiltinTys[kind];
  if (auto *et = lookupEnumType(name))
    return et;
  if (auto *st = lookupStructType(name))
    return st;
  return lookupClassType(name);
}

std::string typeName(Type *ty) {
  if (!ty)
    return "unknown";
  if (auto *bt = dyn_cast<BuiltinType>(ty))
    return builtinKindName(bt->getTypeKind());
  if (auto *ct = dyn_cast<ClassType>(ty))
    return ct->getName();
  if (auto *et = dyn_cast<EnumType>(ty))
    return et->getName();
  // Before TupleType, which a StructType is: a struct goes by its name.
  if (auto *st = dyn_cast<StructType>(ty))
    return st->getName();
  // A function type takes a suffix only in parentheses: `((int) -> int)?`.
  auto suffixed = [](Type *inner) {
    std::string s = typeName(inner);
    return isa<FunctionType>(inner) ? "(" + s + ")" : s;
  };
  // A part's qualifier (#368) is written before it: `(int, shared Node)`.
  auto qualified = [](const Qualifier &q, Type *part) {
    std::string s = q.spelling();
    return (s.empty() ? s : s + " ") + typeName(part);
  };
  if (auto *fnt = dyn_cast<FunctionType>(ty)) {
    // "(int, view Str) -> bool", as the parser accepts it, so the serialised
    // form round-trips through module export.
    std::string s = "(";
    for (size_t i = 0; i < fnt->getNumParams(); ++i) {
      if (i)
        s += ", ";
      if (fnt->getMode(i) != ParamMode::Value) {
        s += paramModeName(fnt->getMode(i));
        s += " ";
      }
      s += qualified(fnt->getParamQual(i), fnt->getParamType(i));
    }
    return s + ") -> " + qualified(fnt->getResultQual(), fnt->getReturnType());
  }
  if (auto *at = dyn_cast<ArrayType>(ty))
    return suffixed(at->getElementType()) + "[]";
  if (auto *ft = dyn_cast<FixedArrayType>(ty)) {
    // An array of arrays is written with its first index's length first, as
    // it is parsed: two `int[3]`s are `int[2][3]`.  The elements' qualifier
    // (#368) binds to the primary type, so it goes first: `shared Node[4]`.
    std::string lengths;
    Type *elem = ft;
    Qualifier elemQual;
    for (auto *f = ft; f; f = dyn_cast<FixedArrayType>(f->getElementType())) {
      lengths += "[" +
                 (f->isInferred() ? std::string("_")
                                  : std::to_string(f->getLength())) +
                 "]";
      elem = f->getElementType();
      elemQual = f->getElemQual();
    }
    std::string q = elemQual.spelling();
    return (q.empty() ? q : q + " ") + suffixed(elem) + lengths;
  }
  if (auto *ot = dyn_cast<OptionalType>(ty))
    return suffixed(ot->getInnerType()) + "?";
  if (auto *at = dyn_cast<AtomicType>(ty))
    return "atomic " + typeName(at->getInnerType());
  if (auto *tt = dyn_cast<TaskType>(ty))
    return "Task<" + typeName(tt->getInnerType()) + ">";
  if (auto *qt = dyn_cast<QualifiedType>(ty)) {
    std::string s = qt->getQualifier().spelling();
    if (qt->getInnerType())
      s += std::string(s.empty() ? "" : " ") + typeName(qt->getInnerType());
    return s;
  }
  // A slice is spelled as its parameter's type is, its mode aside.
  if (auto *st = dyn_cast<SliceType>(ty)) {
    std::string q = st->getElemQual().spelling();
    return (q.empty() ? q : q + " ") + suffixed(st->getElementType()) + "[]";
  }
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
      s += qualified(tt->getElemQual(i), tt->getElementType(i));
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
