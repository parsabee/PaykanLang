// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Arena-style AST node pool and canonical type cache

#pragma once

#include "AST.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace paykan {
namespace ast {

// Arena-style memory pool that owns every AST node.
// All nodes are destroyed when the ASTContext goes out of scope.
class ASTContext {
  std::vector<std::unique_ptr<ASTNode>> Pool;

  // Canonical builtin types -- created once in the constructor.
  BuiltinType *IntTy;
  BuiltinType *FloatTy;
  BuiltinType *BoolTy;
  BuiltinType *VoidTy;

  // Canonical class types.
  ClassType *ObjTy;
  ClassType *StrTy;
  // Canonical array type (ClassType with vtable matching PaykanArrayVTable).
  ClassType *ArrayTy;
  // Canonical file type (ClassType inheriting Obj).
  ClassType *FileTy;
  // Canonical error type (ClassType inheriting Obj, returned by open() on failure).
  ClassType *ErrorTy;

  // Registry of all class types, keyed by name.
  std::unordered_map<std::string, ClassType *> ClassTypes;

  // Per-element specialized array ClassTypes (lazily created).
  // Key: element Type* pointer (canonical within this ASTContext).
  std::unordered_map<Type *, ClassType *> SpecializedArrayTypes;

  // -- Bootstrap helpers (called from the constructor) ----------------------
  void buildObjectType();
  void buildStringType();
  void buildArrayType();
  void buildFileType();
  void buildErrorType();

public:
  ASTContext();

  /// Create an AST node of type T, store it in the pool, return a raw pointer.
  template <ASTNodeType T, typename... Args>
  T *make(Args &&...args) {
    auto node = std::make_unique<T>(std::forward<Args>(args)...);
    T *ptr = node.get();
    Pool.push_back(std::move(node));
    return ptr;
  }

  // -- ClassTypeBuilder ------------------------------------------------------
  //
  // Fluent API for constructing ClassType nodes with methods, fields, and
  // operators in a declarative style.  Usage:
  //
  //   auto *ty = ctx.buildClassType("Foo", parentTy)
  //                  .addOp(BinaryOpcode::Eq)
  //                  .method("bar", retTy, {paramTy})
  //                  .field("x", intTy)
  //                  .build();
  //
  class ClassTypeBuilder {
    ASTContext &Ctx;
    ClassType *Ty;

  public:
    ClassTypeBuilder(ASTContext &ctx, ClassType *ty);

    /// Add a supported binary operator.
    ClassTypeBuilder &addOp(BinaryOpcode op);

    /// Add a supported unary operator.
    ClassTypeBuilder &addOp(UnaryOpcode op);

    /// Declare a virtual method and add it to the class (updates the vtable).
    ClassTypeBuilder &method(const std::string &name, Type *retTy,
                             std::vector<Type *> params = {},
                             uint8_t flags = MethodDecl::None);

    /// Add an instance field.
    ClassTypeBuilder &field(const std::string &name, Type *ty);

    /// Finish building, register in the context's class type registry, and
    /// return the ClassType.
    ClassType *build();
  };

  /// Start building a new ClassType owned by this context.
  ClassTypeBuilder buildClassType(const std::string &name,
                                  ClassType *superClass = nullptr);

  // Canonical builtin type accessors.
  BuiltinType *getIntTy()   const { return IntTy; }
  BuiltinType *getFloatTy() const { return FloatTy; }
  BuiltinType *getBoolTy()  const { return BoolTy; }
  BuiltinType *getVoidTy()  const { return VoidTy; }

  // Canonical class type accessors.
  ClassType *getObjTy()   const { return ObjTy; }
  ClassType *getStrTy()   const { return StrTy; }
  ClassType *getArrayTy() const { return ArrayTy; }
  ClassType *getFileTy()  const { return FileTy; }
  ClassType *getErrorTy() const { return ErrorTy; }

  /// Return (creating if needed) the specialized ClassType for arrays whose
  /// elements have type @p elemTy.  The returned type is a subtype of ArrayTy
  /// and carries push(elemTy)->void and pop()->elemTy method declarations.
  ClassType *getOrCreateSpecializedArrayType(Type *elemTy);

  /// If @p ct is a specialized array ClassType (e.g. Array<Str>), return its
  /// element type; otherwise return nullptr.
  Type *getSpecializedArrayElemType(ClassType *ct) const;

  /// Return the canonical BuiltinType* for a given Kind.
  BuiltinType *getBuiltinType(BuiltinType::Kind k) const;

  /// Register a class type in the name -> type registry.
  void registerClassType(ClassType *ct);

  /// Pre-register a ClassType stub (name + superclass; no fields/methods yet).
  /// Places the type in the registry immediately so that forward field-type
  /// references within the same compilation unit resolve correctly.
  /// Fields and methods may be added via addField() / addMethod() afterwards.
  ClassType *preRegisterClassType(const std::string &name,
                                  ClassType *superClass = nullptr);

  /// Look up a registered class type by name.  Returns nullptr if not found.
  ClassType *lookupClassType(const std::string &name) const;

  /// Register an additional name that resolves to an already-registered
  /// ClassType (e.g. a qualified alias like "module::Foo" -> Foo's ClassType).
  /// Has no effect if the alias is already present.
  void addClassTypeAlias(const std::string &alias, ClassType *ct);

  /// Look up any type by name: builtins (int, float, bool, void) first,
  /// then the class type registry.  Returns nullptr if not found.
  Type *lookupType(const std::string &name) const;

  /// Read-only view of the class type registry (used by Sema to export types
  /// across module boundaries).
  const std::unordered_map<std::string, ClassType *> &getClassTypes() const {
    return ClassTypes;
  }
};

} // namespace ast
} // namespace paykan
