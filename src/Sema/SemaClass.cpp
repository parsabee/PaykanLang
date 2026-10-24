// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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

bool Sema::checkInitFieldsAssigned(
    const std::vector<std::pair<std::string, ast::Type *>> &fields,
    ast::CompoundStmt *body, ast::SourceLocation initLoc,
    const std::string &kind, const std::string &typeName) {
  // Own fields only — base fields are initialised by the required __super__.
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
  auto isSelf = [](const ast::Expr *e) {
    const auto *id = ast::dyn_cast<ast::Identifier>(e);
    return id && id->getName() == names::kSelf;
  };

  // The set of fields left unassigned on a terminating path; bits are reported
  // once (a class-level error per field) to avoid duplicate diagnostics.
  BitSet reported(n, false);
  bool ok = true;

  auto reportMissing = [&](const BitSet &assigned, ast::SourceLocation loc) {
    for (unsigned i = 0; i < n; ++i) {
      if (!assigned[i] && !reported[i]) {
        reported[i] = true;
        std::string msg = "field '";
        msg += fields[i].first;
        msg += "' of ";
        msg += kind;
        msg += " '";
        msg += typeName;
        msg += "' is not assigned on every path through '";
        msg += names::kMethodInit;
        msg += "'";
        error(loc, msg);
        ok = false;
      }
    }
  };

  // Until every field is assigned (docs/language/05-classes.md, "Every field
  // before self"), `self` is only a place to assign fields and to read the
  // ones assigned on every path so far: a method of it could read one that
  // is not (an override in a subclass included), and so could whatever it
  // is passed to, stored in or captured by.  In `__super__(...)`'s
  // arguments no field is assigned yet, the superclass's neither.
  auto reportSelfUse = [&](ast::SourceLocation loc, const BitSet &assigned,
                           bool inSuper) {
    std::string why;
    if (inSuper) {
      why = "'" + std::string(names::kMethodSuper) + "' has not run yet";
    } else {
      for (unsigned i = 0; i < n && why.empty(); ++i)
        if (!assigned[i])
          why = "'" + fields[i].first + "' is not assigned yet";
    }
    error(loc, "'self' is used before every field is assigned: " + why);
    ok = false;
  };
  // `self.<name>` read as a field: a superclass's field has its value once
  // `__super__(...)` has run, an own one once it is assigned on every path.
  auto checkFieldRead = [&](const std::string &name, ast::SourceLocation loc,
                            const BitSet &assigned, bool inSuper) {
    auto it = fieldIndex.find(name);
    if (inSuper) {
      reportSelfUse(loc, assigned, inSuper);
    } else if (it != fieldIndex.end() && !assigned[it->second]) {
      error(loc, "field '" + name + "' is read before it is assigned");
      ok = false;
    }
  };
  std::function<void(const ast::Expr *, const BitSet &, bool)> checkUses =
      [&](const ast::Expr *e, const BitSet &assigned, bool inSuper) {
        if (!e || (!inSuper && assigned == full))
          return;
        auto each = [&](const std::vector<ast::Expr *> &es) {
          for (const ast::Expr *x : es)
            checkUses(x, assigned, inSuper);
        };
        switch (e->getKind()) {
        case ast::ASTNode::NK_Identifier:
          if (isSelf(e))
            reportSelfUse(e->getLocation(), assigned, inSuper);
          return;
        case ast::ASTNode::NK_MemberAccessExpr: {
          const auto *ma = ast::cast<ast::MemberAccessExpr>(e);
          if (isSelf(ma->getReceiver()))
            checkFieldRead(ma->getFieldName(), e->getLocation(), assigned,
                           inSuper);
          else
            checkUses(ma->getReceiver(), assigned, inSuper);
          return;
        }
        case ast::ASTNode::NK_MethodCallExpr: {
          // `self.f(1)` with `f` a field of function type reads `f` and
          // calls the value it holds; `self` is not given to anything.
          const auto *mc = ast::cast<ast::MethodCallExpr>(e);
          if (!isSelf(mc->getReceiver()))
            checkUses(mc->getReceiver(), assigned, inSuper);
          else if (mc->isFieldCall())
            checkFieldRead(mc->getMethodName(), e->getLocation(), assigned,
                           inSuper);
          else
            reportSelfUse(e->getLocation(), assigned, inSuper);
          each(mc->getArguments());
          return;
        }
        case ast::ASTNode::NK_LambdaExpr:
          // Its body is checked as its own function's; what it captures of
          // `self` is captured now.
          if (ast::cast<ast::LambdaExpr>(e)->capturesSelf())
            reportSelfUse(e->getLocation(), assigned, inSuper);
          return;
        case ast::ASTNode::NK_CallExpr:
          each(ast::cast<ast::CallExpr>(e)->getArguments());
          return;
        case ast::ASTNode::NK_CallValueExpr: {
          const auto *c = ast::cast<ast::CallValueExpr>(e);
          checkUses(c->getCallee(), assigned, inSuper);
          each(c->getArguments());
          return;
        }
        case ast::ASTNode::NK_UnaryExpr:
          checkUses(ast::cast<ast::UnaryExpr>(e)->getOperand(), assigned,
                    inSuper);
          return;
        case ast::ASTNode::NK_BinaryExpr: {
          const auto *b = ast::cast<ast::BinaryExpr>(e);
          checkUses(b->getLHS(), assigned, inSuper);
          checkUses(b->getRHS(), assigned, inSuper);
          return;
        }
        case ast::ASTNode::NK_TernaryExpr: {
          const auto *t = ast::cast<ast::TernaryExpr>(e);
          checkUses(t->getCondition(), assigned, inSuper);
          checkUses(t->getTrueExpr(), assigned, inSuper);
          checkUses(t->getFalseExpr(), assigned, inSuper);
          return;
        }
        case ast::ASTNode::NK_ArrayLiteralExpr:
          each(ast::cast<ast::ArrayLiteralExpr>(e)->getElements());
          return;
        case ast::ASTNode::NK_SubscriptExpr: {
          const auto *se = ast::cast<ast::SubscriptExpr>(e);
          checkUses(se->getArray(), assigned, inSuper);
          checkUses(se->getIndex(), assigned, inSuper);
          return;
        }
        case ast::ASTNode::NK_TupleLiteralExpr:
          each(ast::cast<ast::TupleLiteralExpr>(e)->getElements());
          return;
        case ast::ASTNode::NK_TupleIndexExpr:
          checkUses(ast::cast<ast::TupleIndexExpr>(e)->getTuple(), assigned,
                    inSuper);
          return;
        default:
          return; // a literal or an enum value: no `self` in it
        }
      };
  // A write in place (`self.p.x = v` with `p` a struct, `self.a[i] = v` with
  // `a` a fixed-size array) goes into the storage of the field it starts
  // from, which it neither reads nor assigns: only the indexes on the way are
  // read.  A write through an object (`self.h.n = v`) reads the field that
  // holds the object.
  auto inPlace = [](const ast::Expr *e) {
    const ast::Type *t = e->getResolvedType(); // null-safe below
    return ast::dyn_cast<ast::StructType>(t) != nullptr ||
           ast::dyn_cast<ast::FixedArrayType>(t) != nullptr;
  };
  std::function<void(const ast::Expr *, const BitSet &)> checkWrittenIn =
      [&](const ast::Expr *place, const BitSet &assigned) {
        const auto *ma = ast::dyn_cast<ast::MemberAccessExpr>(place);
        const auto *se = ast::dyn_cast<ast::SubscriptExpr>(place);
        if (!inPlace(place) || (!ma && !se)) {
          checkUses(place, assigned, false);
        } else if (ma) {
          // Up to the field's own storage, `self.f`.
          if (!isSelf(ma->getReceiver()))
            checkWrittenIn(ma->getReceiver(), assigned);
        } else {
          checkWrittenIn(se->getArray(), assigned);
          checkUses(se->getIndex(), assigned, false);
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
      // `self.f = v` assigns f; `self.p.x = v` writes in p's storage, which
      // leaves p as it was (checkWrittenIn).
      if (!isSelf(ma->getReceiver()))
        checkWrittenIn(ma->getReceiver(), in);
      checkUses(ma->getValue(), in, false);
      BitSet out = in;
      int bit = selfFieldBit(ma);
      if (bit >= 0)
        out[static_cast<unsigned>(bit)] = true;
      // A class's object becomes one of the class where its last field is
      // assigned (LoweringClass.cpp, emitInitBody).
      if (kind == "class" && in != full && out == full)
        ma->setCompletesInit(true);
      return {out, false};
    }
    if (auto *ret = ast::dyn_cast<ast::ReturnStmt>(s)) {
      checkUses(ret->getReturnValue(), in, false);
      // Every field must be assigned before any return.
      reportMissing(in, ret->getLocation());
      return {in, true};
    }
    if (auto *cs = ast::dyn_cast<ast::CompoundStmt>(s))
      return analyzeBlock(cs->getStatements(), in);
    if (auto *ifs = ast::dyn_cast<ast::IfStmt>(s)) {
      checkUses(ifs->getCondition(), in, false);
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
      checkUses(ws->getCondition(), in, false);
      analyzeStmt(ws->getBody(), in);
      // `while True` with no `break` out of it is left only by a return.
      return {in, detail::stmtAlwaysReturns(ws)};
    }
    if (auto *fs = ast::dyn_cast<ast::ForStmt>(s)) {
      // As a while loop's: the range may be empty.
      checkUses(fs->getStart(), in, false);
      checkUses(fs->getEnd(), in, false);
      checkUses(fs->getStep(), in, false);
      analyzeStmt(fs->getBody(), in);
      return {in, false};
    }
    if (auto *ms = ast::dyn_cast<ast::MatchStmt>(s)) {
      // Analyse every arm (so returns within are validated), then apply
      // exhaustiveness: only an exhaustive match (wildcard arm, every enum
      // variant, both bool literals, or None + the wrapped type for an
      // optional subject — see detail::matchIsExhaustive) can guarantee
      // assignments.
      checkUses(ms->getSubject(), in, false);
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
    // Any other statement neither assigns a field nor returns: only what it
    // reads is checked.
    if (auto *es = ast::dyn_cast<ast::ExprStmt>(s)) {
      checkUses(es->getExpr(), in, false);
    } else if (auto *as = ast::dyn_cast<ast::AssignStmt>(s)) {
      checkUses(as->getValue(), in, false);
    } else if (auto *ds = ast::dyn_cast<ast::DeclStmt>(s)) {
      if (auto *vd = ast::dyn_cast<ast::VarDecl>(ds->getDecl()))
        checkUses(vd->getInitExpr(), in, false);
    } else if (auto *sa = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
      checkWrittenIn(sa->getArray(), in);
      checkUses(sa->getIndex(), in, false);
      checkUses(sa->getValue(), in, false);
    } else if (auto *d = ast::dyn_cast<ast::DestructureStmt>(s)) {
      checkUses(d->getValue(), in, false);
    }
    return {in, false};
  };

  // An optional field (`next: Node?`) is implicitly `None` unless __init__
  // assigns it: the constructor zero-initialises every slot and a NULL box IS
  // None, so such fields start out definitely assigned.  This is what makes
  // linked structures ergonomic (see
  // docs/language/12-optionals.md).
  BitSet entry(n, false);
  for (unsigned i = 0; i < n; ++i)
    if (ast::isa<ast::OptionalType>(fields[i].second))
      entry[i] = true;
  // `__super__(...)` runs first: in its arguments nothing is assigned.
  std::vector<ast::Stmt *> stmts = body->getStatements();
  if (!stmts.empty() && isSuperInitCall(stmts.front())) {
    const auto *call = ast::cast<ast::CallExpr>(
        ast::cast<ast::ExprStmt>(stmts.front())->getExpr());
    for (const ast::Expr *arg : call->getArguments())
      checkUses(arg, entry, true);
    stmts.erase(stmts.begin());
  }
  Flow result = analyzeBlock(stmts, entry);
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
        if (!reportPrivateImport(superName, cd->getLocation()) &&
            !isFailedImportUse(superName))
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
    FixedArrayAllowed = true; // a field may be a fixed-size array
    auto *fty = resolveValueType(field->getType(), field->getLocation(),
                                 "field '" + field->getName() + "' in class '" +
                                     cd->getName() + "'");
    if (!fty) {
      ok = false;
      continue;
    }
    field->setType(fty); // canonical write-back for the lowering
    if (field->isWeak() &&
        !checkWeakType(fty, field->getLocation(),
                       "field '" + field->getName() + "' of class '" +
                           cd->getName() + "'")) {
      ok = false;
      continue;
    }
    ct->addField(field->getName(), fty, field->isWeak());
    // A public field of a public class cannot hand an importer a type it
    // could not name (an instantiation's fields are the template's).
    if (!names::isPrivateName(field->getName()) &&
        !isModuleOwnName(cd->getName()) &&
        cd->getName().find('<') == std::string::npos &&
        !checkPublicSurface("field '" + field->getName() + "' of '" +
                                cd->getName() + "' is public but it",
                            "field", fty, field->getLocation()))
      ok = false;
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
    // __init__ is a per-class constructor, not a virtual override.
    const ast::MethodDecl *overridden = nullptr;
    if (method->getName() != names::kMethodInit && ct->getSuperClass())
      overridden = ct->getSuperClass()->findMethod(method->getName());
    if (overridden && overridden->isImpl()) {
      error(method->getLocation(),
            "method '" + method->getName() + "' of '" +
                ct->implOwner(method->getName())->getName() +
                "' is declared in an 'impl' block and cannot be overridden");
      ok = false;
      continue;
    }
    // A method without a body is pure virtual: a vtable slot that a
    // subclass implements (no class body's method is native: every
    // frontend's grammar keeps `native fn` at the top level).  `__init__`
    // and `destroy` have no slot of their own; they are reported where they
    // are checked (rejectBodyInits, visitClassDecl).
    const bool pure = !method->getBody() &&
                      method->getName() != names::kMethodInit &&
                      method->getName() != names::kMethodDestroy;
    if (pure && overridden && !overridden->isPure()) {
      // Named by the class that implements it, not the direct superclass,
      // which may inherit it.
      const ast::ClassType *implementer =
          declaringClass(ct->getSuperClass(), method->getName());
      error(method->getLocation(),
            "method '" + method->getName() + "' of class '" + cd->getName() +
                "' has no body, but its superclass '" +
                displayName(implementer->getName()) +
                "' implements it: an implemented method cannot become pure "
                "virtual again");
      ok = false;
      continue;
    }
    if (auto *mdecl = declareMethod(
            method, ct, overridden,
            pure ? ast::MethodDecl::Pure : ast::MethodDecl::None, ok))
      ct->addMethod(mdecl);
    else
      ok = false;
  }
  if (!populateImplMethods(cd, ct))
    ok = false;
  if (!ct->findMethod(names::kMethodInit) && !declareDefaultInit(cd, ct))
    ok = false;
  // The vtable is complete: whether a slot is still pure.
  ct->updateAbstract();
  PopulatedClasses.insert(cd->getName());
  return ok;
}

