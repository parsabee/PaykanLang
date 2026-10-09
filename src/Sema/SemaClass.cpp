// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: class declarations and method bodies.

#include "ASTClone.h"
#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

#include "StringMap.h"

#include <functional>
#include <unordered_map>

namespace {

/// Definite-assignment bit set: one bit per own field of the class.
using BitSet = std::vector<bool>;

BitSet bitAnd(const BitSet &a, const BitSet &b) {
  BitSet out(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    out[i] = a[i] && b[i];
  return out;
}

} // namespace

namespace paykan {
namespace sema {

// -- Phase helpers — class declaration checking

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

// -- Definite-assignment analysis for __init__ (Check 1)

bool Sema::checkInitFieldsAssigned(ast::ClassType *ct, ast::CompoundStmt *body,
                                   ast::SourceLocation initLoc,
                                   const std::string &className) {
  // Own fields only — base fields are initialised by the required __super__.
  const auto &fields = ct->getFields();
  if (fields.empty())
    return true;

  // Map each own field name to a bit index.
  StringMap<unsigned> fieldIndex;
  for (unsigned i = 0; i < fields.size(); ++i)
    fieldIndex[fields[i].first] = i;
  const unsigned n = static_cast<unsigned>(fields.size());
  const BitSet full(n, true);

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
  BitSet reported(n, false);
  bool ok = true;

  auto reportMissing = [&](const BitSet &assigned, ast::SourceLocation loc) {
    for (unsigned i = 0; i < n; ++i) {
      if (!assigned[i] && !reported[i]) {
        reported[i] = true;
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
    BitSet Assigned;
    bool AlwaysReturns = false;
  };

  std::function<Flow(ast::Stmt *, const BitSet &)> analyzeStmt;
  std::function<Flow(const std::vector<ast::Stmt *> &, const BitSet &)>
      analyzeBlock;

  analyzeBlock = [&](const std::vector<ast::Stmt *> &stmts,
                     const BitSet &in) -> Flow {
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

  analyzeStmt = [&](ast::Stmt *s, const BitSet &in) -> Flow {
    if (auto *ma = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
      BitSet out = in;
      int bit = selfFieldBit(ma);
      if (bit >= 0)
        out[static_cast<unsigned>(bit)] = true;
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
      BitSet out = bitAnd(thenF.Assigned, elseF.Assigned);
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
      BitSet out = full;
      bool anyFallThrough = false;
      for (ast::MatchArm *arm : ms->getArms()) {
        Flow af = analyzeBlock(arm->getBody()->getStatements(), in);
        if (!af.AlwaysReturns) {
          anyFallThrough = true;
          out = bitAnd(out, af.Assigned);
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
  // linked structures ergonomic (see
  // docs/language/10-optionals.md).
  BitSet entry(n, false);
  for (unsigned i = 0; i < n; ++i)
    if (ast::isa<ast::OptionalType>(fields[i].second))
      entry[i] = true;
  Flow result = analyzeBlock(body->getStatements(), entry);
  if (!result.AlwaysReturns)
    reportMissing(result.Assigned, initLoc);
  return ok;
}

bool Sema::isErroneousClass(const ast::ClassType *ct) const {
  for (; ct; ct = ct->getSuperClass())
    if (ErroneousClasses.count(ct->getName()))
      return true;
  return false;
}

bool Sema::checkClassDecls(const std::vector<ast::ClassDecl *> &classDecls) {
  bool ok = true;

  // A rejected class is not allowed to take the rest of the module down with
  // it: every phase below reports the bad class and carries on with the
  // others, so the classes that are fine are still declared and only the
  // real error is reported (see ErroneousNames / ErroneousClasses).

  // Phase 1: Check for duplicate class names (local, and against builtins,
  //          imports, and enums).  The class name doubles as its constructor
  //          function (phase 4b), so it must be free in the function table
  //          too — otherwise `class print {...}` would silently replace the
  //          builtin `print`.  A class whose name is taken is dropped; the
  //          name keeps resolving to whatever already owns it.
  StringSet seen;
  StringMap<ast::ClassDecl *> localClasses; // the accepted ones
  std::vector<ast::ClassDecl *> accepted;
  for (auto *cd : classDecls) {
    if (!seen.insert(cd->getName()).second) {
      error(cd->getLocation(), "redefinition of class '" + cd->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(cd->getName(), cd->getLocation(),
                                DeclKind::Class)) {
      ok = false;
      ErroneousNames.insert(cd->getName());
      continue;
    }
    localClasses.try_emplace(cd->getName(), cd);
    accepted.push_back(cd);
  }

  // Phase 2: Topological sort (superclass before subclass) + cycle detection.
  //          A class whose superclass is undefined, was dropped in phase 1,
  //          or closes an inheritance cycle loses its superclass (it is
  //          registered on Obj in phase 3) and is marked erroneous.
  std::vector<ast::ClassDecl *> sorted;
  StringMap<uint8_t> color; // 0=white 1=gray(in-progress) 2=black(done)
  StringSet noSuper;

  std::function<void(ast::ClassDecl *)> topoVisit = [&](ast::ClassDecl *cd) {
    if (color[cd->getName()] != 0)
      return; // done, or in progress further up (a cycle, reported below)
    color[cd->getName()] = 1; // gray — in progress
    if (cd->hasSuperClass()) {
      const auto &superName = cd->getSuperClassName();
      auto it = localClasses.find(superName);
      bool broken = false;
      if (it != localClasses.end()) {
        if (color[superName] == 1) {
          // The superclass is still in progress: this edge closes a cycle.
          error(it->second->getLocation(),
                "cyclic inheritance involving class '" + superName + "'");
          broken = true;
        } else {
          topoVisit(it->second); // process superclass first
        }
      } else if (ErroneousNames.count(superName)) {
        broken = true; // the superclass was rejected (already reported)
      } else if (!Ctx.lookupClassType(superName)) {
        // Not declared in this module: the name must resolve through the
        // ASTContext registry — a built-in class type (Obj, Str) or an
        // imported class referenced by a registered alias (e.g.
        // "tmp_import::helper::Adder").  A class of a failed import was
        // reported with the import.
        if (!isFailedImportUse(superName))
          error(cd->getLocation(), "superclass '" + superName + "' of class '" +
                                       cd->getName() + "' is not defined");
        broken = true;
      }
      if (broken) {
        ok = false;
        noSuper.insert(cd->getName());
        ErroneousClasses.insert(cd->getName());
      }
    }
    color[cd->getName()] = 2; // black — done
    sorted.push_back(cd);
  };

  for (auto *cd : accepted)
    topoVisit(cd);

  // Phase 3: Pre-register ClassType stubs so forward field-type references
  //          within this module resolve correctly.
  for (auto *cd : sorted) {
    ast::ClassType *superClass = nullptr;
    if (cd->hasSuperClass() && !noSuper.count(cd->getName())) {
      superClass = Ctx.lookupClassType(cd->getSuperClassName());
      // Non-inheritable (final) classes cannot be subclassed.
      // Currently this covers built-in types like Str; it will also apply
      // to user-defined `final` classes once that keyword is added.
      if (superClass && superClass->isFinal()) {
        error(cd->getLocation(), "cannot inherit from '" +
                                     cd->getSuperClassName() +
                                     "': class is final");
        ok = false;
        ErroneousClasses.insert(cd->getName());
        superClass = nullptr; // fall back to Obj so analysis can continue
      }
    }
    if (!superClass)
      superClass = Ctx.getObjTy();
    Ctx.preRegisterClassType(cd->getName(), superClass);
  }

  // Phase 4: Populate field types and method signatures for each class.  A
  //          member that fails to resolve is left out of the class.
  for (auto *cd : sorted) {
    auto *ct = Ctx.lookupClassType(cd->getName());
    assert(ct && "ClassType stub must exist after pre-registration");
    if (!populateClassType(cd, ct)) {
      ok = false;
      ErroneousClasses.insert(cd->getName());
    }
  }

  // -- Phase 4b: Register constructor functions so method bodies can call them.
  for (auto *cd : sorted)
    declareConstructor(cd, Ctx.lookupClassType(cd->getName()));

  // Method bodies (Phase 5) are checked later by checkClassBodies(), after free
  // functions are forward-declared — so a method may call any module function.
  // Remember the topological order for that pass.
  SortedClasses = std::move(sorted);
  return ok;
}

bool Sema::populateClassType(ast::ClassDecl *cd, ast::ClassType *ct) {
  bool ok = true;

  // Re-inherit the parent vtable now that it is fully populated.
  if (auto *super = ct->getSuperClass())
    ct->reinheritVTable(super);

  // -- fields --
  StringSet fieldNames;
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
          error(field->getLocation(), "field '" + field->getName() +
                                          "' in class '" + cd->getName() +
                                          "' shadows a field in superclass '" +
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
    field->setType(fty); // canonical write-back for the lowering
    ct->addField(field->getName(), fty);
  }

  // -- methods --
  StringSet methodNames;
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
      method->setReturnType(retTy);
    }
    // Resolve parameter types.
    std::vector<ast::Type *> paramTys;
    bool paramsOk = true;
    for (auto &p : method->getMutableParams()) {
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
      if (!checkBinderName(p.getName(), method->getLocation())) {
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
      p.ParamType = pty;
      paramTys.push_back(pty);
    }
    if (!paramsOk)
      continue;

    // -- override signature check
    // If this method overrides a base-class method (same name found by
    // walking up the inheritance chain), the override must have an identical
    // signature: same parameter types and return type.  Strict equality is
    // enforced (no covariance).  __init__ is excluded — it is a per-class
    // constructor, not a virtual override.
    if (method->getName() != names::kMethodInit) {
      if (auto *super = ct->getSuperClass()) {
        if (auto *baseMethod = super->findMethod(method->getName())) {
          if (!checkOverrideModes(method, baseMethod))
            ok = false;
          const auto &baseParams = baseMethod->getParamTypes();
          bool sigMatches = typesEqual(baseMethod->getReturnType(), retTy) &&
                            baseParams.size() == paramTys.size();
          for (size_t i = 0; sigMatches && i < paramTys.size(); ++i)
            sigMatches = typesEqual(baseParams[i], paramTys[i]);
          if (!sigMatches) {
            error(method->getLocation(),
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

    // `view fn`: not on __init__, and kept by an override.
    const ast::MethodDecl *overridden = nullptr;
    if (method->getName() != names::kMethodInit && ct->getSuperClass())
      overridden = ct->getSuperClass()->findMethod(method->getName());
    if (!checkViewFnDecl(method, overridden))
      ok = false;

    auto *mdecl = Ctx.make<ast::MethodDecl>(
        method->getLocation(), method->getName(), retTy, std::move(paramTys),
        method->isView() ? ast::MethodDecl::View : ast::MethodDecl::None);
    mdecl->setParamModes(ast::paramModes(method->getParams()));
    ct->addMethod(mdecl);
  }
  PopulatedClasses.insert(cd->getName());
  return ok;
}

void Sema::declareConstructor(ast::ClassDecl *cd, ast::ClassType *ct) {
  auto *initDecl = ct->findMethod(names::kMethodInit);
  std::vector<ast::Type *> ctorParams;
  if (initDecl)
    ctorParams = initDecl->getParamTypes();
  // The class name was checked to be free of any function (builtin or
  // otherwise), so this registration never overwrites an existing entry.
  assert(!lookupFunction(cd->getName()) &&
         "constructor would overwrite a registered function");
  declareFunction(cd->getName(), ct, ctorParams);
  if (initDecl)
    FunctionTable[cd->getName()].Modes = initDecl->getParamModes();
}

bool Sema::checkClassBodies() {
  // Phase 5: Type-check method bodies (class types, fields, method signatures,
  // constructors, and all free functions are registered by now).
  bool ok = true;
  for (auto *cd : SortedClasses) {
    // An erroneous class is incomplete (members that failed to resolve are
    // missing), so its bodies would only produce follow-on errors.
    if (ErroneousClasses.count(cd->getName()))
      continue;
    if (!visitClassDecl(cd))
      ok = false;
  }
  return ok;
}

// -- visitMethodDecl / visitClassDecl

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
    classCtx.ViewMethod = method->isView();

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
      // Unreachable in practice (a class with an unresolvable parameter type
      // is erroneous and its bodies are skipped); poisoned if it ever is.
      paramTys.push_back(pty ? pty : Ctx.getPoisonTy());
    }

    auto *savedRetTy = CurrentReturnType;
    CurrentReturnType = retTy;
    {
      ScopeGuard guard(*this);
      CurrentScope->declare(names::kSelf, ct);
      for (size_t i = 0; i < method->getParams().size(); ++i)
        CurrentScope->declare(method->getParams()[i].getName(), paramTys[i]);
      declareParamKinds(method);

      // What the body does to `self`, for warnMissingViewFns.
      if (method->getName() != names::kMethodInit) {
        CurrentMethodUse = &MethodUses.emplace_back();
        CurrentMethodUse->Class = ct;
        CurrentMethodUse->Decl = method;
      }

      bool bodyOk = true;
      for (auto *stmt : method->getBody()->getStatements())
        if (!visit(stmt))
          bodyOk = false;
      CurrentMethodUse = nullptr;

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

// -- Generics: templates, instantiation, type-argument inference
//
// See the comment block in Sema.h ("Generics") and
// docs/language/11-generics.md for the design.  In short: a generic declaration
// is a template that is registered by name and never checked itself; each use
// with a distinct tuple of canonical type arguments clones the declaration with
// the type parameters substituted (ast::ASTCloner), registers the clone under
// its canonical name (`Box<int>`) exactly like a hand-written class/function,
// and queues its bodies for checking.  Instantiations are ordinary ClassTypes /
// functions afterwards, so the lowering and the module exporter need no
// generics-specific paths.

bool Sema::registerGenericTemplates(ast::TranslationUnit *tu) {
  bool ok = true;
  LocalClassNames.clear();
  for (auto *cd : tu->getClassDecls())
    LocalClassNames.insert(cd->getName());

  // A type parameter list must not repeat a name, and a parameter must not
  // shadow a type: inside the template `T` would silently mean the type
  // argument instead of the class, or -- since the parser resolves known
  // names at parse time -- the class instead of the type argument.
  auto checkTypeParams = [&](const std::vector<const std::string *> &params,
                             const std::string &what, const std::string &name,
                             ast::SourceLocation loc) -> bool {
    bool pok = true;
    StringSet seen;
    for (auto *p : params) {
      if (!seen.insert(*p).second) {
        std::string msg = "duplicate type parameter '";
        msg += *p;
        msg += "' in ";
        msg += what;
        msg += " '";
        msg += name;
        msg += "'";
        error(loc, msg);
        pok = false;
        continue;
      }
      if (Ctx.lookupType(*p) || LocalClassNames.count(*p) ||
          ClassTemplates.count(*p)) {
        std::string msg = "type parameter '";
        msg += *p;
        msg += "' of ";
        msg += what;
        msg += " '";
        msg += name;
        msg += "' shadows a type of the same name";
        error(loc, msg);
        pok = false;
      }
    }
    return pok;
  };

  for (auto *cd : tu->getGenericClassDecls()) {
    if (ClassTemplates.count(cd->getName())) {
      error(cd->getLocation(),
            "redefinition of generic class '" + cd->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(cd->getName(), cd->getLocation(),
                                DeclKind::Class)) {
      ok = false;
      continue;
    }
    if (!checkTypeParams(cd->getTypeParams(), "generic class", cd->getName(),
                         cd->getLocation())) {
      ok = false;
      continue;
    }
    ClassTemplates[cd->getName()] = cd;
  }

  for (auto *fn : tu->getGenericFuncDecls()) {
    if (FuncTemplates.count(fn->getName())) {
      error(fn->getLocation(),
            "redefinition of generic function '" + fn->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(fn->getName(), fn->getLocation(),
                                DeclKind::Function)) {
      ok = false;
      continue;
    }
    if (!checkTypeParams(fn->getTypeParams(), "generic function", fn->getName(),
                         fn->getLocation())) {
      ok = false;
      continue;
    }
    FuncTemplates[fn->getName()] = fn;
  }
  return ok;
}

// static
std::string Sema::instantiationName(const std::string &templateName,
                                    const std::vector<ast::Type *> &args) {
  // `Box<int>`, `Pair<Str, int>`, `Box<int[]>`, `Box<Box<int>>` -- the same
  // spelling ast::typeName produces for the instantiation afterwards, so the
  // name round-trips through module export/import and reads naturally in
  // diagnostics.  The arguments are canonical, so equal spellings mean equal
  // types and the name is a valid cache key.  LLVM symbol names may contain
  // any character (they are quoted in textual IR), so `Box<int>_vtable` is a
  // legal global -- the same convention the Array<T> specialisations use.
  std::string s = templateName + "<";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i)
      s += ", ";
    s += ast::typeName(args[i]);
  }
  return s + ">";
}

void Sema::errorImportedTemplate(ast::SourceLocation loc,
                                 const std::string &name,
                                 const std::string &msg) {
  if (isFailedImportUse(name))
    return; // the import failed, and was reported
  const std::string key = std::to_string(loc.getLineStart()) + ":" +
                          std::to_string(loc.getColumnStart()) + ":" + name;
  if (!ReportedImportedTemplateUses.insert(key).second) {
    ++SuppressedFollowOns; // the same use, in another instantiation
    return;
  }
  error(loc, msg);
}

ast::ClassType *Sema::instantiateClass(const std::string &name,
                                       const std::vector<ast::Type *> &args,
                                       ast::SourceLocation loc) {
  auto tIt = ClassTemplates.find(name);
  if (tIt == ClassTemplates.end()) {
    if (ErroneousNames.count(name))
      ++SuppressedFollowOns; // a template rejected at its declaration
    else if (name.find(names::kQualSep) != std::string::npos)
      errorImportedTemplate(loc, name,
                            "generic types cannot be imported yet: '" + name +
                                "<...>' names a generic class of another "
                                "module");
    else if (Ctx.lookupClassType(name) || Ctx.lookupEnumType(name))
      error(loc, "'" + name +
                     "' is not a generic class and takes no type "
                     "arguments");
    else if (FuncTemplates.count(name))
      error(loc, "'" + name + "' is a generic function, not a generic class");
    else
      error(loc, "unknown generic class '" + name + "'");
    return nullptr;
  }
  ast::ClassDecl *tmpl = tIt->second;
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc, "generic class '" + name + "' expects " +
                   std::to_string(tmpl->getTypeParams().size()) +
                   " type argument(s), got " + std::to_string(args.size()));
    return nullptr;
  }

  const std::string instName = instantiationName(name, args);
  if (auto cIt = ClassInstantiations.find(instName);
      cIt != ClassInstantiations.end())
    return cIt->second; // cached (nullptr records an earlier failure)

  if (Ctx.lookupClassType(instName)) {
    // An imported module exported its own `Box<int>` as a concrete class.
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported class of the same name "
                   "(type names are global across imports)");
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (InstantiationStack.size() >= kMaxInstantiationDepth) {
    error(loc, "instantiating '" + instName +
                   "' exceeds the maximum instantiation depth (" +
                   std::to_string(kMaxInstantiationDepth) +
                   "); a generic class may not instantiate itself with an "
                   "ever-growing type argument");
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }

  // Clone the template with the type parameters substituted.
  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst));
  ast::ClassDecl *clone = cloner.cloneClassDecl(tmpl, instName);

  InstantiationStack.push_back({instName, loc});
  bool ok = true;

  // Superclass (phases 2-3 of checkClassDecls, for this one class).
  ast::ClassType *superClass = nullptr;
  if (clone->hasSuperClass()) {
    const auto &superName = clone->getSuperClassName();
    superClass = Ctx.lookupClassType(superName);
    if (!superClass) {
      if (!isFailedImportUse(superName))
        error(clone->getLocation(), "superclass '" + superName +
                                        "' of class '" + instName +
                                        "' is not defined");
      ok = false;
    } else if (superClass->isFinal()) {
      error(clone->getLocation(),
            "cannot inherit from '" + superName + "': class is final");
      ok = false;
      superClass = nullptr;
    } else if (LocalClassNames.count(superName) &&
               !PopulatedClasses.count(superName)) {
      // The instantiation would copy an incomplete vtable.
      error(loc, "cannot instantiate '" + instName +
                     "' here: its superclass '" + superName +
                     "' is declared later in this module and is not complete "
                     "yet; use the instantiation after the declaration of '" +
                     superName + "' or move '" + superName + "' up");
      ok = false;
    }
  }
  if (!superClass)
    superClass = Ctx.getObjTy();

  ast::ClassType *ct = Ctx.preRegisterClassType(instName, superClass);
  // Cache before populating: a self-referential field (`next: Node<T>`)
  // resolves to this very type instead of recursing.
  ClassInstantiations[instName] = ct;
  ClassInstantiationInfo[ct] = InstantiationInfo{name, args};

  if (!populateClassType(clone, ct))
    ok = false;
  if (ok)
    declareConstructor(clone, ct);
  InstantiationStack.pop_back();

  if (!ok) {
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }

  PendingInstantiation pending;
  pending.Class = clone;
  pending.Name = instName;
  pending.RequestLoc = loc;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  PendingInstantiations.push_back(std::move(pending));
  InstantiatedClassDecls.push_back(clone);
  return ct;
}

std::string Sema::instantiateFunction(const std::string &name,
                                      const std::vector<ast::Type *> &args,
                                      ast::SourceLocation loc) {
  auto tIt = FuncTemplates.find(name);
  if (tIt == FuncTemplates.end()) {
    if (ErroneousNames.count(name))
      ++SuppressedFollowOns; // a template rejected at its declaration
    else if (name.find(names::kQualSep) != std::string::npos)
      errorImportedTemplate(loc, name,
                            "generic functions cannot be imported yet: '" +
                                name +
                                "<...>' names a generic function of another "
                                "module");
    else if (lookupFunction(name))
      error(loc, "'" + name +
                     "' is not a generic function and takes no type "
                     "arguments");
    else
      error(loc, "unknown generic function '" + name + "'");
    return "";
  }
  ast::FuncDecl *tmpl = tIt->second;
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc, "generic function '" + name + "' expects " +
                   std::to_string(tmpl->getTypeParams().size()) +
                   " type argument(s), got " + std::to_string(args.size()));
    return "";
  }

  std::string instName = instantiationName(name, args);
  if (auto fIt = FuncInstantiations.find(instName);
      fIt != FuncInstantiations.end())
    return fIt->second ? instName : "";

  if (lookupFunction(instName)) {
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported function of the same name");
    FuncInstantiations[instName] = false;
    return "";
  }
  if (InstantiationStack.size() >= kMaxInstantiationDepth) {
    error(loc, "instantiating '" + instName +
                   "' exceeds the maximum instantiation depth (" +
                   std::to_string(kMaxInstantiationDepth) + ")");
    FuncInstantiations[instName] = false;
    return "";
  }

  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst));
  ast::FuncDecl *clone = cloner.cloneFuncDecl(tmpl, instName);

  InstantiationStack.push_back({instName, loc});
  bool ok = declareFunctionSignature(clone);
  InstantiationStack.pop_back();
  FuncInstantiations[instName] = ok;
  if (!ok)
    return "";

  PendingInstantiation pending;
  pending.Func = clone;
  pending.Name = instName;
  pending.RequestLoc = loc;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  PendingInstantiations.push_back(std::move(pending));
  InstantiatedFuncDecls.push_back(clone);
  return instName;
}

bool Sema::unifyTypes(ast::Type *pattern, ast::Type *actual,
                      const StringSet &typeParams,
                      StringMap<ast::Type *> &bindings,
                      InferenceConflict &conflict) {
  if (!pattern || !actual)
    return true;

  if (auto *ct = ast::dyn_cast<ast::ClassType>(pattern)) {
    // A type parameter binds to the argument type; a second occurrence must
    // agree exactly (no promotion, no subtyping: `pick(1, 2.0)` against
    // `pick<T>(a: T, b: T)` is a conflict, not float).
    if (!typeParams.count(ct->getName()))
      return true; // concrete class: left to the argument check
    auto it = bindings.find(ct->getName());
    if (it == bindings.end()) {
      bindings[ct->getName()] = actual;
      return true;
    }
    if (typesEqual(it->second, actual))
      return true;
    conflict = {ct->getName(), it->second, actual};
    return false;
  }

  if (auto *at = ast::dyn_cast<ast::ArrayType>(pattern)) {
    auto *aat = ast::dyn_cast<ast::ArrayType>(actual);
    if (!aat)
      return true;
    // An empty array literal ([]: element void) carries no information.
    if (aat->getElementType() == Ctx.getVoidTy())
      return true;
    return unifyTypes(at->getElementType(), aat->getElementType(), typeParams,
                      bindings, conflict);
  }

  if (auto *tt = ast::dyn_cast<ast::TupleType>(pattern)) {
    // `(T, int)` against a tuple of the same arity: unify element-wise.  A
    // mismatched arity is left to the argument check.
    auto *att = ast::dyn_cast<ast::TupleType>(actual);
    if (!att || att->getArity() != tt->getArity())
      return true;
    for (size_t i = 0; i < tt->getArity(); ++i)
      if (!unifyTypes(tt->getElementType(i), att->getElementType(i), typeParams,
                      bindings, conflict))
        return false;
    return true;
  }

  if (auto *ot = ast::dyn_cast<ast::OptionalType>(pattern)) {
    // `T?` against `U?` or a plain `U` (a `U` widens to `U?`) binds T to U.
    // A bare `None` argument is typed Obj and says nothing about T, so it
    // binds nothing: if no other argument determines T the call must spell
    // its type arguments, rather than silently instantiating with Obj.
    if (auto *aot = ast::dyn_cast<ast::OptionalType>(actual))
      return unifyTypes(ot->getInnerType(), aot->getInnerType(), typeParams,
                        bindings, conflict);
    if (actual == Ctx.getObjTy())
      return true;
    return unifyTypes(ot->getInnerType(), actual, typeParams, bindings,
                      conflict);
  }

  if (auto *gt = ast::dyn_cast<ast::GenericType>(pattern)) {
    // `Box<T>` against a class: find an instantiation of the same template
    // in the argument's class or one of its ancestors and unify argument-wise.
    for (auto *c = ast::dyn_cast<ast::ClassType>(actual); c;
         c = c->getSuperClass()) {
      auto it = ClassInstantiationInfo.find(c);
      if (it == ClassInstantiationInfo.end() ||
          it->second.TemplateName != gt->getName() ||
          it->second.Args.size() != gt->getNumArgs())
        continue;
      for (size_t i = 0; i < gt->getNumArgs(); ++i)
        if (!unifyTypes(gt->getArgs()[i], it->second.Args[i], typeParams,
                        bindings, conflict))
          return false;
      return true;
    }
    return true;
  }

  return true; // builtin / enum: nothing to bind
}

bool Sema::inferTypeArgs(const std::string &templateName,
                         const std::vector<const std::string *> &typeParams,
                         const std::vector<ast::Type *> &paramTypes,
                         const std::vector<ast::Type *> &argTypes,
                         ast::SourceLocation loc,
                         std::vector<ast::Type *> &out) {
  const std::string hint =
      "specify the type arguments explicitly: '" + templateName + "<...>(...)'";
  if (paramTypes.size() != argTypes.size()) {
    error(loc, "cannot infer the type arguments of '" + templateName +
                   "': it expects " + std::to_string(paramTypes.size()) +
                   " argument(s), got " + std::to_string(argTypes.size()));
    return false;
  }
  StringSet params;
  for (auto *p : typeParams)
    params.insert(*p);

  StringMap<ast::Type *> bindings;
  InferenceConflict conflict;
  for (size_t i = 0; i < paramTypes.size(); ++i) {
    if (!unifyTypes(paramTypes[i], argTypes[i], params, bindings, conflict)) {
      error(loc, "cannot infer type parameter '" + conflict.Param + "' of '" +
                     templateName + "': deduced as both '" +
                     typeName(conflict.First) + "' and '" +
                     typeName(conflict.Second) + "'");
      note(loc, hint);
      return false;
    }
  }
  out.clear();
  for (auto *p : typeParams) {
    auto it = bindings.find(*p);
    if (it == bindings.end()) {
      error(loc, "cannot infer type parameter '" + *p + "' of '" +
                     templateName + "' from the call arguments");
      note(loc, hint);
      return false;
    }
    out.push_back(it->second);
  }
  return true;
}

bool Sema::resolveGenericCall(ast::CallExpr *node,
                              const std::vector<ast::Type *> &argTypes) {
  const std::string &callee = node->getCalleeName();
  const ast::SourceLocation loc = node->getLocation();

  // Explicit type arguments (`first<int>(xs)`) resolve like any annotation.
  std::vector<ast::Type *> typeArgs;
  for (size_t i = 0; i < node->getTypeArgs().size(); ++i) {
    auto *t = resolveType(node->getTypeArgs()[i], loc,
                          "type argument " + std::to_string(i + 1) + " of '" +
                              callee + "'");
    if (!t)
      return false;
    typeArgs.push_back(t);
  }

  // Templates are never qualified (they are not exported), so a qualified
  // callee can only get here with explicit type arguments.
  if (callee.find(names::kQualSep) != std::string::npos) {
    errorImportedTemplate(loc, callee,
                          "generic types and functions cannot be imported "
                          "yet: '" +
                              callee +
                              "<...>' names a template of another module");
    return false;
  }

  if (auto it = ClassTemplates.find(callee); it != ClassTemplates.end()) {
    ast::ClassDecl *tmpl = it->second;
    if (typeArgs.empty()) {
      // Constructor-argument inference: `Box(3)` unifies __init__'s
      // parameter types with the argument types.
      std::vector<ast::Type *> paramTypes;
      for (auto *m : tmpl->getMethods())
        if (m->getName() == names::kMethodInit)
          for (auto &p : m->getParams())
            paramTypes.push_back(p.ParamType);
      if (!inferTypeArgs(callee, tmpl->getTypeParams(), paramTypes, argTypes,
                         loc, typeArgs))
        return false;
    }
    auto *ct = instantiateClass(callee, typeArgs, loc);
    if (!ct)
      return false;
    node->setCalleeName(ct->getName()); // interned by the ASTContext
    return true;
  }

  if (auto it = FuncTemplates.find(callee); it != FuncTemplates.end()) {
    ast::FuncDecl *tmpl = it->second;
    if (typeArgs.empty()) {
      std::vector<ast::Type *> paramTypes;
      for (auto &p : tmpl->getParams())
        paramTypes.push_back(p.ParamType);
      if (!inferTypeArgs(callee, tmpl->getTypeParams(), paramTypes, argTypes,
                         loc, typeArgs))
        return false;
    }
    std::string inst = instantiateFunction(callee, typeArgs, loc);
    if (inst.empty())
      return false;
    node->setCalleeName(Ctx.intern(inst));
    return true;
  }

  // Explicit type arguments on something that is not a template.
  if (lookupFunction(callee) || Ctx.lookupClassType(callee))
    error(loc, "'" + callee + "' is not generic and takes no type arguments");
  else if (ErroneousNames.count(callee))
    ++SuppressedFollowOns; // a template rejected at its declaration
  else if (!isFailedImportUse(callee))
    error(loc, "call to undeclared generic function or class '" + callee + "'");
  return false;
}

bool Sema::checkPendingInstantiations() {
  bool ok = true;
  // A body may request further instantiations, which append to the list, so
  // index (not iterate) and copy the entry (the vector may reallocate).
  for (size_t i = 0; i < PendingInstantiations.size(); ++i) {
    PendingInstantiation p = PendingInstantiations[i];
    InstantiationStack.push_back({p.Name, p.RequestLoc});
    const StringSet *savedParams = CurrentTypeParams;
    CurrentTypeParams = &p.TypeParams;
    bool r = p.Class ? visitClassDecl(p.Class) : visitFuncDecl(p.Func);
    CurrentTypeParams = savedParams;
    InstantiationStack.pop_back();
    if (!r)
      ok = false;
  }
  PendingInstantiations.clear();
  return ok;
}

void Sema::injectInstantiations(ast::TranslationUnit *tu) {
  if (InstantiatedClassDecls.empty() && InstantiatedFuncDecls.empty())
    return;

  // The lowering emits classes in list order and resolves the functions a class
  // needs by name at the point of use: a constructor (`B()` in a method of A)
  // and a superclass's `__init__` / vtable slots (`__super__(...)`, inherited
  // methods) must already exist when the class that uses them is emitted.
  // Hand-written classes come in source order and rely on that order; the
  // instantiations have no source position, so merge them in by a post-order
  // walk over "superclass first" and "constructed class first" edges (the
  // latter recorded by Sema::visitCallExpr).  Cycles are cut arbitrarily.
  // The walk starts from the hand-written classes in source order, so a
  // program without generics keeps its list untouched (this function returns
  // early in that case) and one with generics keeps the source order except
  // where an edge requires otherwise.
  std::vector<ast::ClassDecl *> all = tu->getClassDecls();
  all.insert(all.end(), InstantiatedClassDecls.begin(),
             InstantiatedClassDecls.end());
  StringMap<ast::ClassDecl *> byName;
  for (auto *cd : all)
    byName[cd->getName()] = cd;

  std::vector<ast::ClassDecl *> ordered;
  StringSet done, visiting;
  std::function<void(ast::ClassDecl *)> place = [&](ast::ClassDecl *cd) {
    if (done.count(cd->getName()) || !visiting.insert(cd->getName()).second)
      return;
    if (cd->hasSuperClass())
      if (auto s = byName.find(cd->getSuperClassName()); s != byName.end())
        place(s->second);
    if (auto e = ConstructsEdges.find(cd->getName());
        e != ConstructsEdges.end())
      for (const auto &dep : e->second)
        if (auto d = byName.find(dep); d != byName.end())
          place(d->second);
    done.insert(cd->getName());
    ordered.push_back(cd);
  };
  for (auto *cd : all)
    place(cd);

  tu->setClassDecls(std::move(ordered));
  for (auto *fn : InstantiatedFuncDecls)
    tu->addFuncDecl(fn);
  InstantiatedClassDecls.clear();
  InstantiatedFuncDecls.clear();
}

} // namespace sema
} // namespace paykan
