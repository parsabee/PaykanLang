// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Arena-style AST node pool and canonical type cache

#pragma once

#include "AST.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace paykan {
namespace ast {

// Arena-style memory pool that owns every AST node.
// All nodes are destroyed when the ASTContext goes out of scope.
class ASTContext {
  std::vector<std::unique_ptr<ASTNode>> Pool;

  // Stable string intern pool.  std::set guarantees that references to stored
  // elements are never invalidated by subsequent insertions.
  std::set<std::string> StringPool;

  // Canonical builtin types -- created once in the constructor.
  BuiltinType *IntTy;
  BuiltinType *FloatTy;
  BuiltinType *BoolTy;
  BuiltinType *CharTy;
  BuiltinType *VoidTy;

  // Canonical class types.
  ClassType *ObjTy;
  ClassType *StrTy;
  // Canonical array type (ClassType with vtable matching PaykanArrayVTable).
  ClassType *ArrayTy;
  // Canonical file type (ClassType inheriting Obj).
  ClassType *FileTy;
  // Canonical error type (ClassType inheriting Obj, returned by open() on
  // failure).
  ClassType *ErrorTy;
  // Boxed primitive types (ClassType inheriting Obj, returned by
  // IntStr/FloatStr).
  ClassType *IntBoxTy;
  ClassType *FloatBoxTy;
  ClassType *BoolBoxTy;

  // Registry of all class types, keyed by name.
  std::unordered_map<std::string, ClassType *> ClassTypes;

  // Registry of all enum types, keyed by name.  Enum types are nominal value
  // types backed by i64; they are NOT class types and live in their own map.
  std::unordered_map<std::string, EnumType *> EnumTypes;

  // Per-element specialized array ClassTypes (lazily created).
  // Key: element Type* pointer (canonical within this ASTContext).
  std::unordered_map<Type *, ClassType *> SpecializedArrayTypes;
  // Reverse map for O(1) getSpecializedArrayElemType lookups.
  std::unordered_map<ClassType *, Type *> SpecializedArrayElemTypes;

  // -- Bootstrap helpers (called from the constructor) ----------------------
  void buildObjectType();
  void buildStringType();
  void buildArrayType();
  void buildFileType();
  void buildErrorType();
  void buildBoxedIntType();
  void buildBoxedFloatType();
  void buildBoxedBoolType();

public:
  ASTContext();
  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;
  ASTContext(ASTContext &&) = delete;
  ASTContext &operator=(ASTContext &&) = delete;

  /// Intern a string and return a stable reference into the pool.
  /// The reference is valid for the lifetime of this ASTContext.
  const std::string &intern(const std::string &s) {
    return *StringPool.insert(s).first;
  }

  /// Create an AST node of type T, store it in the pool, return a raw pointer.
  template <ASTNodeType T, typename... Args> T *make(Args &&...args) {
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
  BuiltinType *getIntTy() const { return IntTy; }
  BuiltinType *getFloatTy() const { return FloatTy; }
  BuiltinType *getBoolTy() const { return BoolTy; }
  BuiltinType *getCharTy() const { return CharTy; }
  BuiltinType *getVoidTy() const { return VoidTy; }

  // Canonical class type accessors.
  ClassType *getObjTy() const { return ObjTy; }
  ClassType *getStrTy() const { return StrTy; }
  ClassType *getArrayTy() const { return ArrayTy; }
  ClassType *getFileTy() const { return FileTy; }
  ClassType *getErrorTy() const { return ErrorTy; }
  ClassType *getIntBoxTy() const { return IntBoxTy; }
  ClassType *getFloatBoxTy() const { return FloatBoxTy; }
  ClassType *getBoolBoxTy() const { return BoolBoxTy; }

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

  /// Create and register a new (empty) EnumType.  Variants are added to the
  /// returned type via addVariant().  Returns nullptr if the name is taken.
  EnumType *registerEnumType(const std::string &name, SourceLocation loc);

  /// Look up a registered enum type by name.  Returns nullptr if not found.
  EnumType *lookupEnumType(const std::string &name) const;

  /// Read-only view of the enum type registry.
  const std::unordered_map<std::string, EnumType *> &getEnumTypes() const {
    return EnumTypes;
  }

  /// Register an additional name that resolves to an already-registered
  /// ClassType (e.g. a qualified alias like "module::Foo" -> Foo's ClassType).
  /// Has no effect if the alias is already present.
  void addClassTypeAlias(const std::string &alias, ClassType *ct);

  /// Register an additional name that resolves to an already-registered
  /// EnumType (e.g. a qualified alias like "module::Color" -> Color's
  /// EnumType). Has no effect if the alias is already present.
  void addEnumTypeAlias(const std::string &alias, EnumType *et);

  /// Look up any type by name: builtins (int, float, bool, void) first,
  /// then the class type registry.  Returns nullptr if not found.
  Type *lookupType(const std::string &name) const;

  /// Read-only view of the class type registry (used by Sema to export types
  /// across module boundaries).
  const std::unordered_map<std::string, ClassType *> &getClassTypes() const {
    return ClassTypes;
  }
};

/// Canonical display name for a type: builtin keyword ("int", "float", …),
/// class or enum name, element name plus "[]" for arrays, and "unknown" for
/// null or unrecognised types.  The single source of truth shared by Sema
/// diagnostics, module-export serialisation (SemaImport), and specialized
/// array-type naming.
std::string typeName(Type *ty);

} // namespace ast
} // namespace paykan