std::string Sema::paramList(const ast::FuncDecl *fn) {
  std::string out;
  for (const auto &p : fn->getParams())
    out += (out.empty() ? "" : ", ") + p.getName() + ": " +
           ast::typeName(p.ParamType);
  return out;
}

bool Sema::doesNothing(const ast::ClassType *ct) const {
  for (; ct && ct != Ctx.getObjTy(); ct = ct->getSuperClass())
    if (!DefaultInits.count(ct->getName()) || ct->getNumFields())
      return false;
  return true;
}

bool Sema::declareDefaultInit(ast::ClassDecl *cd, ast::ClassType *ct) {
  const ast::SourceLocation loc = cd->getLocation();
  StringSet used;
  for (auto &[name, _] : ct->getFields())
    used.insert(name);
  std::vector<ast::Param> params;
  std::vector<ast::Stmt *> body;

  // The superclass constructor's arguments, named as it names them where
  // that is free (a field of this class may take the name).
  ast::ClassType *super = ct->getSuperClass();
  if (const ast::MethodDecl *superInit =
          super && super != Ctx.getObjTy()
              ? super->findMethod(names::kMethodInit)
              : nullptr) {
    const ast::ParamModes &modes = superInit->getParamModes();
    std::vector<ast::Expr *> args;
    for (size_t i = 0; i < superInit->getNumParams(); ++i) {
      std::string name = modes.name(i);
      if (name.empty())
        name = "arg" + std::to_string(i + 1);
      while (!used.insert(name).second)
        name += "_";
      const std::string &interned = Ctx.intern(name);
      params.push_back(
          {&interned, superInit->getParamTypes()[i], modes.mode(i)});
      args.push_back(Ctx.make<ast::Identifier>(loc, interned));
    }
    body.push_back(Ctx.make<ast::ExprStmt>(
        loc, Ctx.make<ast::CallExpr>(loc, Ctx.intern(names::kMethodSuper),
                                     std::move(args))));
  }
  for (auto &[name, type] : ct->getFields()) {
    const std::string &interned = Ctx.intern(name);
    params.push_back({&interned, type});
    body.push_back(Ctx.make<ast::MemberAssignStmt>(
        loc, Ctx.make<ast::Identifier>(loc, Ctx.intern(names::kSelf)), interned,
        Ctx.make<ast::Identifier>(loc, interned)));
  }

  auto *fn = Ctx.make<ast::FuncDecl>(
      loc, Ctx.intern(names::kMethodInit), std::move(params), nullptr,
      Ctx.make<ast::CompoundStmt>(loc, std::move(body)));
  bool ok = true;
  DeclaringDefaultInit = true; // its parameters are the fields, checked
  auto *md = declareMethod(fn, ct, nullptr, ast::MethodDecl::None, ok);
  DeclaringDefaultInit = false;
  if (!md)
    return false;
  ct->addMethod(md);
  cd->addMethod(fn); // checked and lowered like a written one
  DefaultInits[cd->getName()] = fn;
  return ok;
}

