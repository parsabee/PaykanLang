// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Deep-copy of declarations with type-parameter substitution (generics).

#pragma once

#include "AST.h"
#include "ASTContext.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace paykan {
namespace ast {

// Clones a generic declaration into a concrete one.
//
// Monomorphisation works on the AST: a class or function template is never
// type-checked itself; each instantiation is a fresh deep copy of the
// declaration in which every type annotation naming a type parameter is
// replaced by the corresponding type argument.  The copy is then registered
// and checked by Sema exactly like a hand-written declaration, and lowered
// without any generics-specific code path.
//
// Substitution rules for a type annotation:
//   * ClassType stub whose name is a type parameter  -> the argument type
//   * ArrayType, FixedArrayType                      -> element substituted
//   * GenericType (Box<T>)                           -> arguments substituted
//   * OptionalType (T?)                              -> inner type substituted
//   * TupleType ((T, int))                           -> elements substituted
//   * StructType (a resolved struct)                 -> a stub by its name
//   * anything else (builtin, enum, other class)     -> shared, not copied
// Source locations are preserved on every cloned node, so a diagnostic inside
// an instantiation points at the template's source text.
//
// Expressions and statements are copied node by node; every node of the copy
// is fresh, so Sema's per-node annotations (resolved types, rewritten callee
// names, canonical match-arm types) never leak from one instantiation into
// another or into the template.
//
// A template of another module (docs/design/pkm.md §4) is copied twice.  Its
// module copies it once with a Renamer, which spells every name the module
// declares or imports as the hidden qualified name an importer registers
// (Sema::exportTemplates).  The importer copies that from the other
// module's ASTContext into its own (`Foreign`): every name is interned anew
// and every type is rebuilt, a class or an enum as a stub by its name, so
// nothing of the other context leaks into the copy.
class ASTCloner {
public:
  /// How a template's module spells the names of its own and its imports'
  /// declarations for an importer; a name returned unchanged stays as it is.
  struct Renamer {
    virtual ~Renamer() = default;
    /// A class or an enum, in a type annotation or an `Enum::Variant`.
    virtual std::string type(const std::string &name) const = 0;
    /// A generic class, in `Box<T>`.
    virtual std::string generic(const std::string &name) const = 0;
    /// The callee of a call: a function, a constructor or a template.
    virtual std::string callee(const std::string &name) const = 0;
  };

private:
  ASTContext &Ctx;
  std::unordered_map<std::string, Type *> Subst;
  const Renamer *Names;
  bool Foreign;

  /// @p s interned in this context (a no-op for a name already there).
  const std::string &in(const std::string &s) { return Ctx.intern(s); }
  /// A class or enum annotation named @p name: shared, or a stub by its
  /// (renamed) name.
  Type *named(Type *ty, const std::string &name);

public:
  ASTCloner(ASTContext &ctx, std::unordered_map<std::string, Type *> subst,
            const Renamer *names = nullptr, bool foreign = false)
      : Ctx(ctx), Subst(std::move(subst)), Names(names), Foreign(foreign) {}

  /// Substitute type parameters inside @p ty.  Returns @p ty itself when
  /// nothing inside it changes.
  Type *cloneType(Type *ty);

  Expr *cloneExpr(Expr *e);
  Stmt *cloneStmt(Stmt *s);
  CompoundStmt *cloneCompound(CompoundStmt *cs);

  /// Copy a function (or method) under a new name.  The copy has no type
  /// parameters.
  FuncDecl *cloneFuncDecl(FuncDecl *fn, const std::string &newName);

  /// Copy a class under a new name (fields, methods, superclass name).  The
  /// copy has no type parameters.
  ClassDecl *cloneClassDecl(ClassDecl *cd, const std::string &newName);

  /// Copy a struct under a new name (fields, and the `impl` methods Sema
  /// added).  The copy has no type parameters.
  StructDecl *cloneStructDecl(StructDecl *sd, const std::string &newName);

  /// Copy an `impl` block for the type named @p newTypeName.  The copy has
  /// no type parameters.
  ImplDecl *cloneImplDecl(ImplDecl *im, const std::string &newTypeName);

  /// Copy an enum under a new name (its variants and their fields).  The
  /// copy has no type parameters.
  EnumDecl *cloneEnumDecl(EnumDecl *ed, const std::string &newName);

  /// Copy a template as a template: same name, same type parameters.
  FuncDecl *cloneTemplate(FuncDecl *fn);
  ClassDecl *cloneTemplate(ClassDecl *cd);
  StructDecl *cloneTemplate(StructDecl *sd);
  ImplDecl *cloneTemplate(ImplDecl *im);
  EnumDecl *cloneTemplate(EnumDecl *ed);
  /// The type parameters' bounds of a template, cloned.
  TypeParamBounds cloneBounds(const TypeParamBounds &bounds);
};

} // namespace ast
} // namespace paykan
