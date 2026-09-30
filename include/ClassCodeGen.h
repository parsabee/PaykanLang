// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// ClassCodeGen — LLVM IR code generation for user-defined Paykan classes.
//
// This helper is owned by CodeGen and handles every class-specific concern:
//
//   • Computing the LLVM struct layout for a class (vtable ptr + fields).
//   • Building vtable global constants.
//   • Emitting concrete method functions (ClassName_methodName).
//   • Emitting the constructor function (ClassName(args) -> PaykanShared*).
//   • Member-field access (visitMemberAccessExpr) and assignment
//     (visitMemberAssignStmt), including retain/release for class-typed fields.
//
// Everything is implemented in src/CodeGen/ClassCodeGen.cpp.
//
// Design note: ClassCodeGen is a *non-owning* helper — it holds a mutable
// reference to its parent CodeGen and calls back into it for primitives like
// toLLVMType(), createEntryAlloca(), emitRetain(), etc.  The parent's
// CodeGen.h declares ClassCodeGen as a friend so that the helper can reach
// private members without exposing them publicly.

#pragma once

#include "AST.h"
#include "ASTContext.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include <string>
#include <vector>

namespace paykan {
namespace codegen {

class CodeGen; // forward declaration

class ClassCodeGen {
public:
  explicit ClassCodeGen(CodeGen &cg) : CG(cg) {}

  // -------------------------------------------------------------------------
  // Struct-type helpers
  // -------------------------------------------------------------------------

  /// Get or create the LLVM named struct type for a user-defined class.
  /// Layout: { ptr vtable, field0_ty, field1_ty, ... }
  /// The parent's fields come before the child's own fields (depth-first).
  llvm::StructType *getOrCreateClassStructType(ast::ClassType *ct);

  /// Return the 1-based index of a named field inside the class struct.
  /// (Index 0 is always the vtable pointer.)  Returns -1 if not found.
  int getFieldIndex(ast::ClassType *ct, const std::string &name) const;

  // -------------------------------------------------------------------------
  // Type helpers
  // -------------------------------------------------------------------------

  /// Derive the ClassType of an expression's resolved value.
  /// Returns nullptr if the expression's type is not a class.
  ast::ClassType *getExprClassType(ast::Expr *expr) const;

  // -------------------------------------------------------------------------
  // Method name helpers
  // -------------------------------------------------------------------------

  /// Walk the inheritance chain and return the mangled IR function name of
  /// the concrete implementation of `methodName` for class `ct`.
  /// Returns "" if no concrete function is found (abstract / unimplemented).
  std::string findConcreteMethodFuncName(ast::ClassType *ct,
                                         const std::string &methodName);

  /// Return the IR function `ClassName_methodName` defined by class `ct`
  /// itself (no inheritance walk), under its imported qualified name if the
  /// class comes from another module.  Returns nullptr if there is none.
  llvm::Function *lookupOwnMethodFunction(ast::ClassType *ct,
                                          const std::string &methodName);

  // -------------------------------------------------------------------------
  /// Emit an i1 runtime type-check: load the vtable pointer from rawObjPtr
  /// and compare it against the ClassName_vtable global for ct.
  /// Returns an i1 value (true = exact type match).
  /// Each vtable global has a unique address in memory — this serves as a
  /// stable, cross-module type identity without any integer ID scheme.
  llvm::Value *emitIsExactType(llvm::Value *rawObjPtr, ast::ClassType *ct);

  /// Visitor entry points (called from CodeGen)
  // -------------------------------------------------------------------------

  /// Forward-declare everything a function body may reference by name for
  /// this class — struct type, vtable global, method functions, destructor
  /// and constructor — without emitting any body.  Run for every class of the
  /// module before any body is emitted, so a method can construct its own
  /// class or a later-declared one, and `__super__` / vtable slots resolve a
  /// base class declared later in the file.
  void declareClass(ast::ClassDecl *node);

  /// Emit a complete class declaration: method bodies, vtable global,
  /// and constructor function.
  llvm::Value *visitClassDecl(ast::ClassDecl *node);

  /// Emit a member-assignment statement (receiver.field = value).
  llvm::Value *visitMemberAssignStmt(ast::MemberAssignStmt *node);

  /// Emit a member-access expression (receiver.field -> value).
  llvm::Value *visitMemberAccessExpr(ast::MemberAccessExpr *node);

  /// Emit the synthetic destructor `ClassName_destroy(ptr self)` for a class.
  /// It runs the user-defined `destroy` body (if any), releases every
  /// reference-counted field (Str/Obj/class/array), and frees the object
  /// struct.  It becomes the class's vtable `destroy` slot so that the last
  /// `Paykan_release` of an instance also releases everything the instance
  /// owns — without this, object-typed fields leak.
  void emitDestructor(ast::ClassDecl *node, ast::ClassType *ct);

  // -------------------------------------------------------------------------
  // State
  // -------------------------------------------------------------------------

  /// Maps ClassType* -> LLVM named struct type.
  llvm::DenseMap<ast::ClassType *, llvm::StructType *> ClassStructTypes;

  /// Maps ClassType* -> emitted vtable global variable.
  llvm::DenseMap<ast::ClassType *, llvm::GlobalVariable *> ClassVTableGlobals;

  /// Maps imported ClassType* -> the LLVM name qualifier used for its methods
  /// (e.g. ClassType for 'Adder' imported as 'helper' -> "helper").
  /// Populated by CodeGen::processImports; used by findConcreteMethodFuncName.
  llvm::DenseMap<ast::ClassType *, std::string> ImportedClassQualifiers;

  /// ClassType of the method currently being emitted (nullptr otherwise).
  ast::ClassType *CurrentMethodClassType = nullptr;

private:
  /// LLVM signature of a method function: `ret (ptr self, params...)`.
  llvm::FunctionType *getMethodFunctionType(ast::MethodDecl *md);

  /// LLVM signature of a class constructor: `ptr (initParams...)`.
  llvm::FunctionType *getConstructorFunctionType(ast::ClassType *ct);

  /// Return the function @p name declared by declareClass, declaring it now
  /// if it does not exist yet.
  llvm::Function *getOrDeclareFunction(const std::string &name,
                                       llvm::FunctionType *fnTy);

  CodeGen &CG;
};

} // namespace codegen
} // namespace paykan