ast::MethodDecl *Sema::declareMethod(ast::FuncDecl *method, ast::Type *owner,
                                     const ast::MethodDecl *overridden,
                                     uint8_t flags, bool &ok) {
  const std::string className = typeName(owner);
  struct SelfGuard {
    ast::Type *&Slot, *Saved;
    ~SelfGuard() { Slot = Saved; }
  } selfGuard{SelfType, SelfType};
  SelfType = owner;
  // A method's result type is written, `fn get() -> int => self.n;`: one
  // with an expression and none is reported (a template's once, by
  // rejectInferredMethods), as a result type that does not resolve is.
  if (method->hasInferredResult()) {
    if (className.find('<') == std::string::npos)
      reportInferredMethod(method);
    else
      ++SuppressedFollowOns;
    return nullptr;
  }
  // An initializer sets up `self`; constructing gives the object (or the
  // struct), so there is nothing for it to return.
  if (method->getName() == names::kMethodInit && method->getReturnType()) {
    error(method->getLocation(),
          std::string("'") +
              (method->isModulePrivateInit() ? names::kMethodInitModulePrivate
                                             : names::kMethodInit) +
              "' cannot have a result type: it sets up 'self' and returns "
              "nothing");
    return nullptr;
  }
  // Resolve return type.
  ast::Type *retTy = Ctx.getVoidTy();
  if (method->getReturnType()) {
    FixedArrayAllowed = true; // a result may be a fixed-size array
    retTy = resolveType(method->getReturnType(), method->getLocation(),
                        "method '" + method->getName() + "' in '" + className +
                            "'");
    if (!retTy || !checkFixedResult(retTy, method->getResultMode(),
                                    method->getLocation()))
      return nullptr;
    method->setReturnType(retTy);
  }
  // Resolve parameter types.
  if (!checkDistinctParams(method, "method '" + method->getName() + "' of '" +
                                       className + "'"))
    return nullptr;
  std::vector<ast::Type *> paramTys;
  for (auto &p : method->getMutableParams()) {
    // `self` is the implicit receiver of every method; a parameter of the
    // same name would silently shadow it in the body scope.
    if (p.getName() == names::kSelf) {
      error(method->getLocation(),
            "'" + std::string(names::kSelf) +
                "' cannot be used as a parameter name of method '" +
                method->getName() + "' in " + typeKindWord(owner) + " '" +
                className + "'; it is the implicit receiver");
      return nullptr;
    }
    if (!checkBinderName(p.getName(), method->getLocation(),
                         /*binds=*/!DeclaringDefaultInit))
      return nullptr;
    FixedArrayAllowed = true; // a parameter may be a fixed-size array
    SliceAllowed = p.Mode != ast::ParamMode::Value; // or a slice
    auto *pty =
        resolveValueType(p.ParamType, method->getLocation(),
                         "parameter '" + p.getName() + "' of method '" +
                             method->getName() + "' in '" + className + "'");
    if (!pty)
      return nullptr;
    p.ParamType = pty;
    paramTys.push_back(pty);
  }

  // A public method of a public type cannot hand an importer a type it
  // could not name (an instantiation's methods are the template's).
  if (!DeclaringDefaultInit && !names::isPrivateName(method->getName()) &&
      !isModuleOwnName(className) && className.find('<') == std::string::npos) {
    const std::string who = "method '" + method->getName() + "' of '" +
                            className + "' is public but ";
    bool pub = checkPublicSurface(who + "its result", "method", retTy,
                                  method->getLocation());
    for (size_t i = 0; i < paramTys.size(); ++i)
      pub = checkPublicSurface(who + "parameter '" +
                                   method->getParams()[i].getName() + "'",
                               "method", paramTys[i], method->getLocation()) &&
            pub;
    if (!pub)
      return nullptr;
  }

  // -- override signature check
  // An override must have an identical signature: same parameter types and
  // return type.  Strict equality is enforced (no covariance).
  if (overridden) {
    auto *ct = ast::cast<ast::ClassType>(owner); // only a class overrides
    if (!checkOverrideModes(method, overridden))
      ok = false;
    const auto &baseParams = overridden->getParamTypes();
    bool sigMatches = typesEqual(overridden->getReturnType(), retTy) &&
                      baseParams.size() == paramTys.size();
    for (size_t i = 0; sigMatches && i < paramTys.size(); ++i)
      sigMatches = typesEqual(baseParams[i], paramTys[i]);
    if (!sigMatches) {
      error(method->getLocation(),
            "override of '" +
                signatureString(ct->getSuperClass(), overridden->getName(),
                                overridden->getReturnType(),
                                overridden->getParamTypes()) +
                "' has incompatible signature '" +
                signatureString(ct, method->getName(), retTy, paramTys) + "'");
      ok = false;
    }
  }

  // `view fn`: not on __init__, and kept by an override.
  if (!checkViewFnDecl(method, overridden))
    ok = false;
  if (!checkResultDecl(method, /*method=*/true))
    ok = false;

  if (method->isView())
    flags |= ast::MethodDecl::View;
  auto *mdecl =
      Ctx.make<ast::MethodDecl>(method->getLocation(), method->getName(), retTy,
                                std::move(paramTys), flags);
  ast::ParamModes modes = ast::paramModes(method->getParams());
  modes.Result = method->getResultMode();
  mdecl->setParamModes(std::move(modes));
  mdecl->setModulePrivateInit(method->isModulePrivateInit());
  return mdecl;
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

void Sema::reportAbstractConstruction(const ast::ClassType *ct,
                                      ast::SourceLocation loc) {
  // Each slot by the class that declares it pure.
  std::vector<std::string> missing;
  for (const ast::MethodDecl *m : ct->pureSlots())
    missing.push_back("'" +
                      displayName(ct->pureSlotOwner(m->getName())->getName()) +
                      "." + m->getName() + "'");
  std::string msg = "cannot instantiate abstract class '" +
                    displayName(ct->getName()) + "': ";
  for (size_t i = 0; i < missing.size(); ++i) {
    if (i)
      msg += i + 1 == missing.size() ? " and " : ", ";
    msg += missing[i];
  }
  msg += missing.size() == 1 ? " has no implementation"
                             : " have no implementation";
  error(loc, msg);
}

// -- impl blocks
//
// An `impl` block's methods join its class: checked like the class body's,
// but called directly instead of through the vtable, so a subclass cannot
// override one, and none may take a name the class or a superclass already
// has.  Its `static fn`s become the module's functions `Type::name`.

/// The class, from @p ct up, that introduces method @p name.
static const ast::ClassType *methodOwner(const ast::ClassType *ct,
                                         const std::string &name) {
  while (ct->getSuperClass() && ct->getSuperClass()->findMethod(name))
    ct = ct->getSuperClass();
  return ct;
}

bool Sema::populateImplMethods(ast::ClassDecl *cd, ast::ClassType *ct) {
  auto it = ImplsByClass.find(cd->getName());
  if (it == ImplsByClass.end())
    return true;
  bool ok = true;
  for (auto *im : it->second)
    for (auto *fn : im->getMethods()) {
      if (fn->isStatic())
        continue;
      const std::string &name = fn->getName();
      // __init__ is the same here as in the class body: the class's own,
      // never inherited or called through the vtable.
      const bool init = name == names::kMethodInit;
      if (ct->findMethod(name)) {
        const ast::ClassType *owner = init ? ct : methodOwner(ct, name);
        error(fn->getLocation(),
              "class '" + cd->getName() + "' already has a method '" + name +
                  "'" +
                  (owner == ct ? "" : " (from '" + owner->getName() + "')"));
        ok = false;
        continue;
      }
      if (auto *md = declareMethod(
              fn, ct, nullptr,
              init ? ast::MethodDecl::None : ast::MethodDecl::Impl, ok)) {
        ct->addMethod(md);
        cd->addMethod(fn); // checked and lowered with the class body
      } else {
        ok = false;
      }
    }
  return ok;
}

bool Sema::rejectBodyInits(ast::TranslationUnit *tu) {
  bool ok = true;
  for (auto *cds : {&tu->getClassDecls(), &tu->getGenericClassDecls()})
    for (auto *cd : *cds)
      for (auto *m : cd->getMethods()) {
        if (m->getName() != names::kMethodInit)
          continue;
        std::string impl = cd->getName();
        const auto &params = cd->getTypeParams();
        for (size_t i = 0; i < params.size(); ++i)
          (impl += i ? ", " : "<") += *params[i];
        if (!params.empty())
          impl += ">";
        std::string msg = "'";
        msg += names::kMethodInit;
        msg += "' goes in an 'impl' block, not the class body: write 'impl ";
        msg += impl;
        msg += " { fn ";
        msg += names::kMethodInit;
        msg += "(...) { ... } }'";
        error(m->getLocation(), msg);
        ok = false;
      }
  return ok;
}

bool Sema::checkImplDecls(ast::TranslationUnit *tu) {
  StringSet localTypes;
  for (auto *cd : tu->getClassDecls())
    localTypes.insert(cd->getName());
  for (auto *sd : tu->getStructDecls())
    localTypes.insert(sd->getName());
  bool ok = true;
  for (auto *im : tu->getImplDecls()) {
    const std::string &name = im->getTypeName();
    // A generic class's or struct's, checked in registerGenericTemplates.
    if (auto t = ClassTemplates.find(name);
        t != ClassTemplates.end() && !foreignOrigin(t->second))
      continue;
    if (auto t = StructTemplates.find(name);
        t != StructTemplates.end() && !foreignOrigin(t->second))
      continue;
    if (ErroneousNames.count(name)) {
      ok = false; // a dropped class was reported with its declaration
      continue;
    }
    auto *ct = Ctx.lookupClassType(name);
    auto *st = Ctx.lookupStructType(name);
    ast::Type *target = ct ? static_cast<ast::Type *>(ct) : st;
    ast::Type *named = Ctx.lookupType(name);
    std::string why;
    if ((ct && ct->isBuiltin()) || (named && ast::isa<ast::BuiltinType>(named)))
      why = "'" + name +
            "' is a built-in type; an 'impl' block cannot add "
            "methods to it";
    else if ((target && !localTypes.count(name)) ||
             ClassTemplates.count(name) || StructTemplates.count(name))
      why = std::string("an 'impl' block for a ") +
            (st || StructTemplates.count(name) ? "struct" : "class") +
            " of another module is not supported yet";
    else if (Ctx.lookupEnumType(name) || EnumTemplates.count(name))
      why = "an 'impl' block for an enum is not supported yet";
    else if (target && !im->getTypeParams().empty()) {
      why = "'" + name + "' is not a generic " + typeKindWord(target) +
            ": write 'impl ";
      why += name + "'";
    } else if (!target && !isFailedImportUse(name))
      why = "'impl' for undefined class '" + name + "'";
    if (!why.empty())
      error(im->getLocation(), why);
    if (!target || !why.empty()) {
      ok = false;
      continue;
    }
    if (im->getTrait()) {
      if (!checkTraitImpl(im, target))
        ok = false;
      continue;
    }
    for (auto *fn : im->getMethods()) {
      if (!fn->isStatic())
        continue;
      if (methodOf(target, fn->getName())) {
        error(fn->getLocation(), std::string(typeKindWord(target)) + " '" +
                                     name + "' already has a method '" +
                                     fn->getName() + "'");
        ok = false;
        continue;
      }
      fn->setName(Ctx.intern(name + names::kQualSep + fn->getName()));
      tu->addFuncDecl(fn);
    }
  }
  ImplsChecked = true;
  for (const auto &p : PendingTraitImpls) {
    InstantiationStack.push_back(p.Request);
    OriginSource inOrigin(*this, p.Origin);
    if (!checkTraitImpl(p.Impl, p.Type))
      ok = false;
    InstantiationStack.pop_back();
  }
  PendingTraitImpls.clear();
  return ok;
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
    // A pure virtual method has no body to check (a bodiless `__init__`
    // was reported by rejectBodyInits).
    if (!method->getBody())
      continue;

    // Determine whether this __init__ must call __super__.  A derived
    // __init__ must call __super__(...) as its first statement whenever its
    // superclass (other than the implicit Obj root) declares an __init__ at
    // all — even a zero-parameter one — so the base initializer always runs.
    classCtx.SuperInitRequired = false;
    classCtx.SuperInitCalled = false;
    if (method->getName() == names::kMethodInit && ct->getSuperClass() &&
        ct->getSuperClass() != Ctx.getObjTy()) {
      auto *superInit = ct->getSuperClass()->findMethod(names::kMethodInit);
      if (superInit && !doesNothing(ct->getSuperClass()))
        classCtx.SuperInitRequired = true;
    }

    // Resolve return type (already validated in Phase 4; needed for body).
    ast::Type *retTy = Ctx.getVoidTy();
    if (method->getReturnType()) {
      FixedArrayAllowed = true;
      retTy = resolveType(method->getReturnType(), method->getLocation(),
                          "method '" + method->getName() + "'");
    }

    // Resolve parameter types.
    std::vector<ast::Type *> paramTys;
    for (auto &p : method->getParams()) {
      FixedArrayAllowed = true;
      auto *pty = resolveType(p.ParamType, method->getLocation(),
                              "parameter '" + p.getName() + "'");
      // Unreachable in practice (a class with an unresolvable parameter type
      // is erroneous and its bodies are skipped); poisoned if it ever is.
      paramTys.push_back(pty ? pty : Ctx.getPoisonTy());
    }
    auto *savedRetTy = CurrentReturnType;
    const ResultContext savedResult = CurrentResult;
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

      bool bodyOk = visitBody(method->getBody()->getStatements());
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
            else {
              const std::string &super = ct->getSuperClass()->getName();
              std::string msg = "'";
              msg += names::kMethodInit;
              msg += "' in class '" + node->getName() + "' must call '";
              msg += names::kMethodSuper;
              msg += "' as its first statement because its superclass '";
              msg += super + "' ";
              if (auto d = DefaultInits.find(super); d != DefaultInits.end()) {
                msg += "has fields to initialize (its default '";
                msg += names::kMethodInit;
                msg += "' takes " + paramList(d->second) + ")";
              } else {
                msg += "has an '";
                msg += names::kMethodInit;
                msg += "'";
              }
              error(method->getLocation(), msg);
            }
            ok = false;
          }
        }

        // Definite-assignment: every own field must be set on every path out
        // of __init__.  Run after the body type-checks so subject types (used
        // for match-exhaustiveness) are resolved.
        if (method->getName() == names::kMethodInit) {
          if (!checkInitFieldsAssigned(ct->getFields(), method->getBody(),
                                       method->getLocation(), "class",
                                       node->getName()))
            ok = false;
        }
      }
    }
    CurrentReturnType = savedRetTy;
    CurrentResult = savedResult;
  }

  CurrentClassCtx = savedClassCtx;
  return ok;
}

