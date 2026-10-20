// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Arena-style AST node pool and canonical type cache

#pragma once

#include "AST.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
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
  // Every builtin type by kind: the five above and the sized numeric types.
  BuiltinType *BuiltinTys[BuiltinType::kNumKinds];

  // The internal poison type of a binder whose declaration failed (#89); not
  // a source type (see ast::PoisonType).
  PoisonType *PoisonTy;

  // Canonical class types.
  ClassType *ObjTy;
  ClassType *StrTy;
  // Canonical file type (ClassType inheriting Obj).
  // Canonical error type (ClassType inheriting Obj, returned by open() on
  // failure).

  // Registry of all class types, keyed by name.
  std::unordered_map<std::string, ClassType *> ClassTypes;

  // Registry of all enum types, keyed by name.  Enum types are nominal value
  // types backed by i64; they are NOT class types and live in their own map.
  std::unordered_map<std::string, EnumType *> EnumTypes;

  // Registry of all struct types, keyed by name (and by any qualified
  // import alias, see addStructTypeAlias).  A struct is a value type held in
  // place; it is not a class type and lives in its own map.
  std::unordered_map<std::string, StructType *> StructTypes;

  // Canonical FixedArrayType per (element type, length), lazily created by
  // getFixedArrayType.  Keys are canonical within this ASTContext, so
  // identity holds at any nesting depth.
  std::map<std::pair<Type *, int64_t>, FixedArrayType *> FixedArrayTypes;

  // Canonical SliceType per element type.
  std::unordered_map<Type *, SliceType *> SliceTypes;

  // Canonical OptionalType per inner type (lazily created by getOptionalType).
  // The inner Type* must be canonical, so `Node?`
  // resolves to one node everywhere and identity is pointer identity.
  std::unordered_map<Type *, OptionalType *> OptionalTypes;

  // -- Tuples
  // Canonical TupleType per element-type list (lazily created by
  // getTupleType).  Key: the element Type* pointers, each canonical, so that
  // `(int, (Str, bool))` resolves to one node everywhere.
  std::map<std::vector<Type *>, TupleType *> TupleTypes;

  // -- Function types
  // Canonical FunctionType per (parameter types, modes, result type), lazily
  // created by getFunctionType; the parts are canonical, so a function type's
  // identity is pointer identity.
  std::map<std::tuple<std::vector<Type *>, std::vector<ParamMode>, Type *>,
           FunctionType *>
      FunctionTypes;

  // -- Bootstrap helpers (called from the constructor)
  void buildObjectType();
  void buildStringType();

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

  // -- ClassTypeBuilder
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

    /// Obj's operators (== !=) and vtable slots (destroy, toString, equals:
    /// slots 0-2, Runtime.h), which every builtin class starts with.
    ClassTypeBuilder &objectSlots();

    /// Declare a virtual method and add it to the class (updates the vtable).
    ClassTypeBuilder &method(const std::string &name, Type *retTy,
                             std::vector<Type *> params = {},
                             uint8_t flags = MethodDecl::None,
                             ParamModes modes = {});

    /// Add an instance field.
    ClassTypeBuilder &field(const std::string &name, Type *ty,
                            bool weak = false);

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

  /// The canonical poison type (see ast::PoisonType).  Sema-internal: it has
  /// no name, so lookupType never returns it.
  PoisonType *getPoisonTy() const { return PoisonTy; }

  // Canonical class type accessors.
  ClassType *getObjTy() const { return ObjTy; }
  ClassType *getStrTy() const { return StrTy; }

  /// Return the canonical FixedArrayType of @p length elements of type
  /// @p elemTy (`int[3]`), creating it on first use.  @p elemTy must itself
  /// be canonical (a builtin singleton, a registered ClassType/EnumType, or
  /// another canonical type), so that type identity is pointer identity.
  FixedArrayType *getFixedArrayType(Type *elemTy, int64_t length);
  /// The canonical slice of @p elemTy (see ast::SliceType); Sema-internal.
  SliceType *getSliceType(Type *elemTy);

  /// Return the canonical OptionalType wrapping @p innerTy, creating it on
  /// first use.  @p innerTy must itself be canonical (see getFixedArrayType).
  /// Sema resolves every parser-emitted (source-located) OptionalType to this
  /// node.
  OptionalType *getOptionalType(Type *innerTy);

  /// Return the canonical TupleType with the given element types, creating
  /// it on first use.  Each element must itself be canonical (see
  /// getFixedArrayType) so that tuple type identity is pointer identity.
  TupleType *getTupleType(std::vector<Type *> elemTys);

  /// Return the canonical FunctionType of the given parameter types and
  /// modes and result type (each canonical), creating it on first use.
  FunctionType *getFunctionType(std::vector<Type *> paramTys,
                                std::vector<ParamMode> modes, Type *retTy);

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

  /// Create and register a new (empty) StructType.  Fields are added to the
  /// returned type via addField().  Returns nullptr if the name is taken.
  StructType *registerStructType(const std::string &name, SourceLocation loc);

  /// Look up a registered struct type by name.  Returns nullptr if not found.
  StructType *lookupStructType(const std::string &name) const;

  /// Register an additional name that resolves to an already-registered
  /// StructType (a qualified alias like "module::Point").  Has no effect if
  /// the alias is already present.
  void addStructTypeAlias(const std::string &alias, StructType *st);

  /// Read-only view of the struct type registry.
  const std::unordered_map<std::string, StructType *> &getStructTypes() const {
    return StructTypes;
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
  /// then the enum, struct and class type registries.  Returns nullptr if
  /// not found.
  Type *lookupType(const std::string &name) const;

  /// Read-only view of the class type registry (used by Sema to export types
  /// across module boundaries).
  const std::unordered_map<std::string, ClassType *> &getClassTypes() const {
    return ClassTypes;
  }
};

/// Canonical display name for a type: builtin keyword ("int", "float", …),
/// class, struct or enum name, element name plus "[]" for arrays, inner name
/// plus "?" for optionals, and "unknown" for null or unrecognised types.  The
/// single source of truth shared by Sema diagnostics, module-export
/// serialisation (SemaImport, which parses the "[]" / "?" suffixes back), and
/// specialized array-type naming.
std::string typeName(Type *ty);

} // namespace ast
} // namespace paykan
