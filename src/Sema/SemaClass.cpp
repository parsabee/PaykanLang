// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: class declarations and method bodies.

#include "Sema.h"
#include "Names.h"
#include "SemaInternal.h"

#include <functional>

namespace paykan {
namespace sema {

// ---------------------------------------------------------------------------
// Phase helpers — class declaration checking
// ---------------------------------------------------------------------------

bool Sema::checkClassDecls(const std::vector<ast::ClassDecl *> &classDecls) {
  bool ok = true;

  // -------------------------------------------------------------------------
  // Phase 1: Check for duplicate class names (local + against imports).
  // -------------------------------------------------------------------------
  llvm::StringMap<ast::ClassDecl *> localClasses;
  for (auto *cd : classDecls) {
    if (!localClasses.try_emplace(cd->getName(), cd).second) {
      error(cd->getLocation(),
            "redefinition of class '" + cd->getName() + "'");
      ok = false;
      continue;
    }
    if (Ctx.lookupClassType(cd->getName())) {
      error(cd->getLocation(),
            "class '" + cd->getName() +
                "' conflicts with an imported type of the same name");
      ok = false;
    }
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
        // Imported base class: must be referenced by its full module path
        // (e.g. "tmp_import::helper::Adder"), not a bare name or short
        // qualifier.  A name with no "::" can only be a local class.
        bool hasQualifier = superName.find("::") != std::string::npos;
        if (!hasQualifier || !Ctx.lookupClassType(superName)) {
          error(cd->getLocation(),
                "superclass '" + superName +
                    "' of class '" + cd->getName() + "' is not defined");
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
    if (cd->hasSuperClass())
      superClass = Ctx.lookupClassType(cd->getSuperClassName());
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
        error(field->getLocation(),
              "duplicate field '" + field->getName() + "' in class '" +
                  cd->getName() + "'");
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
        error(method->getLocation(),
              "duplicate method '" + method->getName() + "' in class '" +
                  cd->getName() + "'");
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
        auto *pty = resolveType(p.ParamType, method->getLocation(),
                                "parameter '" + p.Name + "' of method '" +
                                    method->getName() + "' in '" +
                                    cd->getName() + "'");
        if (!pty) {
          paramsOk = false;
          ok = false;
          break;
        }
        paramTys.push_back(pty);
      }
      if (!paramsOk)
        continue;
      auto *mdecl = Ctx.make<ast::MethodDecl>(method->getLocation(),
                                              method->getName(), retTy,
                                              std::move(paramTys));
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
    declareFunction(cd->getName(), ct, ctorParams);
  }

  // -------------------------------------------------------------------------
  // Phase 5: Type-check method bodies.
  // -------------------------------------------------------------------------
  for (auto *cd : sorted)
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

  for (auto *method : node->getMethods()) {
    classCtx.MethodName = method->getName();

    // Determine whether this __init__ must call __super__.
    classCtx.SuperInitRequired = false;
    classCtx.SuperInitCalled   = false;
    if (method->getName() == names::kMethodInit && ct->getSuperClass() &&
        ct->getSuperClass() != Ctx.getObjTy()) {
      auto *superInit = ct->getSuperClass()->findMethod(names::kMethodInit);
      if (superInit && !superInit->getParamTypes().empty())
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
                              "parameter '" + p.Name + "'");
      paramTys.push_back(pty ? pty : Ctx.getVoidTy());
    }

    auto *savedRetTy = CurrentReturnType;
    CurrentReturnType = retTy;
    {
      ScopeGuard guard(*this);
      CurrentScope->declare(names::kSelf, ct);
      for (size_t i = 0; i < method->getParams().size(); ++i)
        CurrentScope->declare(method->getParams()[i].Name, paramTys[i]);

      bool bodyOk = true;
      for (auto *stmt : method->getBody()->getStatements())
        if (!visit(stmt))
          bodyOk = false;

      if (!bodyOk) {
        ok = false;
      } else {
        if (retTy != Ctx.getVoidTy() &&
            !detail::blockAlwaysReturns(method->getBody()->getStatements())) {
          error(method->getLocation(),
                "non-void method '" + method->getName() +
                    "' in class '" + node->getName() +
                    "' does not always return a value");
          ok = false;
        }
        if (classCtx.SuperInitRequired && !classCtx.SuperInitCalled) {
          error(method->getLocation(),
                std::string("'") + names::kMethodInit + "' in class '" +
                    node->getName() +
                    "' must call '" + names::kMethodSuper + "' because its superclass '" +
                    ct->getSuperClass()->getName() +
                    "' has a parameterized '" + names::kMethodInit + "'");
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