// -- Generics: templates, instantiation, type-argument inference
//
// See the comment block in Sema.h ("Generics") and
// docs/language/13-generics.md for the design.  In short: a generic declaration
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
  // Every template's name, whatever the declaration order: a template is
  // a type a parameter must not shadow, before and after its registration.
  StringSet templateNames;
  for (auto *cd : tu->getGenericClassDecls())
    templateNames.insert(cd->getName());
  for (auto *sd : tu->getGenericStructDecls())
    templateNames.insert(sd->getName());
  for (auto *ed : tu->getGenericEnumDecls())
    templateNames.insert(ed->getName());

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
          ClassTemplates.count(*p) || StructTemplates.count(*p) ||
          EnumTemplates.count(*p) || templateNames.count(*p)) {
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

  for (auto *sd : tu->getGenericStructDecls()) {
    if (StructTemplates.count(sd->getName())) {
      error(sd->getLocation(),
            "redefinition of generic struct '" + sd->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(sd->getName(), sd->getLocation(),
                                DeclKind::Struct)) {
      ok = false;
      continue;
    }
    if (!checkTypeParams(sd->getTypeParams(), "generic struct", sd->getName(),
                         sd->getLocation())) {
      ok = false;
      continue;
    }
    StructTemplates[sd->getName()] = sd;
  }

  for (auto *ed : tu->getGenericEnumDecls()) {
    if (EnumTemplates.count(ed->getName())) {
      error(ed->getLocation(),
            "redefinition of generic enum '" + ed->getName() + "'");
      ok = false;
      continue;
    }
    if (!checkDeclNameAvailable(ed->getName(), ed->getLocation(),
                                DeclKind::Enum) ||
        !checkTypeParams(ed->getTypeParams(), "generic enum", ed->getName(),
                         ed->getLocation())) {
      ok = false;
      continue;
    }
    // Its variants are its instantiations': checked once, here (a repeated
    // one is left out of them, as visitEnumDecl leaves it out of an enum).
    StringSet seen;
    for (auto *v : ed->getVariants())
      if (!seen.insert(*v).second) {
        error(ed->getLocation(),
              "duplicate variant '" + *v + "' in enum '" + ed->getName() + "'");
        ok = false;
      }
    EnumTemplates[ed->getName()] = ed;
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
    if (!checkDistinctParams(fn, "function '" + fn->getName() + "'")) {
      ErroneousNames.insert(fn->getName()); // calls are follow-ons
      ok = false;
      continue;
    }
    if (fn->isNative()) {
      std::vector<ast::Type *> annotations;
      for (const ast::Param &p : fn->getParams())
        annotations.push_back(p.ParamType);
      if (!checkNativeParamModes(fn, annotations)) {
        ErroneousNames.insert(fn->getName()); // calls are follow-ons
        ok = false;
        continue;
      }
    }
    FuncTemplates[fn->getName()] = fn;
  }

  // The `impl` blocks of the generic classes and structs: each
  // instantiation gets a copy (instantiateClass, instantiateStruct).  One of
  // another module's is checkImplDecls's to reject.
  for (auto *im : tu->getImplDecls()) {
    const std::string &name = im->getTypeName();
    const ast::Decl *tmpl = nullptr;
    const std::vector<const std::string *> *want = nullptr;
    const char *kind = "class";
    if (auto t = ClassTemplates.find(name); t != ClassTemplates.end()) {
      tmpl = t->second;
      want = &t->second->getTypeParams();
    } else if (auto s = StructTemplates.find(name);
               s != StructTemplates.end()) {
      tmpl = s->second;
      want = &s->second->getTypeParams();
      kind = "struct";
    }
    if (!tmpl || foreignOrigin(tmpl))
      continue;
    const auto &params = im->getTypeParams();
    ast::SourceLocation loc = im->getLocation();
    std::string why;
    if (params.empty()) {
      why = "'";
      why += name;
      why += "' is a generic ";
      why += kind;
      why += ": write 'impl ";
      why += name;
      for (size_t i = 0; i < want->size(); ++i)
        (why += i ? ", " : "<") += *(*want)[i];
      why += ">'";
    } else if (params.size() != want->size()) {
      why = "'impl ";
      why += name;
      why += "' names " + std::to_string(params.size());
      why += " type parameter(s); generic ";
      why += kind;
      why += " '";
      why += name;
      why += "' has " + std::to_string(want->size());
    }
    if (!why.empty())
      error(loc, why);
    if (!why.empty() ||
        !checkTypeParams(params, "'impl' block for", name, im->getLocation())) {
      ok = false;
      continue;
    }
    TemplateImpls[tmpl].push_back(im);
  }
  return ok;
}

// static
std::string Sema::instantiationName(const std::string &templateName,
                                    const std::vector<ast::Type *> &args) {
  // `Box<int>`, `Pair<Str, int>`, `Box<Str?>`, `Box<Box<int>>` -- the same
  // spelling ast::typeName produces for the instantiation afterwards, so the
  // name round-trips through module export/import and reads naturally in
  // diagnostics.  The arguments are canonical, so equal spellings mean equal
  // types and the name is a valid cache key.  LLVM symbol names may contain
  // any character (they are quoted in textual IR), so `Box<int>_vtable` is a
  // legal global.
  std::string s = templateName + "<";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i)
      s += ", ";
    s += ast::typeName(args[i]);
  }
  return s + ">";
}

