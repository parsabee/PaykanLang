// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: class declarations and method bodies.

#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

#include <llvm/ADT/SmallBitVector.h>
#include <llvm/ADT/StringMap.h>

#include <functional>

namespace paykan {
namespace sema {

// ---------------------------------------------------------------------------
// Phase helpers — class declaration checking
// ---------------------------------------------------------------------------

namespace {

/// Returns true if `stmt` is a bare call to __super__(...) in statement
/// position (i.e. `__super__(args);`).
bool isSuperInitCall(ast::Stmt *stmt) {
  auto *es = ast::dyn_cast<ast::ExprStmt>(stmt);
  if (!es)
    return false;
  auto *call = ast::dyn_cast<ast::CallExpr>(es->getExpr());
  return call && call->getCalleeName() == names::kMethodSuper;
}

} // namespace

std::string Sema::signatureString(const ast::ClassType *owner,
                                  const std::string &methodName,
                                  ast::Type *retTy,
                                  const std::vector<ast::Type *> &paramTys) {
  std::string s = owner->getName() + "." + methodName + "(";
  for (size_t i = 0; i < paramTys.size(); ++i) {
    if (i)
      s += ", ";
    s += typeName(paramTys[i]);
  }
  s += ") -> " + typeName(retTy);
  return s;
}

// ---------------------------------------------------------------------------
// Definite-assignment analysis for __init__ (Check 1)
// ---------------------------------------------------------------------------

bool Sema::checkInitFieldsAssigned(ast::ClassType *ct, ast::CompoundStmt *body,
                                   ast::SourceLocation initLoc,
                                   const std::string &className) {
  // Own fields only — base fields are initialised by the required __super__.
  const auto &fields = ct->getFields();
  if (fields.empty())
    return true;

  // Map each own field name to a bit index.
  llvm::StringMap<unsigned> fieldIndex;
  for (unsigned i = 0; i < fields.size(); ++i)
    fieldIndex[fields[i].first] = i;
  const unsigned n = static_cast<unsigned>(fields.size());
  const llvm::SmallBitVector full(n, true);

  // Returns the bit index of `self.<field>` for a member assignment, or -1 if
  // the statement does not assign one of this class's own fields.
  auto selfFieldBit = [&](ast::MemberAssignStmt *ma) -> int {
    auto *recv = ast::dyn_cast<ast::Identifier>(ma->getReceiver());
    if (!recv || recv->getName() != names::kSelf)
      return -1;
    auto it = fieldIndex.find(ma->getFieldName());
    return it == fieldIndex.end() ? -1 : static_cast<int>(it->second);
  };

  // The set of fields left unassigned on a terminating path; bits are reported
  // once (a class-level error per field) to avoid duplicate diagnostics.
  llvm::SmallBitVector reported(n, false);
  bool ok = true;

  auto reportMissing = [&](const llvm::SmallBitVector &assigned,
                           ast::SourceLocation loc) {
    for (unsigned i = 0; i < n; ++i) {
      if (!assigned.test(i) && !reported.test(i)) {
        reported.set(i);
        error(loc, "field '" + fields[i].first + "' of class '" + className +
                       "' is not assigned on every path through '" +
                       names::kMethodInit + "'");
        ok = false;
      }
    }
  };

  // Result of analysing a statement / block: the set of fields definitely
  // assigned along the fall-through path, and whether the path always exits via
  // return (so it never falls through).
  struct Flow {
    llvm::SmallBitVector Assigned;
    bool AlwaysReturns = false;
  };

  std::function<Flow(ast::Stmt *, const llvm::SmallBitVector &)> analyzeStmt;
  std::function<Flow(llvm::ArrayRef<ast::Stmt *>, const llvm::SmallBitVector &)>
      analyzeBlock;

  analyzeBlock = [&](llvm::ArrayRef<ast::Stmt *> stmts,
                     const llvm::SmallBitVector &in) -> Flow {
    Flow f{in, false};
    for (auto *s : stmts) {
      if (f.AlwaysReturns)
        break; // dead code after an unconditional return — ignore
      Flow sf = analyzeStmt(s, f.Assigned);
      f.Assigned = sf.Assigned;
      if (sf.AlwaysReturns) {
        f.AlwaysReturns = true;
        break;
      }
    }
    return f;
  };

  analyzeStmt = [&](ast::Stmt *s, const llvm::SmallBitVector &in) -> Flow {
    if (auto *ma = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
      llvm::SmallBitVector out = in;
      int bit = selfFieldBit(ma);
      if (bit >= 0)
        out.set(static_cast<unsigned>(bit));
      return {out, false};
    }
    if (auto *ret = ast::dyn_cast<ast::ReturnStmt>(s)) {
      // Every field must be assigned before any return.
      reportMissing(in, ret->getLocation());
      return {in, true};
    }
    if (auto *cs = ast::dyn_cast<ast::CompoundStmt>(s))
      return analyzeBlock(cs->getStatements(), in);
    if (auto *ifs = ast::dyn_cast<ast::IfStmt>(s)) {
      Flow thenF = analyzeStmt(ifs->getThenBranch(), in);
      if (!ifs->getElseBranch()) {
        // No else: the condition may be false, so nothing the then-branch
        // assigns is guaranteed.  If the then-branch always returns, the only
        // surviving path is the (false) fall-through with the incoming set.
        return {in, false};
      }
      Flow elseF = analyzeStmt(ifs->getElseBranch(), in);
      // Fall-through reaches here from branches that do not always return.
      // A field is definitely assigned afterwards iff it is assigned on every
      // such surviving branch.
      if (thenF.AlwaysReturns && elseF.AlwaysReturns)
        return {full, true};
      if (thenF.AlwaysReturns)
        return {elseF.Assigned, false};
      if (elseF.AlwaysReturns)
        return {thenF.Assigned, false};
      llvm::SmallBitVector out = thenF.Assigned;
      out &= elseF.Assigned;
      return {out, false};
    }
    if (auto *ws = ast::dyn_cast<ast::WhileStmt>(s)) {
      // The body may execute zero times, so no assignment inside it is
      // guaranteed.  Still analyse it so returns within are validated.
      analyzeStmt(ws->getBody(), in);
      return {in, false};
    }
    if (auto *ms = ast::dyn_cast<ast::MatchStmt>(s)) {
      // Analyse every arm (so returns within are validated), then apply
      // exhaustiveness: only an exhaustive match (wildcard arm, every enum
      // variant, both bool literals, or None + the wrapped type for an
      // optional subject — see detail::matchIsExhaustive) can guarantee
      // assignments.
      llvm::SmallBitVector out = full;
      bool anyFallThrough = false;
      for (ast::MatchArm *arm : ms->getArms()) {
        Flow af = analyzeBlock(arm->getBody()->getStatements(), in);
        if (!af.AlwaysReturns) {
          anyFallThrough = true;
          out &= af.Assigned;
        }
      }
      if (!detail::matchIsExhaustive(ms))
        return {in, false}; // a non-matching path keeps only the incoming set
      if (!anyFallThrough)
        return {full, true}; // every arm returns
      return {out, false};
    }
    // Any other statement neither assigns a field nor returns.
    return {in, false};
  };

  // An optional field (`next: Node?`) is implicitly `None` unless __init__
  // assigns it: the constructor zero-initialises every slot and a NULL box IS
  // None, so such fields start out definitely assigned.  This is what makes
  // linked structures ergonomic (prototype decision, see
  // proposals/optionals.md).
  llvm::SmallBitVector entry(n, false);
  for (unsigned i = 0; i < n; ++i)
    if (ast::isa<ast::OptionalType>(fields[i].second))
      entry.set(i);
  Flow result = analyzeBlock(body->getStatements(), entry);
  if (!result.AlwaysReturns)
    reportMissing(result.Assigned, initLoc);
  return ok;
}

bool Sema::checkClassDecls(const std::vector<ast::ClassDecl *> &classDecls) {
  bool ok = true;

  // -------------------------------------------------------------------------
  // Phase 1: Check for duplicate class names (local, and against builtins,
  //          imports, and enums).  The class name doubles as its constructor
  //          function (phase 4b), so it must be free in the function table
  //          too — otherwise `class print {...}` would silently replace the
  //          builtin `print`.
  // -------------------------------------------------------------------------
  llvm::StringMap<ast::ClassDecl *> localClasses;
  for (auto *cd : classDecls) {
    if (!localClasses.try_emplace(cd->getName(), cd).second) {
      error(cd->getLocation(), "redefinition of class '" + cd->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(cd->getName(), cd->getLocation(),
                                DeclKind::Class))
      ok = false;
  }
  if (!ok)
    return false;

  // -------------------------------------------------------------------------
  // Phase 2: Topological sort (superclass before subclass) + cycle detection.
  // -------------------------------------------------------------------------
  std::vector<ast::ClassDecl *> sorted;
  llvm::StringMap<uint8_t> color; // 0=white 1=gray(in-progress) 2=black(done)
  bool aborted = false;

  std::function<void(ast::ClassDecl *)> topoVisit = [&](ast::ClassDecl *cd) {
    if (aborted)
      return;
    uint8_t &c = color[cd->getName()];
    if (c == 2)
      return; // already processed
    if (c == 1) {
      error(cd->getLocation(),
            "cyclic inheritance involving class '" + cd->getName() + "'");
      aborted = true;
      return;
    }
    c = 1; // gray — in progress
    if (cd->hasSuperClass()) {
      const auto &superName = cd->getSuperClassName();
      auto it = localClasses.find(superName);
      if (it != localClasses.end()) {
        topoVisit(it->second); // process superclass first
      } else {
        // Not declared in this module: the name must resolve through the
        // ASTContext registry — a built-in class type (Obj, Str) or an
        // imported class referenced by a registered alias (e.g.
        // "tmp_import::helper::Adder").
        if (!Ctx.lookupClassType(superName)) {
          error(cd->getLocation(), "superclass '" + superName + "' of class '" +
                                       cd->getName() + "' is not defined");
          aborted = true;
          return;
        }
      }
    }
    c = 2; // black — done
    sorted.push_back(cd);
  };

  for (auto *cd : classDecls)
    topoVisit(cd);

  if (aborted)
    return false;

  // -------------------------------------------------------------------------
  // Phase 3: Pre-register ClassType stubs so forward field-type references
  //          within this module resolve correctly.
  // -------------------------------------------------------------------------
  for (auto *cd : sorted) {
    ast::ClassType *superClass = nullptr;
    if (cd->hasSuperClass()) {
      superClass = Ctx.lookupClassType(cd->getSuperClassName());
      // Non-inheritable (final) classes cannot be subclassed.
      // Currently this covers built-in types like Str; it will also apply
      // to user-defined `final` classes once that keyword is added.
      if (superClass && superClass->isFinal()) {
        error(cd->getLocation(), "cannot inherit from '" +
                                     cd->getSuperClassName() +
                                     "': class is final");
        ok = false;
        superClass = nullptr; // fall back to Obj so analysis can continue
      }
    }
    if (!superClass)
      superClass = Ctx.getObjTy();
    Ctx.preRegisterClassType(cd->getName(), superClass);
  }

  // -------------------------------------------------------------------------
  // Phase 4: Populate field types and method signatures for each class.
  // -------------------------------------------------------------------------
  for (auto *cd : sorted) {
    auto *ct = Ctx.lookupClassType(cd->getName());
    assert(ct && "ClassType stub must exist after pre-registration");

    // Re-inherit the parent vtable now that it is fully populated.
    if (auto *super = ct->getSuperClass())
      ct->reinheritVTable(super);

    // -- fields --
    llvm::StringSet<> fieldNames;
    for (auto *field : cd->getFields()) {
      if (!fieldNames.insert(field->getName()).second) {
        error(field->getLocation(), "duplicate field '" + field->getName() +
                                        "' in class '" + cd->getName() + "'");
        ok = false;
        continue;
      }
      // Fields must not shadow superclass fields.
      for (auto *super = ct->getSuperClass(); super;
           super = super->getSuperClass()) {
        for (auto &[fn, _] : super->getFields()) {
          if (fn == field->getName()) {
            error(field->getLocation(),
                  "field '" + field->getName() + "' in class '" +
                      cd->getName() + "' shadows a field in superclass '" +
                      super->getName() + "'");
            ok = false;
          }
        }
      }
      auto *fty = resolveType(field->getType(), field->getLocation(),
                              "field '" + field->getName() + "' in class '" +
                                  cd->getName() + "'");
      if (!fty) {
        ok = false;
        continue;
      }
      ct->addField(field->getName(), fty);
    }

    // -- methods --
    llvm::StringSet<> methodNames;
    for (auto *method : cd->getMethods()) {
      if (!methodNames.insert(method->getName()).second) {
        error(method->getLocation(), "duplicate method '" + method->getName() +
                                         "' in class '" + cd->getName() + "'");
        ok = false;
        continue;
      }
      // Resolve return type.
      ast::Type *retTy = Ctx.getVoidTy();
      if (method->getReturnType()) {
        retTy = resolveType(method->getReturnType(), method->getLocation(),
                            "method '" + method->getName() + "' in '" +
                                cd->getName() + "'");
        if (!retTy) {
          ok = false;
          continue;
        }
      }
      // Resolve parameter types.
      std::vector<ast::Type *> paramTys;
      bool paramsOk = true;
      for (auto &p : method->getParams()) {
        // `self` is the implicit receiver of every method; a parameter of the
        // same name would silently shadow it in the body scope.
        if (p.getName() == names::kSelf) {
          error(method->getLocation(),
                "'" + std::string(names::kSelf) +
                    "' cannot be used as a parameter name of method '" +
                    method->getName() + "' in class '" + cd->getName() +
                    "'; it is the implicit receiver");
          paramsOk = false;
          ok = false;
          break;
        }
        auto *pty =
            resolveType(p.ParamType, method->getLocation(),
                        "parameter '" + p.getName() + "' of method '" +
                            method->getName() + "' in '" + cd->getName() + "'");
        if (!pty) {
          paramsOk = false;
          ok = false;
          break;
        }
        paramTys.push_back(pty);
      }
      if (!paramsOk)
        continue;

      // -- override signature check -----------------------------------------
      // If this method overrides a base-class method (same name found by
      // walking up the inheritance chain), the override must have an identical
      // signature: same parameter types and return type.  Strict equality is
      // enforced (no covariance).  __init__ is excluded — it is a per-class
      // constructor, not a virtual override.
      if (method->getName() != names::kMethodInit) {
        if (auto *super = ct->getSuperClass()) {
          if (auto *baseMethod = super->findMethod(method->getName())) {
            const auto &baseParams = baseMethod->getParamTypes();
            bool sigMatches = typesEqual(baseMethod->getReturnType(), retTy) &&
                              baseParams.size() == paramTys.size();
            for (size_t i = 0; sigMatches && i < paramTys.size(); ++i)
              sigMatches = typesEqual(baseParams[i], paramTys[i]);
            if (!sigMatches) {
              error(
                  method->getLocation(),
                  "override of '" +
                      signatureString(super, baseMethod->getName(),
                                      baseMethod->getReturnType(),
                                      baseMethod->getParamTypes()) +
                      "' has incompatible signature '" +
                      signatureString(ct, method->getName(), retTy, paramTys) +
                      "'");
              ok = false;
            }
          }
        }
      }

      auto *mdecl = Ctx.make<ast::MethodDecl>(
          method->getLocation(), method->getName(), retTy, std::move(paramTys));
      ct->addMethod(mdecl);
    }
  }

  if (!ok)
    return false;

  // -------------------------------------------------------------------------
  // Phase 4b: Register constructor functions so method bodies can call them.
  // -------------------------------------------------------------------------
  for (auto *cd : sorted) {
    auto *ct = Ctx.lookupClassType(cd->getName());
    auto *initDecl = ct->findMethod(names::kMethodInit);
    std::vector<ast::Type *> ctorParams;
    if (initDecl)
      ctorParams = initDecl->getParamTypes();
    // Phase 1 rejected any class whose name is already a function (builtin or
    // otherwise), so this registration never overwrites an existing entry.
    assert(!lookupFunction(cd->getName()) &&
           "constructor would overwrite a registered function");
    declareFunction(cd->getName(), ct, ctorParams);
  }

  // Method bodies (Phase 5) are checked later by checkClassBodies(), after free
  // functions are forward-declared — so a method may call any module function.
  // Remember the topological order for that pass.
  SortedClasses = std::move(sorted);
  return ok;
}

bool Sema::checkClassBodies() {
  // -------------------------------------------------------------------------
  // Phase 5: Type-check method bodies (class types, fields, method signatures,
  // constructors, and all free functions are registered by now).
  // -------------------------------------------------------------------------
  bool ok = true;
  for (auto *cd : SortedClasses)
    if (!visitClassDecl(cd))
      ok = false;
  return ok;
}

// ---------------------------------------------------------------------------
// visitMethodDecl / visitClassDecl
// ---------------------------------------------------------------------------

bool Sema::visitMethodDecl(ast::MethodDecl *) {
  // MethodDecl nodes are not produced by the parser; nothing to do.
  return true;
}

bool Sema::visitClassDecl(ast::ClassDecl *node) {
  // By the time we reach here (Phase 5), all ClassType stubs are already
  // populated with field types and method signatures.  We only type-check
  // the method bodies.
  auto *ct = Ctx.lookupClassType(node->getName());
  assert(ct && "ClassType must exist before visitClassDecl body-check");

  ClassContext classCtx;
  classCtx.ClassType = ct;
  auto *savedClassCtx = CurrentClassCtx;
  CurrentClassCtx = &classCtx;

  bool ok = true;

  // A derived class whose (non-Obj) superclass declares an __init__ must itself
  // declare an __init__ — otherwise the base initializer (which may require
  // arguments) is never invoked and base fields are left uninitialised.  The
  // declared __init__ is in turn required to call __super__ as its first
  // statement (checked per-method below).
  if (auto *super = ct->getSuperClass();
      super && super != Ctx.getObjTy() &&
      super->findMethod(names::kMethodInit)) {
    bool declaresOwnInit = false;
    for (auto *method : node->getMethods())
      if (method->getName() == names::kMethodInit) {
        declaresOwnInit = true;
        break;
      }
    if (!declaresOwnInit) {
      error(node->getLocation(),
            "class '" + node->getName() + "' must declare an '" +
                names::kMethodInit + "' that calls '" + names::kMethodSuper +
                "' because its superclass '" + super->getName() + "' has an '" +
                names::kMethodInit + "'");
      ok = false;
    }
  }

  for (auto *method : node->getMethods()) {
    classCtx.MethodName = method->getName();

    // `destroy` is the compiler-generated destructor: it is emitted for every
    // class (releasing fields and freeing the object) and is final. User
    // classes may not declare or override it.
    if (method->getName() == names::kMethodDestroy) {
      error(method->getLocation(),
            "'destroy' is the compiler-generated destructor and cannot be "
            "overridden");
      ok = false;
      continue;
    }

    // Determine whether this __init__ must call __super__.  A derived
    // __init__ must call __super__(...) as its first statement whenever its
    // superclass (other than the implicit Obj root) declares an __init__ at
    // all — even a zero-parameter one — so the base initializer always runs.
    classCtx.SuperInitRequired = false;
    classCtx.SuperInitCalled = false;
    if (method->getName() == names::kMethodInit && ct->getSuperClass() &&
        ct->getSuperClass() != Ctx.getObjTy()) {
      auto *superInit = ct->getSuperClass()->findMethod(names::kMethodInit);
      if (superInit)
        classCtx.SuperInitRequired = true;
    }

    // Resolve return type (already validated in Phase 4; needed for body).
    ast::Type *retTy = Ctx.getVoidTy();
    if (method->getReturnType())
      retTy = resolveType(method->getReturnType(), method->getLocation(),
                          "method '" + method->getName() + "'");

    // Resolve parameter types.
    std::vector<ast::Type *> paramTys;
    for (auto &p : method->getParams()) {
      auto *pty = resolveType(p.ParamType, method->getLocation(),
                              "parameter '" + p.getName() + "'");
      paramTys.push_back(pty ? pty : Ctx.getVoidTy());
    }

    auto *savedRetTy = CurrentReturnType;
    CurrentReturnType = retTy;
    {
      ScopeGuard guard(*this);
      CurrentScope->declare(names::kSelf, ct);
      for (size_t i = 0; i < method->getParams().size(); ++i)
        CurrentScope->declare(method->getParams()[i].getName(), paramTys[i]);

      bool bodyOk = true;
      for (auto *stmt : method->getBody()->getStatements())
        if (!visit(stmt))
          bodyOk = false;

      if (!bodyOk) {
        ok = false;
      } else {
        if (retTy != Ctx.getVoidTy() &&
            !detail::blockAlwaysReturns(method->getBody()->getStatements())) {
          error(method->getLocation(), "non-void method '" + method->getName() +
                                           "' in class '" + node->getName() +
                                           "' does not always return a value");
          ok = false;
        }
        if (classCtx.SuperInitRequired) {
          const auto &stmts = method->getBody()->getStatements();
          bool firstIsSuper = !stmts.empty() && isSuperInitCall(stmts.front());
          if (!firstIsSuper) {
            // Distinguish "never called" from "called but not first" so the
            // diagnostic points the programmer at the actual fix.
            if (classCtx.SuperInitCalled)
              error(method->getLocation(),
                    std::string("'") + names::kMethodSuper +
                        "' must be the first statement of '" +
                        names::kMethodInit + "' in class '" + node->getName() +
                        "'");
            else
              error(method->getLocation(),
                    std::string("'") + names::kMethodInit + "' in class '" +
                        node->getName() + "' must call '" +
                        names::kMethodSuper +
                        "' as its first statement because its superclass '" +
                        ct->getSuperClass()->getName() + "' has an '" +
                        names::kMethodInit + "'");
            ok = false;
          }
        }

        // Definite-assignment: every own field must be set on every path out
        // of __init__.  Run after the body type-checks so subject types (used
        // for match-exhaustiveness) are resolved.
        if (method->getName() == names::kMethodInit) {
          if (!checkInitFieldsAssigned(ct, method->getBody(),
                                       method->getLocation(), node->getName()))
            ok = false;
        }
      }
    }
    CurrentReturnType = savedRetTy;
  }

  CurrentClassCtx = savedClassCtx;
  return ok;
}

} // namespace sema
} // namespace paykan
