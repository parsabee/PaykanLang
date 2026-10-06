// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
//   * ArrayType                                      -> element substituted
//   * GenericType (Box<T>)                           -> arguments substituted
//   * OptionalType (T?)                              -> inner type substituted
//   * TupleType ((T, int))                           -> elements substituted
//   * anything else (builtin, enum, other class)     -> shared, not copied
// Source locations are preserved on every cloned node, so a diagnostic inside
// an instantiation points at the template's source text.
//
// Expressions and statements are copied node by node; every node of the copy
// is fresh, so Sema's per-node annotations (resolved types, rewritten callee
// names, canonical match-arm types) never leak from one instantiation into
// another or into the template.
class ASTCloner {
  ASTContext &Ctx;
  std::unordered_map<std::string, Type *> Subst;

public:
  ASTCloner(ASTContext &ctx, std::unordered_map<std::string, Type *> subst)
      : Ctx(ctx), Subst(std::move(subst)) {}

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
};

} // namespace ast
} // namespace paykan