const Sema::ModuleTemplates *Sema::foreignOrigin(const ast::Decl *tmpl) const {
  auto it = ForeignTemplates.find(tmpl);
  return it == ForeignTemplates.end() ? nullptr : it->second;
}

Sema::OriginSource::OriginSource(Sema &s, const ModuleTemplates *origin)
    : S(s), SavedOrigin(s.CurrentOrigin), SavedName(s.Diags.getSourceName()),
      SavedLines(s.Diags.getSourceLines()), Active(origin != nullptr) {
  S.CurrentOrigin = origin;
  if (Active)
    S.Diags.setSourceInfo(origin->File, origin->Lines);
}

Sema::OriginSource::~OriginSource() {
  S.CurrentOrigin = SavedOrigin;
  if (Active)
    S.Diags.setSourceInfo(SavedName, SavedLines);
}

Sema::InstantiationFrame Sema::frameAt(const std::string &name,
                                       ast::SourceLocation loc) {
  return {name, loc, Diags.getSourceName(), Diags.getSourceLines()};
}

ast::ClassType *Sema::instantiateClass(const std::string &name,
                                       const std::vector<ast::Type *> &args,
                                       ast::SourceLocation loc) {
  auto tIt = ClassTemplates.find(name);
  if (tIt == ClassTemplates.end()) {
    if (ErroneousNames.count(name) || isFailedImportUse(name))
      ++SuppressedFollowOns; // reported with its declaration or its import
    else if (Ctx.lookupClassType(name))
      error(loc, "'" + name +
                     "' is not a generic class and takes no type "
                     "arguments");
    else if (Ctx.lookupEnumType(name))
      error(loc, "'" + name +
                     "' is not a generic enum and takes no type "
                     "arguments");
    else if (Ctx.lookupStructType(name))
      error(loc, "'" + name +
                     "' is not a generic struct and takes no type "
                     "arguments");
    else if (FuncTemplates.count(name))
      error(loc, "'" + name + "' is a generic function, not a generic class");
    else if (Traits.count(name))
      error(loc, "'" + name + "' is a trait, not a type");
    else if (!reportPrivateImport(name, loc))
      error(loc, "unknown generic class '" + name + "'");
    return nullptr;
  }
  ast::ClassDecl *tmpl = tIt->second;
  if (!checkTypeArgs(name, args, loc))
    return nullptr;
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc, "generic class '" + name + "' expects " +
                   std::to_string(tmpl->getTypeParams().size()) +
                   " type argument(s), got " + std::to_string(args.size()));
    return nullptr;
  }
  if (!checkBounds(tmpl, name, tmpl->getTypeParams(),
                   tmpl->getTypeParamBounds(), args, loc))
    return nullptr;

  // Class names are global across imports, so an instantiation is named by
  // its template's own name, however it was spelled here.
  const ModuleTemplates *origin = foreignOrigin(tmpl);
  const std::string instName = instantiationName(tmpl->getName(), args);
  if (auto cIt = ClassInstantiations.find(instName);
      cIt != ClassInstantiations.end()) {
    // Cached (nullptr records an earlier failure) -- unless it is another
    // template's of the same name.
    auto info = cIt->second ? ClassInstantiationInfo.find(cIt->second)
                            : ClassInstantiationInfo.end();
    if (info == ClassInstantiationInfo.end() || info->second.Template == tmpl)
      return cIt->second;
    error(loc, "instantiation '" + instName + "' of '" + displayName(name) +
                   "' conflicts with another template's instantiation of the "
                   "same name (type names are global across imports)");
    return nullptr;
  }

  if (auto *existing = Ctx.lookupClassType(instName)) {
    // An imported module exported `Box<int>` as a concrete class: the very
    // instantiation, when the template is that module's (or one it reached
    // through the same template's module).
    auto o = ImportedTypeOrigins.find(instName);
    if (origin && o != ImportedTypeOrigins.end() &&
        o->second == origin->Module) {
      // Its constructor goes by the class's name too.
      if (!lookupFunction(instName)) {
        auto *init = existing->findMethod(names::kMethodInit);
        declareFunction(instName, existing,
                        init ? init->getParamTypes()
                             : std::vector<ast::Type *>{},
                        /*isBuiltin=*/true);
        if (init)
          FunctionTable[instName].Modes = init->getParamModes();
      }
      ClassInstantiationInfo[existing] = InstantiationInfo{tmpl, args};
      return ClassInstantiations[instName] = existing;
    }
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported class of the same name "
                   "(type names are global across imports)");
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (Ctx.lookupType(instName)) {
    // A struct (or another type) of that name, from a template of another
    // module or declared outright: the same rule as for a struct template.
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported type of the same name "
                   "(type names are global across imports)");
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (instantiationDepth() >= kMaxInstantiationDepth) {
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
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  ast::ClassDecl *clone = cloner.cloneClassDecl(tmpl, instName);
  // And its `impl` blocks, each by its own names for the type parameters.
  std::vector<ast::ImplDecl *> &impls = ImplsByClass[instName];
  if (auto it = TemplateImpls.find(tmpl); it != TemplateImpls.end())
    for (auto *im : it->second) {
      Subst s;
      for (size_t i = 0; i < args.size(); ++i)
        s[*im->getTypeParams()[i]] = args[i];
      ast::ASTCloner c(Ctx, std::move(s), nullptr, origin != nullptr);
      impls.push_back(c.cloneImplDecl(im, instName));
    }

  const InstantiationFrame request = frameAt(instName, loc);
  InstantiationStack.push_back(request);
  OriginSource inOrigin(*this, origin);
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
  ClassInstantiationInfo[ct] = InstantiationInfo{tmpl, args};
  // Another module's template makes a class of that module: so its
  // importers see it, wherever it was instantiated.
  if (origin)
    ImportedTypeOrigins[instName] = origin->Module;

  if (!populateClassType(clone, ct))
    ok = false;
  if (ok)
    declareConstructor(clone, ct);
  for (auto *im : ImplsByClass[instName]) {
    if (!ok || !im->getTrait())
      continue;
    if (!ImplsChecked)
      PendingTraitImpls.push_back({im, ct, request, origin});
    else if (!checkTraitImpl(im, ct))
      ok = false;
  }
  InstantiationStack.pop_back();

  if (!ok) {
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }

  PendingInstantiation pending;
  pending.Class = clone;
  pending.Name = instName;
  pending.Depth = instantiationDepth() + 1;
  pending.RequestLoc = loc;
  pending.Origin = origin;
  pending.RequestFile = request.File;
  pending.RequestLines = request.Lines;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  PendingInstantiations.push_back(pending);
  (origin ? ForeignInstantiatedClassDecls : InstantiatedClassDecls)
      .push_back(clone);
  declareInstantiationStatics(instName, ct, pending);
  return ct;
}

void Sema::declareInstantiationStatics(const std::string &instName,
                                       ast::Type *type,
                                       const PendingInstantiation &base) {
  auto it = ImplsByClass.find(instName);
  if (it == ImplsByClass.end())
    return;
  // Declared in the template's module, as the bodies are checked.
  InstantiationStack.push_back(
      {base.Name, base.RequestLoc, base.RequestFile, base.RequestLines});
  OriginSource inOrigin(*this, base.Origin);
  for (auto *im : it->second) {
    if (im->getTrait())
      continue;
    for (auto *fn : im->getMethods()) {
      if (!fn->isStatic())
        continue;
      const std::string name = instName + names::kQualSep + fn->getName();
      if (methodOf(type, fn->getName())) {
        error(fn->getLocation(), std::string(typeKindWord(type)) + " '" +
                                     instName + "' already has a method '" +
                                     fn->getName() + "'");
        ErroneousNames.insert(name); // its calls are follow-ons
        continue;
      }
      fn->setName(Ctx.intern(name));
      if (!declareFunctionSignature(fn))
        continue; // ErroneousNames has it
      // Another module's template's: the instantiation's, not this
      // module's to export (the first module to need it emits it).
      if (base.Origin)
        ImportedTypeOrigins[fn->getName()] = base.Origin->Module;
      PendingInstantiation p = base;
      p.Class = nullptr;
      p.Struct = nullptr;
      p.Func = fn;
      queueInstantiation(std::move(p));
      (base.Origin ? ForeignInstantiatedFuncDecls : InstantiatedFuncDecls)
          .push_back(fn);
    }
  }
  InstantiationStack.pop_back();
}

std::string Sema::instantiateFunction(const std::string &name,
                                      const std::vector<ast::Type *> &args,
                                      ast::SourceLocation loc) {
  auto tIt = FuncTemplates.find(name);
  if (tIt == FuncTemplates.end()) {
    if (ErroneousNames.count(name) || isFailedImportUse(name))
      ++SuppressedFollowOns; // reported with its declaration or its import
    else if (lookupFunction(name))
      error(loc, "'" + name +
                     "' is not a generic function and takes no type "
                     "arguments");
    else
      error(loc, "unknown generic function '" + name + "'");
    return "";
  }
  ast::FuncDecl *tmpl = tIt->second;
  if (!checkTypeArgs(name, args, loc))
    return "";
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc, "generic function '" + name + "' expects " +
                   std::to_string(tmpl->getTypeParams().size()) +
                   " type argument(s), got " + std::to_string(args.size()));
    return "";
  }
  if (!checkBounds(tmpl, name, tmpl->getTypeParams(),
                   tmpl->getTypeParamBounds(), args, loc))
    return "";

  // Function names are per module: another module's template is
  // instantiated under that module's hidden qualifier.
  const ModuleTemplates *origin = foreignOrigin(tmpl);
  std::string instName = origin ? hiddenQualifier(origin->Module) +
                                      names::kQualSep +
                                      instantiationName(tmpl->getName(), args)
                                : instantiationName(name, args);
  if (auto fIt = FuncInstantiations.find(instName);
      fIt != FuncInstantiations.end())
    return fIt->second ? instName : "";

  if (!origin && lookupFunction(instName)) {
    error(loc, "instantiation '" + instName +
                   "' conflicts with an imported function of the same name");
    FuncInstantiations[instName] = false;
    return "";
  }
  // The template's module instantiated it with these arguments itself, and
  // exported the instantiation (an imported function under the module's
  // hidden qualifier): that one is called, as an imported concrete
  // instantiation of a struct is used (instantiateStruct).  A native one is
  // made here, as it crosses into C by its template's rules.
  if (const FunctionSig *sig = origin ? lookupFunction(instName) : nullptr;
      sig && sig->IsBuiltin && !tmpl->isNative()) {
    FuncInstantiations[instName] = true;
    return instName;
  }
  if (instantiationDepth() >= kMaxInstantiationDepth) {
    error(loc, "instantiating '" + instName +
                   "' exceeds the maximum instantiation depth (" +
                   std::to_string(kMaxInstantiationDepth) + ")");
    FuncInstantiations[instName] = false;
    return "";
  }

  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  ast::FuncDecl *clone = cloner.cloneFuncDecl(tmpl, instName);
  clone->setTypeArgs(args);
  if (tmpl->isNative()) {
    // What the template typed by a bare type parameter crosses by address.
    auto bare = [&](ast::Type *t) {
      auto *named = t ? ast::dyn_cast<ast::ClassType>(t) : nullptr;
      for (auto *p : tmpl->getTypeParams())
        if (named && *p == named->getName())
          return true;
      return false;
    };
    std::vector<bool> params;
    for (const ast::Param &p : tmpl->getParams())
      params.push_back(bare(p.ParamType));
    clone->setByAddress(std::move(params), bare(tmpl->getReturnType()));
  }

  const InstantiationFrame request = frameAt(instName, loc);
  InstantiationStack.push_back(request);
  bool ok;
  {
    OriginSource inOrigin(*this, origin);
    ok = declareFunctionSignature(clone);
  }
  InstantiationStack.pop_back();
  FuncInstantiations[instName] = ok;
  if (!ok)
    return "";

  PendingInstantiation pending;
  pending.Func = clone;
  pending.Name = instName;
  pending.Depth = instantiationDepth() + 1;
  pending.RequestLoc = loc;
  pending.Origin = origin;
  pending.RequestFile = request.File;
  pending.RequestLines = request.Lines;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  queueInstantiation(std::move(pending));
  (origin ? ForeignInstantiatedFuncDecls : InstantiatedFuncDecls)
      .push_back(clone);
  return instName;
}

void Sema::queueInstantiation(PendingInstantiation p) {
  if (p.Func && p.Func->hasInferredResult())
    if (auto it = InferredFunctions.find(p.Func->getName());
        it != InferredFunctions.end())
      it->second.Instance = std::make_shared<PendingInstantiation>(p);
  PendingInstantiations.push_back(std::move(p));
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
    // A fixed-size array or a slice passed for a slice (`view T[]`) gives
    // its element type.
    if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(actual))
      return unifyTypes(at->getElementType(), ft->getElementType(), typeParams,
                        bindings, conflict);
    if (auto *st = ast::dyn_cast<ast::SliceType>(actual))
      return unifyTypes(at->getElementType(), st->getElementType(), typeParams,
                        bindings, conflict);
    return true;
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

  if (auto *ft = ast::dyn_cast<ast::FunctionType>(pattern)) {
    // `(view T) -> U` against a function type of the same arity and modes:
    // unify the parameters and the result pairwise (a lambda argument's
    // result type gives U).  A mismatch is left to the argument check.
    auto *aft = ast::dyn_cast<ast::FunctionType>(actual);
    if (!aft || aft->getNumParams() != ft->getNumParams())
      return true;
    for (size_t i = 0; i < ft->getNumParams(); ++i) {
      if (aft->getMode(i) != ft->getMode(i))
        return true;
      if (!unifyTypes(ft->getParamType(i), aft->getParamType(i), typeParams,
                      bindings, conflict))
        return false;
    }
    return unifyTypes(ft->getReturnType(), aft->getReturnType(), typeParams,
                      bindings, conflict);
  }

  if (auto *gt = ast::dyn_cast<ast::GenericType>(pattern)) {
    // `Pair<T>` against a struct: an instantiation of the same template
    // unifies argument-wise (a struct has no ancestors).
    if (auto stmpl = StructTemplates.find(gt->getName());
        stmpl != StructTemplates.end()) {
      auto *st = ast::dyn_cast<ast::StructType>(actual);
      const StructInstantiationInfo *info =
          st ? structInstantiationInfo(st) : nullptr;
      if (!info || info->Template != stmpl->second ||
          info->Args.size() != gt->getNumArgs())
        return true;
      for (size_t i = 0; i < gt->getNumArgs(); ++i)
        if (!unifyTypes(gt->getArgs()[i], info->Args[i], typeParams, bindings,
                        conflict))
          return false;
      return true;
    }
    // `Maybe<T>` against an enum: an instantiation of the same template
    // unifies argument-wise.
    if (auto etmpl = EnumTemplates.find(gt->getName());
        etmpl != EnumTemplates.end()) {
      auto *et = ast::dyn_cast<ast::EnumType>(actual);
      const EnumInstantiationInfo *info =
          et ? enumInstantiationInfo(et) : nullptr;
      if (!info || info->Template != etmpl->second ||
          info->Args.size() != gt->getNumArgs())
        return true;
      for (size_t i = 0; i < gt->getNumArgs(); ++i)
        if (!unifyTypes(gt->getArgs()[i], info->Args[i], typeParams, bindings,
                        conflict))
          return false;
      return true;
    }
    // `Box<T>` against a class: find an instantiation of the same template
    // (however either spells it) in the argument's class or one of its
    // ancestors and unify argument-wise.
    auto tmpl = ClassTemplates.find(gt->getName());
    if (tmpl == ClassTemplates.end())
      return true;
    for (auto *c = ast::dyn_cast<ast::ClassType>(actual); c;
         c = c->getSuperClass()) {
      const InstantiationInfo *info = instantiationInfo(c);
      if (!info || info->Template != tmpl->second ||
          info->Args.size() != gt->getNumArgs())
        continue;
      for (size_t i = 0; i < gt->getNumArgs(); ++i)
        if (!unifyTypes(gt->getArgs()[i], info->Args[i], typeParams, bindings,
                        conflict))
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
                         ast::SourceLocation loc, std::vector<ast::Type *> &out,
                         const std::string &spelling, ast::Type *resultPattern,
                         ast::Type *expected) {
  const std::string hint =
      "specify the type arguments explicitly: '" +
      (spelling.empty() ? templateName + "<...>(...)" : spelling) + "'";
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
  // The ones the arguments leave open, from the type the result flows into
  // (never against the arguments: a conflict there is the argument check's).
  if (resultPattern && expected) {
    StringMap<ast::Type *> fromResult;
    InferenceConflict ignored;
    if (unifyTypes(resultPattern, expected, params, fromResult, ignored))
      for (auto &[name, ty] : fromResult)
        bindings.try_emplace(name, ty);
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
                              const std::vector<ast::Type *> &argTypes,
                              ast::Type *expected) {
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

  // A parameter type of another module's template, as this context spells
  // it (inference compares it with this context's types).
  auto here = [&](const ast::Decl *tmpl, ast::Type *ty) {
    if (!foreignOrigin(tmpl))
      return ty;
    return ast::ASTCloner(Ctx, {}, nullptr, /*foreign=*/true).cloneType(ty);
  };

  if (auto it = ClassTemplates.find(callee); it != ClassTemplates.end()) {
    ast::ClassDecl *tmpl = it->second;
    if (typeArgs.empty()) {
      // Constructor-argument inference: `Box(3)` unifies __init__'s
      // parameter types with the argument types, by the type parameters'
      // names in the class body or the `impl` block it is in.
      const ast::FuncDecl *init = nullptr;
      const std::vector<const std::string *> *params = &tmpl->getTypeParams();
      for (auto *m : tmpl->getMethods())
        if (m->getName() == names::kMethodInit)
          init = m;
      if (auto impls = TemplateImpls.find(tmpl);
          !init && impls != TemplateImpls.end())
        for (auto *im : impls->second)
          for (auto *m : im->getMethods())
            if (!im->getTrait() && m->getName() == names::kMethodInit) {
              init = m;
              params = &im->getTypeParams();
            }
      std::vector<ast::Type *> paramTypes;
      if (init) {
        for (auto &p : init->getParams())
          paramTypes.push_back(here(tmpl, p.ParamType));
      } else {
        // The default __init__: the superclass constructor's parameters (a
        // generic class's superclass is not generic), then the fields.
        if (auto *super = tmpl->hasSuperClass()
                              ? Ctx.lookupClassType(tmpl->getSuperClassName())
                              : nullptr)
          if (auto *superInit = super->findMethod(names::kMethodInit))
            paramTypes = superInit->getParamTypes();
        for (auto *f : tmpl->getFields())
          paramTypes.push_back(here(tmpl, f->getType()));
      }
      if (!inferTypeArgs(callee, *params, paramTypes, argTypes, loc, typeArgs))
        return false;
    }
    auto *ct = instantiateClass(callee, typeArgs, loc);
    if (!ct)
      return false;
    node->setCalleeName(ct->getName()); // interned by the ASTContext
    return true;
  }

  if (auto it = StructTemplates.find(callee); it != StructTemplates.end()) {
    ast::StructDecl *tmpl = it->second;
    if (typeArgs.empty()) {
      // Constructor-argument inference: `Pair(1, "a")` unifies the
      // parameters of the `__init__` of an `impl` block (by that block's
      // names for the type parameters), or the fields of the default one,
      // with the argument types.
      const ast::FuncDecl *init = nullptr;
      const std::vector<const std::string *> *params = &tmpl->getTypeParams();
      if (auto impls = TemplateImpls.find(tmpl); impls != TemplateImpls.end())
        for (auto *im : impls->second)
          for (auto *m : im->getMethods())
            if (!im->getTrait() && m->getName() == names::kMethodInit) {
              init = m;
              params = &im->getTypeParams();
            }
      std::vector<ast::Type *> paramTypes;
      if (init)
        for (auto &p : init->getParams())
          paramTypes.push_back(here(tmpl, p.ParamType));
      else
        for (auto *f : tmpl->getFields())
          paramTypes.push_back(here(tmpl, f->getType()));
      if (!inferTypeArgs(callee, *params, paramTypes, argTypes, loc, typeArgs))
        return false;
    }
    auto *st = instantiateStruct(callee, typeArgs, loc);
    if (!st)
      return false;
    node->setCalleeName(st->getName()); // interned by the ASTContext
    return true;
  }

  if (auto it = FuncTemplates.find(callee); it != FuncTemplates.end()) {
    ast::FuncDecl *tmpl = it->second;
    if (typeArgs.empty()) {
      std::vector<ast::Type *> paramTypes;
      for (auto &p : tmpl->getParams())
        paramTypes.push_back(here(tmpl, p.ParamType));
      ast::Type *result = tmpl->getReturnType();
      if (!inferTypeArgs(callee, tmpl->getTypeParams(), paramTypes, argTypes,
                         loc, typeArgs, "",
                         result ? here(tmpl, result) : nullptr, expected))
        return false;
    }
    std::string inst = instantiateFunction(callee, typeArgs, loc);
    if (inst.empty())
      return false;
    node->setCalleeName(Ctx.intern(inst));
    return true;
  }

  // `Box<int>::of(1)`: a `static fn` of the `impl` block of a class or
  // struct template is the function `Box<int>::of` of the instantiation
  // (declareInstantiationStatics).
  if (const size_t sep = callee.rfind(names::kQualSep);
      sep != std::string::npos && isTemplateStatic(callee)) {
    const std::string owner = callee.substr(0, sep);
    const std::string member = callee.substr(sep + 2);
    const bool isClass = ClassTemplates.count(owner) != 0;
    if (typeArgs.empty()) {
      error(loc, "'" + callee + "' is a function of the generic " +
                     (isClass ? "class" : "struct") + " '" + owner +
                     "': write '" + owner + "<...>::" + member +
                     "(...)' with its type arguments");
      return false;
    }
    std::string instName;
    if (isClass) {
      auto *ct = instantiateClass(owner, typeArgs, loc);
      if (!ct)
        return false;
      instName = ct->getName();
    } else {
      auto *st = instantiateStruct(owner, typeArgs, loc);
      if (!st)
        return false;
      instName = st->getName();
    }
    // Declared with the instantiation here, or imported with the one an
    // imported module made (`m::Box<int>::of`, qualified as its functions
    // are).
    std::string fn = instName + names::kQualSep + member;
    if (!lookupFunction(fn)) {
      const size_t q = owner.rfind(names::kQualSep);
      const std::string imported =
          q == std::string::npos ? "" : owner.substr(0, q + 2) + fn;
      if (imported.empty() || !lookupFunction(imported)) {
        if (ErroneousNames.count(fn))
          ++SuppressedFollowOns; // rejected with its declaration
        else
          error(loc,
                "'" + instName + "' has no static function '" + member + "'");
        return false;
      }
      fn = imported;
    }
    node->setCalleeName(Ctx.intern(fn));
    return true;
  }

  // Explicit type arguments on something that is not a template.
  if (lookupFunction(callee) || Ctx.lookupClassType(callee) ||
      Ctx.lookupStructType(callee))
    error(loc, "'" + callee + "' is not generic and takes no type arguments");
  else if (ErroneousNames.count(callee))
    ++SuppressedFollowOns; // a template rejected at its declaration
  else {
    // `q::G<int>::V(...)` names a member of `q::G`, whose import may have
    // failed or which may be private to its module.
    const size_t sep = callee.rfind(names::kQualSep);
    const std::string owner =
        sep == std::string::npos ? std::string() : callee.substr(0, sep);
    if (!isFailedImportUse(callee) &&
        (owner.empty() || !isFailedImportUse(owner)) &&
        !reportPrivateImport(callee, loc) &&
        (owner.empty() || !reportPrivateImport(owner, loc)))
      error(loc,
            "call to undeclared generic function or class '" + callee + "'");
  }
  return false;
}

bool Sema::checkPendingInstantiations() {
  bool ok = true;
  // A body may request further instantiations, which append to the list, so
  // index (not iterate) and copy the entry (the vector may reallocate).
  for (size_t i = 0; i < PendingInstantiations.size(); ++i) {
    PendingInstantiation p = PendingInstantiations[i];
    InstantiationStack.push_back(
        {p.Name, p.RequestLoc, p.RequestFile, p.RequestLines});
    const StringSet *savedParams = CurrentTypeParams;
    CurrentTypeParams = &p.TypeParams;
    const size_t savedDepth = CurrentInstantiationDepth;
    CurrentInstantiationDepth = p.Depth;
    bool r;
    if (p.Func && p.Func->hasInferredResult()) {
      // Checked where it was first used (inferResultOf), or now.
      InstantiationStack.pop_back();
      CurrentInstantiationDepth = savedDepth;
      CurrentTypeParams = savedParams;
      if (!inferResultOf(p.Func->getName(), p.RequestLoc) ||
          InferredFunctions[p.Func->getName()].State != Inference::Done)
        ok = false;
      continue;
    }
    {
      OriginSource inOrigin(*this, p.Origin);
      r = p.Class    ? visitClassDecl(p.Class)
          : p.Struct ? visitStructDecl(p.Struct)
                     : visitFuncDecl(p.Func);
    }
    CurrentInstantiationDepth = savedDepth;
    CurrentTypeParams = savedParams;
    InstantiationStack.pop_back();
    if (!r)
      ok = false;
  }
  PendingInstantiations.clear();
  return ok;
}

void Sema::injectInstantiations(ast::TranslationUnit *tu) {
  // A struct's functions are all declared before any body is emitted, and
  // a struct holds another only after it: creation order is fine.
  for (auto *sd : InstantiatedStructDecls)
    tu->addStructDecl(sd);
  InstantiatedStructDecls.clear();
  // An enum's instantiations are the module's enums (its interface exports
  // them); they have no code.
  for (auto *ed : InstantiatedEnumDecls)
    tu->addEnumDecl(ed);
  InstantiatedEnumDecls.clear();
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
