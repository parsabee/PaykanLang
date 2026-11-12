// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Semantic analysis: class declarations and method bodies.

#include "ASTClone.h"
#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

#include "StringMap.h"

#include <algorithm>
#include <functional>
#include <iterator>
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

/// The `__super__(args);` that is the first of @p stmts, or null.
const ast::CallExpr *frontSuperCall(const std::vector<ast::Stmt *> &stmts) {
  auto *es = stmts.empty() ? nullptr : ast::dyn_cast<ast::ExprStmt>(stmts[0]);
  if (!es)
    return nullptr;
  auto *call = ast::dyn_cast<ast::CallExpr>(es->getExpr());
  return call && call->getCalleeName() == names::kMethodSuper ? call : nullptr;
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
    if (auto *ls = ast::dyn_cast<ast::LockStmt>(s)) {
      // A lock block runs once, to its end (it cannot return).
      checkUses(ls->getCell(), in, false);
      return {analyzeStmt(ls->getBody(), in).Assigned, false};
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
  if (const ast::CallExpr *call = frontSuperCall(stmts)) {
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
        // "tmp_import::helper::Adder").
        reportUndefinedSuperclass(superName, cd->getName(), nullptr,
                                  cd->getLocation());
        broken = true;
      }
      if (broken) {
        ok = false;
        SuperclassLost.insert(cd->getName());
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
    if (cd->hasSuperClass() && !SuperclassLost.count(cd->getName())) {
      superClass = Ctx.lookupClassType(cd->getSuperClassName());
      // A final class cannot be subclassed: `Str`, and a class declared
      // `final class`.
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
    Ctx.preRegisterClassType(cd->getName(), superClass)
        ->setFinal(cd->isFinal());
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

  // An instantiation's field names are its template's, checked there.
  const InstantiationInfo *info = instantiationInfo(ct);
  const ast::Decl *tmpl = localTemplate(info ? info->Template : nullptr);
  if (tmpl)
    checkTemplateFieldNames(tmpl);

  // -- fields --
  StringSet fieldNames;
  for (auto *field : cd->getFields()) {
    if (!fieldNames.insert(field->getName()).second) {
      error(field->getLocation(), "duplicate field '" + field->getName() +
                                      "' in class '" + cd->getName() + "'");
      ok = false;
      continue;
    }
    // Checked here, whether or not the class writes its `__init__`.
    if (tmpl && isTypeNameForVariable(field->getName())) {
      ++SuppressedFollowOns; // reported at the template
      ok = false;
      continue;
    }
    if (!tmpl && !checkBinderName(field->getName(), field->getLocation(),
                                  /*binds=*/false, "a field name")) {
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
    ResolvingWeakDecl = field->isWeak();
    auto *fty =
        resolveValueType(field->getType(), field->getLocation(),
                         TypeContext({"field '", field->getName(),
                                      "' in class '", cd->getName(), "'"}));
    ResolvingWeakDecl = false;
    if (!fty) {
      ok = false;
      continue;
    }
    const ast::Qualifier fieldQual = takeQualifier(); // the prototype
    field->setQualifier(fieldQual);
    field->setType(fty); // canonical write-back for the lowering
    if (field->isWeak() &&
        !checkWeakType(fty, field->getLocation(),
                       "field '" + field->getName() + "' of class '" +
                           cd->getName() + "'")) {
      ok = false;
      continue;
    }
    ct->addField(field->getName(), fty, field->isWeak(), fieldQual);
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
    // `__deinit__` is the class's own, like `__init__`: the generated
    // `destroy` runs it, then each superclass's.
    const bool deinit = method->getName() == names::kMethodDeinit;
    if (deinit && !checkDeinitDecl(method, ct)) {
      ok = false;
      continue;
    }
    // __init__ is a per-class constructor, not a virtual override.
    const ast::MethodDecl *overridden = nullptr;
    if (method->getName() != names::kMethodInit && !deinit &&
        ct->getSuperClass())
      overridden = ct->getSuperClass()->findMethod(method->getName());
    if (overridden && overridden->isImpl()) {
      error(method->getLocation(),
            "method '" + method->getName() + "' of '" +
                ct->implOwner(method->getName())->getName() +
                "' is declared in an 'impl' block and cannot be overridden");
      ok = false;
      continue;
    }
    // A generic method (#353) is an `impl` block's: its name is taken, in
    // the class and below it.
    if (std::string has; method->getName() != names::kMethodInit && !deinit &&
                         methodTemplateOwner(ct, method->getName(), has)) {
      error(method->getLocation(),
            has == ct->getName()
                ? "class '" + cd->getName() + "' already has a method '" +
                      method->getName() + "'"
                : "method '" + method->getName() + "' of '" + has +
                      "' is declared in an 'impl' block and cannot be "
                      "overridden");
      ok = false;
      continue;
    }
    // A method without a body is pure virtual: a vtable slot that a
    // subclass implements (no class body's method is native: every
    // frontend's grammar keeps `native fn` at the top level).  `__init__`
    // and `destroy` have no slot of their own; they are reported where they
    // are checked (rejectBodyInits, visitClassDecl).
    const bool pure = !method->getBody() && !deinit &&
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

void Sema::reportUndefinedSuperclass(const std::string &super,
                                     const std::string &cls,
                                     const ast::ClassDecl *tmpl,
                                     ast::SourceLocation loc) {
  if (reportPrivateImport(super, loc) || isFailedImportUse(super))
    return;
  const std::string head = "superclass '" + super + "' of class '" + cls + "' ";
  if (Traits.count(super)) {
    // How a class takes a trait: `impl T for A`, `impl T for Box<T>`.
    std::string target = cls;
    if (tmpl) {
      target = tmpl->getName() + "<";
      for (size_t i = 0; i < tmpl->getTypeParams().size(); ++i)
        target += (i ? ", " : "") + *tmpl->getTypeParams()[i];
      target += ">";
    }
    error(loc, head +
                   "is a trait, not a class: a class implements a trait "
                   "with 'impl " +
                   super + " for " + target + " { ... }'");
    return;
  }
  if (ClassTemplates.count(super)) {
    error(loc, head + "is a generic class: a superclass cannot be generic");
    return;
  }
  const char *what = nullptr;
  if (ast::BuiltinType::Kind builtin; ast::builtinKindOfName(super, builtin))
    what = "a builtin type";
  else if (tmpl &&
           std::any_of(tmpl->getTypeParams().begin(),
                       tmpl->getTypeParams().end(),
                       [&](const std::string *p) { return *p == super; }))
    what = "a type parameter";
  else if (Ctx.lookupStructType(super))
    what = "a struct";
  else if (Ctx.lookupEnumType(super))
    what = "an enum";
  else if (StructTemplates.count(super))
    what = "a generic struct";
  else if (EnumTemplates.count(super))
    what = "a generic enum";
  else if (FuncTemplates.count(super))
    what = "a generic function";
  else if (ModuleFunctionNames.count(super) || lookupFunction(super))
    what = "a function"; // the module's are not declared yet
  error(loc, head + (what ? "is " + std::string(what) + ", not a class"
                          : std::string("is not defined")));
}

ast::FuncDecl *Sema::defaultInit(ast::ClassType *ct, ast::SourceLocation loc) {
  StringSet used;
  for (auto &[name, _] : ct->getFields())
    used.insert(name);
  std::vector<ast::Param> params;
  std::vector<ast::Stmt *> body;

  // The superclass constructor's arguments, named as it names them where
  // that is free (a field of this class may take the name).  An imported
  // class's names are its modes', which it has only when one parameter is
  // `view` or `inout`; `argN` stands for a name it does not have.
  ast::ClassType *super = ct->getSuperClass();
  if (const ast::MethodDecl *superInit =
          super && super != Ctx.getObjTy()
              ? super->findMethod(names::kMethodInit)
              : nullptr) {
    const ast::ParamModes &modes = superInit->getParamModes();
    std::vector<ast::Expr *> args;
    for (size_t i = 0; i < superInit->getNumParams(); ++i) {
      std::string name = superInit->getParamName(i);
      if (name.empty())
        name = modes.name(i);
      if (name.empty())
        name = "arg" + std::to_string(i + 1);
      while (!used.insert(name).second)
        name += "_";
      const std::string &interned = Ctx.intern(name);
      params.push_back({&interned, superInit->getParamTypes()[i], modes.mode(i),
                        superInit->getParamQual(i), loc});
      // An argument is passed on by `move` (the copy rule; a Copyable one
      // moves too, as the parameter ends here), but an `atomic` handle,
      // which copies into the cell's handle, not a new cell.  Not decided
      // by Copyable: a struct may not have its fields yet.
      ast::Expr *arg = Ctx.make<ast::Identifier>(loc, interned);
      if (modes.mode(i) == ast::ParamMode::Value &&
          !ast::isa<ast::AtomicType>(superInit->getParamTypes()[i]))
        arg = Ctx.make<ast::UnaryExpr>(loc, ast::UnaryOpcode::Move, arg);
      args.push_back(arg);
    }
    body.push_back(Ctx.make<ast::ExprStmt>(
        loc, Ctx.make<ast::CallExpr>(loc, Ctx.intern(names::kMethodSuper),
                                     std::move(args))));
  }
  for (auto &[name, type] : ct->getFields()) {
    const std::string &interned = Ctx.intern(name);
    // A `weak` field keeps no owner: its parameter borrows (#369).
    const bool weak = ct->isWeakField(name);
    params.push_back({&interned, type,
                      weak ? ast::ParamMode::View : ast::ParamMode::Value,
                      ct->findFieldQualifier(name), loc});
    // The field takes the parameter by `move` (the copy rule), but an
    // `atomic` handle, which copies into the cell's handle.  Not decided by
    // Copyable: a struct may not have its fields yet.
    ast::Expr *value = Ctx.make<ast::Identifier>(loc, interned);
    if (!ast::isa<ast::AtomicType>(type) && !weak)
      value = Ctx.make<ast::UnaryExpr>(loc, ast::UnaryOpcode::Move, value);
    body.push_back(Ctx.make<ast::MemberAssignStmt>(
        loc, Ctx.make<ast::Identifier>(loc, Ctx.intern(names::kSelf)), interned,
        value));
  }

  return Ctx.make<ast::FuncDecl>(
      loc, Ctx.intern(names::kMethodInit), std::move(params), nullptr,
      Ctx.make<ast::CompoundStmt>(loc, std::move(body)));
}

bool Sema::declareDefaultInit(ast::ClassDecl *cd, ast::ClassType *ct) {
  ast::FuncDecl *fn = defaultInit(ct, cd->getLocation());
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

/// Whether @p sub is the class @p super or one of its subclasses.
static bool isSubclassType(const ast::Type *sub, const ast::Type *super) {
  for (const auto *c = sub ? ast::dyn_cast<ast::ClassType>(sub) : nullptr; c;
       c = c->getSuperClass())
    if (c == super)
      return true;
  return false;
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
    retTy = resolveType(
        method->getReturnType(), method->getLocation(),
        TypeContext({"method '", method->getName(), "' in '", className, "'"}));
    if (!retTy || !checkFixedResult(retTy, method->getResultMode(),
                                    method->getLocation()))
      return nullptr;
    if (ast::Qualifier q = takeQualifier(); !q.isUnique()) {
      if (!checkBorrowQualifier(q, method->getResultMode(),
                                "the result of '" + method->getName() + "'",
                                method->getLocation()))
        return nullptr;
      method->setResultQual(q);
    }
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
      error(p.Loc, "'" + std::string(names::kSelf) +
                       "' cannot be used as a parameter name of method '" +
                       method->getName() + "' in " + typeKindWord(owner) +
                       " '" + className + "'; it is the implicit receiver");
      return nullptr;
    }
    if (!checkBinderName(p.getName(), p.Loc, /*binds=*/!DeclaringDefaultInit,
                         "a parameter name"))
      return nullptr;
    FixedArrayAllowed = true; // a parameter may be a fixed-size array
    SliceAllowed = p.Mode != ast::ParamMode::Value; // or a slice
    auto *pty = resolveValueType(
        p.ParamType, p.Loc,
        TypeContext({"parameter '", p.getName(), "' of method '",
                     method->getName(), "' in '", className, "'"}));
    if (!pty)
      return nullptr;
    if (ast::Qualifier q = takeQualifier(); !q.isUnique()) {
      if (!checkBorrowQualifier(q, p.Mode, "parameter '" + p.getName() + "'",
                                p.Loc))
        return nullptr;
      p.Qual = q;
    }
    p.ParamType = pty;
    paramTys.push_back(pty);
  }

  // A public method of a public type cannot hand an importer a type it
  // could not name (an instantiation's methods are the template's).
  if (!DeclaringDefaultInit && !names::isPrivateName(method->getName()) &&
      !isModuleOwnName(className) && className.find('<') == std::string::npos &&
      method->getName().find('<') == std::string::npos) {
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

  // An async method's type is its task's; its colour is its slot's.
  if (method->isAsync()) {
    if (method->getName() == names::kMethodInit) {
      error(method->getLocation(), "'__init__' cannot be 'async': constructing "
                                   "gives the object at once");
      return nullptr;
    }
    // The compiler calls these itself, and has no task to wait for: the
    // ones it calls, by their parameters (a `hash(salt: int)` is the
    // program's own).
    for (const char *implicit : {names::kMethodClone, names::kMethodEquals,
                                 names::kMethodHash, names::kMethodToString})
      if (method->getName() == implicit &&
          method->getParams().size() ==
              (method->getName() == names::kMethodEquals ? 1u : 0u)) {
        error(method->getLocation(),
              "'" + method->getName() +
                  "' cannot be 'async': the compiler calls it, and waits for "
                  "no task");
        return nullptr;
      }
    if (!checkAsyncDecl(method, retTy))
      return nullptr;
    retTy = taskOf(method->getResultQual(), retTy);
    flags |= ast::MethodDecl::Async;
  }
  if (overridden && overridden->isAsync() != method->isAsync()) {
    error(method->getLocation(),
          "override of '" + method->getName() + "' " +
              (overridden->isAsync()
                   ? "must be an 'async fn', like the method it overrides"
                   : "cannot be an 'async fn': the method it overrides is "
                     "not"));
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
    // `clone` alone returns its own class (#369): each class's clone
    // makes an object of that class.
    bool sigMatches = (typesEqual(overridden->getReturnType(), retTy) ||
                       (method->getName() == names::kMethodClone &&
                        isSubclassType(retTy, overridden->getReturnType()))) &&
                      baseParams.size() == paramTys.size();
    for (size_t i = 0; sigMatches && i < paramTys.size(); ++i)
      sigMatches = typesEqual(baseParams[i], paramTys[i]);
    // And in the forms a caller sees (#368), as a function type's: a call
    // through the base gives and takes what the override does, so an
    // override's `-> shared mut Node` of a base's `-> Node` would be a
    // shared object handed out as a new unique one.
    // A borrow's form is a type argument's (a written one is refused): it
    // is what the override takes, exactly.
    auto seen = [](ast::Qualifier q, ast::ParamMode mode) {
      if (mode != ast::ParamMode::Value)
        return q.form();
      return q.Form != ast::QualForm::Unique ? q.form() : ast::Qualifier{};
    };
    std::string forms; // the forms that differ, when the types do not
    if (sigMatches && !(seen(overridden->getResultQual(),
                             overridden->getParamModes().Result) ==
                        seen(method->getResultQual(), method->getResultMode())))
      forms = "its result is '" +
              qualifiedName(method->getResultQual(), retTy) + "', not '" +
              qualifiedName(overridden->getResultQual(),
                            overridden->getReturnType()) +
              "'";
    for (size_t i = 0; sigMatches && forms.empty() && i < paramTys.size(); ++i)
      if (!(seen(overridden->getParamQual(i),
                 overridden->getParamModes().mode(i)) ==
            seen(method->getParams()[i].Qual, method->getParams()[i].Mode)))
        forms = "its parameter '" + method->getParams()[i].getName() +
                "' is '" +
                qualifiedName(method->getParams()[i].Qual, paramTys[i]) +
                "', not '" +
                qualifiedName(overridden->getParamQual(i), baseParams[i]) + "'";
    if (!forms.empty()) {
      error(method->getLocation(),
            "override of '" +
                signatureString(ct->getSuperClass(), overridden->getName(),
                                overridden->getReturnType(),
                                overridden->getParamTypes()) +
                "' takes and gives the forms it does: " + forms);
      ok = false;
    } else if (!sigMatches) {
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
  if (method->isShared())
    flags |= ast::MethodDecl::Shared;
  auto *mdecl =
      Ctx.make<ast::MethodDecl>(method->getLocation(), method->getName(), retTy,
                                std::move(paramTys), flags);
  ast::ParamModes modes = ast::paramModes(method->getParams());
  modes.Result = method->getResultMode();
  mdecl->setParamModes(std::move(modes));
  std::vector<const std::string *> names;
  for (const ast::Param &p : method->getParams())
    names.push_back(p.Name);
  mdecl->setParamNames(std::move(names));
  std::vector<ast::Qualifier> quals; // the prototype
  for (const ast::Param &p : method->getParams())
    quals.push_back(p.Qual);
  mdecl->setParamQuals(std::move(quals));
  mdecl->setResultQual(method->getResultQual());
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
  if (initDecl) {
    FunctionTable[cd->getName()].Modes = initDecl->getParamModes();
    FunctionTable[cd->getName()].ParamQuals = initDecl->getParamQuals();
  }
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

bool Sema::populateImplMethods(ast::ClassDecl *cd, ast::ClassType *ct,
                               const ast::ImplDecl *only) {
  static const std::vector<ast::ImplDecl *> kNone;
  auto it = ImplsByClass.find(cd->getName());
  bool ok = true;
  // By index: a method's signature may ask for an instance, whose blocks
  // are added to the table this vector is in.
  const std::vector<ast::ImplDecl *> impls =
      it == ImplsByClass.end() ? kNone : it->second;
  for (auto *im : impls) {
    if (only && im != only)
      continue;
    // An instance's copy of a block leaves the block's generic methods
    // (#353) out: they join the instance as templates, by name.
    if (auto of = ImplCopyOf.find(im);
        of != ImplCopyOf.end() && !joinGenericMembers(ct, of->second))
      ok = false;
    // A `Cloneable` block's `clone` is virtual, not final like the other
    // methods of an `impl` block: each class clones as its own type, so a
    // subclass's overrides its superclass's (#369).
    const bool cloneable =
        im->getTrait() && namesBuiltinTrait(im->getTrait()) &&
        traitAnnotationName(im->getTrait()) == names::kTraitCloneable;
    const ast::MethodDecl *superClone =
        cloneable && ct->getSuperClass()
            ? ct->getSuperClass()->findMethod(names::kMethodClone)
            : nullptr;
    bool hasClone = false;
    for (auto *fn : im->getMethods()) {
      if (fn->isStatic())
        continue;
      if (rejectReservedDestroy(fn, ct)) {
        ok = false;
        continue;
      }
      const std::string &name = fn->getName();
      if (name == names::kMethodDeinit && !checkDeinitDecl(fn, ct)) {
        ok = false;
        continue;
      }
      // A generic method (#353) is a template of the class: declared by
      // its instantiations, where it is called.
      if (!fn->getTypeParams().empty()) {
        std::string has;
        if (ct->findMethod(name))
          has = methodOwner(ct, name)->getName();
        if (!has.empty() || methodTemplateOwner(ct, name, has)) {
          error(fn->getLocation(),
                "class '" + cd->getName() + "' already has a method '" + name +
                    "'" +
                    (has == ct->getName() ? "" : " (from '" + has + "')"));
          ok = false;
        } else if (!checkMethodTemplate(cd->getName(), fn, im)) {
          ok = false;
        } else {
          noteMethodTemplate(fn, im, nullptr);
          MethodTemplates[ct->getName()][name] = MethodTemplate{fn, im};
        }
        continue;
      }
      // __init__ is the same here as in the class body: the class's own,
      // never inherited or called through the vtable.  So is __deinit__.
      const bool init =
          name == names::kMethodInit || name == names::kMethodDeinit;
      const bool clone = cloneable && name == names::kMethodClone;
      hasClone = hasClone || clone;
      if (ct->findMethod(name) &&
          !(clone && superClone && declaringClass(ct, name) != ct)) {
        const ast::ClassType *owner = init ? ct : methodOwner(ct, name);
        error(fn->getLocation(),
              "class '" + cd->getName() + "' already has a method '" + name +
                  "'" +
                  (owner == ct ? "" : " (from '" + owner->getName() + "')"));
        ok = false;
        continue;
      }
      if (std::string has; methodTemplateOwner(ct, name, has)) {
        error(fn->getLocation(),
              "class '" + cd->getName() + "' already has a method '" + name +
                  "'" + (has == ct->getName() ? "" : " (from '" + has + "')"));
        ok = false;
        continue;
      }
      std::optional<TypeParamScope> params;
      implTypeParamScope(params, fn);
      if (auto *md = declareMethod(fn, ct, clone ? superClone : nullptr,
                                   init || clone ? ast::MethodDecl::None
                                                 : ast::MethodDecl::Impl,
                                   ok)) {
        ct->addMethod(md);
        cd->addMethod(fn); // checked and lowered with the class body
      } else {
        ok = false;
      }
    }
    // An empty `impl Cloneable for C {}` derives `clone`: the lowering
    // builds a new object from the class's fields (a unique one cloned, a
    // Copyable, `shared` or `weak` one the same).
    if (cloneable && !hasClone) {
      // Whether every field has a clone is known once each field's type is
      // whole: until then the check waits (checkPendingCloneDerives).
      bool whole = true;
      for (const ast::ClassType *c = ct; c && whole; c = c->getSuperClass())
        for (const auto &[fname, fty] : c->getFields())
          whole = whole && isWhole(fty);
      // Reported, the method is still declared: its uses are not errors too.
      if (!whole)
        PendingCloneDerives.push_back({cd->getName(), ct, im->getLocation(),
                                       CurrentOrigin, InstantiationStack});
      else if (!checkCloneDerive(cd->getName(), ct, im->getLocation()))
        ok = false;
      auto *fn = Ctx.make<ast::FuncDecl>(
          im->getLocation(), Ctx.intern(names::kMethodClone),
          std::vector<ast::Param>{}, ct, nullptr);
      fn->setView(true);
      fn->setDerivedClone(true);
      if (auto *md =
              declareMethod(fn, ct, superClone, ast::MethodDecl::None, ok)) {
        ct->addMethod(md);
        cd->addMethod(fn);
      } else {
        ok = false;
      }
    }
  }
  // A class that inherits `Cloneable` clones as its own type: a
  // superclass's `clone` would make only that superclass's object.
  if (ast::ClassType *from = declaringClass(ct, names::kMethodClone);
      from && from != ct && ct->getSuperClass() &&
      hasBuiltinTraitImpl(ct->getSuperClass(), names::kTraitCloneable)) {
    error(cd->getLocation(),
          "class '" + cd->getName() + "' inherits 'Cloneable' from '" +
              from->getName() + "', whose 'clone' makes a '" + from->getName() +
              "': write 'impl Cloneable for " + cd->getName() +
              " {}' to derive its own, or a 'clone' of its own");
    ok = false;
  }
  return ok;
}

bool Sema::rejectReservedDestroy(const ast::FuncDecl *fn, ast::Type *owner) {
  // `__deinit__` is the object's own: no `static fn` has the name.
  if (fn->getName() == names::kMethodDeinit && fn->isStatic())
    return !checkDeinitDecl(fn, owner);
  // A class's `__walk` (the prototype) is emitted for it too: the walk of
  // its references that freezes an `atomic shared` object.
  if (fn->getName() == names::kMethodWalk) {
    error(fn->getLocation(),
          "'__walk' is reserved for the compiler-generated walk and cannot "
          "be declared in " +
              std::string(typeKindWord(owner)) + " '" + typeName(owner) + "'");
    return true;
  }
  // A class's `destroy` is emitted for it (releasing the fields and freeing
  // the object) and is final; a struct, a value, is never destroyed.
  if (fn->getName() != names::kMethodDestroy)
    return false;
  error(fn->getLocation(),
        "'destroy' is reserved for the compiler-generated destructor and "
        "cannot be declared in " +
            std::string(typeKindWord(owner)) + " '" + typeName(owner) + "'" +
            (ast::isa<ast::StructType>(owner) ? " (a struct has none)" : ""));
  return true;
}

void Sema::checkTemplateFieldNames(const ast::Decl *tmpl) {
  if (!TemplateFieldsChecked.insert(tmpl).second)
    return;
  // Not under an instantiation (its notes would name one of many): the
  // template's fields are checked as a struct's are, its own type
  // parameters being type names there (not a generic body's that uses it).
  std::vector<InstantiationFrame> frames =
      std::exchange(InstantiationStack, {});
  auto check = [&](const std::vector<const std::string *> &typeParams,
                   const std::vector<ast::VarDecl *> &fields) {
    TypeParamScope params(*this, typeParams);
    for (const ast::VarDecl *field : fields)
      checkBinderName(field->getName(), field->getLocation(),
                      /*binds=*/false, "a field name");
  };
  if (const auto *cd = ast::dyn_cast<ast::ClassDecl>(tmpl))
    check(cd->getTypeParams(), cd->getFields());
  else if (const auto *sd = ast::dyn_cast<ast::StructDecl>(tmpl))
    check(sd->getTypeParams(), sd->getFields());
  else if (const auto *ed = ast::dyn_cast<ast::EnumDecl>(tmpl))
    for (size_t v = 0; v < ed->getNumVariants(); ++v)
      check(ed->getTypeParams(), ed->getPayload(v));
  InstantiationStack = std::move(frames);
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
  StringSet plainStatics; // `Type::name` of each `static fn` not generic
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
      if (rejectReservedDestroy(fn, target)) {
        // Its calls are follow-ons.
        ErroneousNames.insert(name + names::kQualSep + fn->getName());
        ok = false;
        continue;
      }
      if (std::string has; methodOf(target, fn->getName()) ||
                           methodTemplateOwner(target, fn->getName(), has)) {
        error(fn->getLocation(), std::string(typeKindWord(target)) + " '" +
                                     name + "' already has a method '" +
                                     fn->getName() + "'");
        ok = false;
        continue;
      }
      // A generic one (#353) is a template of the type: the function
      // `Type::name<args>` of each of its instantiations.
      if (!fn->getTypeParams().empty()) {
        // (A `static fn` of the name that is not generic is a function
        // already, which no method table has.)
        if (plainStatics.count(name + names::kQualSep + fn->getName())) {
          error(fn->getLocation(), std::string(typeKindWord(target)) + " '" +
                                       name + "' already has a method '" +
                                       fn->getName() + "'");
          ok = false;
        } else if (!checkMethodTemplate(name, fn, im)) {
          ok = false;
        } else {
          noteMethodTemplate(fn, im, nullptr);
          MethodTemplates[typeName(target)][fn->getName()] =
              MethodTemplate{fn, im};
        }
        continue;
      }
      fn->setName(Ctx.intern(name + names::kQualSep + fn->getName()));
      plainStatics.insert(fn->getName());
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
  // What implements a trait is known: the bounded blocks of the instances
  // made so far join the ones that meet their bounds, and then a derived
  // `clone` can be told of its fields.
  ConformancesKnown = true;
  if (!joinLateImpls())
    ok = false;
  if (!checkPendingCloneDerives())
    ok = false;
  return ok;
}

bool Sema::checkCloneDerive(const std::string &name, const ast::ClassType *ct,
                            ast::SourceLocation loc) {
  // Every field, the superclasses' too, has a clone, or is the same object
  // (`shared`, `weak`) or a value (an `atomic` field gets a cell of its
  // own).
  for (const ast::ClassType *c = ct; c; c = c->getSuperClass())
    for (const auto &[fname, fty] : c->getFields())
      if (!ct->isWeakField(fname) && !ast::isa<ast::AtomicType>(fty) &&
          !isCloneablePart(fty, ct->findFieldQualifier(fname)) &&
          // An archetype stands for any type: whether a `T` has a `clone`
          // is each instance's question (#368), in a part too.
          !(Archetypes &&
            typeName(fty).find(names::kArchetypePrefix) != std::string::npos)) {
        std::string inner;
        isCloneable(fty, &inner);
        std::string msg = "'impl Cloneable for ";
        msg += name;
        msg += " {}' cannot derive 'clone': its field '";
        msg += fname;
        msg += "' ('";
        msg += qualifiedName(ct->findFieldQualifier(fname), fty);
        msg += "') has no clone";
        if (!inner.empty()) {
          msg += ": ";
          msg += inner;
        }
        msg += "; write the 'clone' instead";
        error(loc, msg);
        return false;
      }
  return true;
}

bool Sema::checkPendingCloneDerives() {
  bool ok = true;
  std::vector<PendingCloneDerive> pending =
      std::exchange(PendingCloneDerives, {});
  for (PendingCloneDerive &p : pending) {
    std::vector<InstantiationFrame> saved =
        std::exchange(InstantiationStack, std::move(p.Frames));
    {
      OriginSource inOrigin(*this, p.Origin);
      if (!checkCloneDerive(p.Name, p.Class, p.Loc))
        ok = false;
    }
    InstantiationStack = std::move(saved);
  }
  return ok;
}

bool Sema::isWhole(const ast::Type *ty) const {
  if (!ty)
    return true;
  if (const auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
    return isWhole(qt->getInnerType());
  if (const auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return isWhole(ot->getInnerType());
  if (const auto *tk = ast::dyn_cast<ast::TaskType>(ty))
    return isWhole(tk->getInnerType());
  if (const auto *at = ast::dyn_cast<ast::ArrayType>(ty))
    return isWhole(at->getElementType());
  if (const auto *fa = ast::dyn_cast<ast::FixedArrayType>(ty))
    return isWhole(fa->getElementType());
  if (const auto *sl = ast::dyn_cast<ast::SliceType>(ty))
    return isWhole(sl->getElementType());
  const std::string *name = nullptr;
  const std::vector<ast::Type *> *args = nullptr;
  if (const auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (ct->isBuiltin() || (Archetypes && Archetypes->Classes.count(ct)))
      return true;
    name = &ct->getName();
    if (auto it = ClassInstantiationInfo.find(const_cast<ast::ClassType *>(ct));
        it != ClassInstantiationInfo.end())
      args = &it->second.Args;
  } else if (const auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    name = &st->getName();
    if (auto it =
            StructInstantiationInfos.find(const_cast<ast::StructType *>(st));
        it != StructInstantiationInfos.end())
      args = &it->second.Args;
  } else if (const auto *et = ast::dyn_cast<ast::EnumType>(ty)) {
    name = &et->getName();
    if (auto it = EnumInstantiationInfos.find(const_cast<ast::EnumType *>(et));
        it != EnumInstantiationInfos.end())
      args = &it->second.Args;
  }
  // (A struct is a tuple with a name: asked by its name, above.)
  if (const auto *tt = name ? nullptr : ast::dyn_cast<ast::TupleType>(ty)) {
    const auto &elems = tt->getElementTypes();
    return std::all_of(elems.begin(), elems.end(),
                       [&](const ast::Type *e) { return isWhole(e); });
  }
  if (!name)
    return true; // a number, a function value: nothing waits
  // Until the module's `impl` blocks are recorded, a type of the module may
  // get a field or a conformance yet, and so may any instance: its own
  // `impl ... for` blocks are checked with the module's, and its template's
  // fields may name a class that has none of its own yet.
  if (!ConformancesKnown)
    return !args && ImportedTypeOrigins.count(*name) != 0;
  if (!args)
    return true;
  if (HalfMade.count(ty))
    return false;
  for (size_t i = LateImplsDone; i < LateImpls.size(); ++i)
    if (LateImpls[i].Inst == *name)
      return false;
  return std::all_of(args->begin(), args->end(),
                     [&](const ast::Type *a) { return isWhole(a); });
}

bool Sema::checkDeinitDecl(const ast::FuncDecl *fn, const ast::Type *owner) {
  std::string why;
  const bool object = ast::isa<ast::ClassType>(owner);
  if (!object && !ast::isa<ast::StructType>(owner))
    why = "is a class's or a struct's: a " + std::string(typeKindWord(owner)) +
          " has none";
  else if (fn->isStatic() || fn->isView() || fn->isShared())
    why = "is a plain 'fn', not a 'static', 'view' or 'shared' one";
  else if (fn->isAsync())
    why = object ? "cannot be 'async': an object is destroyed where its last "
                   "reference goes, which waits for no task"
                 : "cannot be 'async': a value is destroyed where its storage "
                   "ends, which waits for no task";
  else if (!fn->getParams().empty())
    why = "takes no parameters";
  else if (fn->getReturnType() || fn->hasInferredResult())
    why = "returns nothing";
  else if (!fn->getBody())
    why = "needs a body";
  if (why.empty())
    return true;
  error(fn->getLocation(), std::string("'") + names::kMethodDeinit + "' " +
                               why + " ('fn __deinit__() { ... }' runs when " +
                               (object ? "the object" : "the value") +
                               " is destroyed)");
  return false;
}

bool Sema::joinsLater(const ast::ImplDecl *im, bool ofClass,
                      const std::vector<ast::Type *> &args) const {
  // The archetype check asks of its own stand-ins.
  if (Archetypes)
    return false;
  bool bounded = false;
  for (const auto &bs : im->getTypeParamBounds())
    bounded = bounded || !bs.empty();
  if (!bounded)
    return false;
  if (ofClass && im->getTrait() && namesBuiltinTrait(im->getTrait()) &&
      traitAnnotationName(im->getTrait()) == names::kTraitCloneable)
    return false;
  if (std::any_of(im->getMethods().begin(), im->getMethods().end(),
                  [](const ast::FuncDecl *fn) {
                    return fn->getName() == names::kMethodInit;
                  }))
    return false;
  return !std::all_of(args.begin(), args.end(),
                      [&](const ast::Type *a) { return isWhole(a); });
}

ast::ImplDecl *Sema::cloneImplFor(ast::ImplDecl *im,
                                  const std::string &instName,
                                  const std::vector<ast::Type *> &args,
                                  bool foreign) {
  // By the block's own names for the type parameters.
  Subst s;
  for (size_t i = 0; i < args.size(); ++i)
    s[*im->getTypeParams()[i]] = args[i];
  ast::ASTCloner c(Ctx, std::move(s), nullptr, foreign);
  c.recordTypeArgs(WrittenTypeArgs, im->getTypeParams(), instName);
  ast::ImplDecl *copy = c.cloneImplDecl(im, instName);
  ImplCopyOf[copy] = im;
  ImplsByClass[instName].push_back(copy);
  noteImplTypeParams(im, copy);
  return copy;
}

void Sema::instancesMade() {
  if (JoiningLateImpls || !ConformancesKnown)
    return;
  joinLateImpls();
  checkPendingCloneDerives();
  if (TraitImplsKnown) {
    checkPendingBounds();
    checkPendingAsyncResults();
  }
}

bool Sema::joinLateImpls() {
  if (JoiningLateImpls)
    return true;
  JoiningLateImpls = true;
  bool ok = true;
  // A block that joins may ask for an instance, whose own blocks wait in
  // turn: at the end of the list, which is done in order.
  {
    for (; LateImplsDone < LateImpls.size(); ++LateImplsDone) {
      LateImpl l = std::move(LateImpls[LateImplsDone]);
      if (!l.Base)
        continue; // the instance was not made
      ast::ClassType *ct = nullptr;
      ast::StructType *st = nullptr;
      if (l.Base->Class) {
        auto it = ClassInstantiations.find(l.Inst);
        ct = it == ClassInstantiations.end() ? nullptr : it->second;
      } else {
        auto it = StructInstantiations.find(l.Inst);
        st = it == StructInstantiations.end() ? nullptr : it->second;
      }
      ast::Type *type = ct ? static_cast<ast::Type *>(ct) : st;
      if (!type)
        continue;
      // As where the instance was made: in its module's file, under the
      // instantiations in progress there.
      std::vector<InstantiationFrame> savedStack =
          std::exchange(InstantiationStack, std::move(l.Frames));
      std::vector<InstantiatingTemplate> savedTemplates =
          std::exchange(InstantiatingTemplates, l.Base->Templates);
      ast::ImplDecl *copy = nullptr;
      {
        OriginSource inOrigin(*this, l.Origin);
        MakingInstance making(*this, l.Template, l.Args);
        if (implBoundsHold(l.Impl, l.Args)) {
          copy = cloneImplFor(l.Impl, l.Inst, l.Args, l.Origin != nullptr);
          const std::vector<const std::string *> &typeParams =
              ct ? ast::cast<ast::ClassDecl>(l.Template)->getTypeParams()
                 : ast::cast<ast::StructDecl>(l.Template)->getTypeParams();
          TypeParamScope params(*this, typeParams);
          if (!(ct ? populateImplMethods(l.Base->Class, ct, copy)
                   : populateImplMethods(l.Base->Struct, st, copy)))
            ok = false;
          if (copy->getTrait() && !checkTraitImpl(copy, type))
            ok = false;
        }
      }
      InstantiatingTemplates = std::move(savedTemplates);
      InstantiationStack.pop_back(); // the instance's own request
      if (copy)
        declareInstantiationStatics(l.Inst, type, *l.Base, copy);
      InstantiationStack = std::move(savedStack);
    }
  }
  LateImpls.clear();
  LateImplsDone = 0;
  JoiningLateImpls = false;
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
    classCtx.SharedMethod = method->isShared();
    std::optional<TypeParamScope> params;
    implTypeParamScope(params, method);

    if (rejectReservedDestroy(method, ct)) {
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
    classCtx.FrontSuperCall =
        method->getName() == names::kMethodInit
            ? frontSuperCall(method->getBody()->getStatements())
            : nullptr;
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
      // `self` is written in every method but a `view fn` (`__init__`
      // builds it); an `inout` parameter is the borrow's writes.  A
      // `shared fn`'s is shared: a Copyable reference it may hand on.
      CurrentScope->Quals[names::kSelf] = ast::Qualifier{
          method->isShared() ? ast::QualForm::Shared : ast::QualForm::Unique,
          !method->isView()};
      for (size_t i = 0; i < method->getParams().size(); ++i) {
        const ast::Param &p = method->getParams()[i];
        CurrentScope->declare(p.getName(), paramTys[i]);
        ast::Qualifier q = p.Qual;
        // (a shared form, a template argument\'s, is its own: #368)
        if (p.Mode == ast::ParamMode::Inout && q.Form == ast::QualForm::Unique)
          q.Mut = true;
        CurrentScope->Quals[p.getName()] = q;
      }
      declareParamKinds(method);
      CurrentResultQual = method->getResultQual(); // the prototype

      // What the body does to `self`, for warnMissingViewFns.
      if (method->getName() != names::kMethodInit) {
        CurrentMethodUse = &MethodUses.emplace_back();
        CurrentMethodUse->Class = ct;
        CurrentMethodUse->Decl = method;
      }

      // The method's moves, task handles and loop depths are its own (the
      // prototype); a generic instance's methods are checked in the middle
      // of another body, whose state comes back after.
      Scope *savedFunctionScope = std::exchange(FunctionScope, CurrentScope);
      std::unordered_map<std::string, ast::SourceLocation> savedTasks =
          std::exchange(TaskLocals, {});
      MovedMap savedMoves = std::exchange(MovedVars, {});
      std::vector<LoopMoves> savedLoopMoves = std::exchange(LoopMoveStack, {});
      const bool savedFlowEnded = std::exchange(FlowEnded, false);
      std::unordered_map<std::string, unsigned> savedDeclDepths =
          std::exchange(DeclLoopDepth, {});
      const unsigned savedLoopDepth = std::exchange(LoopDepth, 0u);
      bool bodyOk = visitBody(method->getBody()->getStatements());
      if (bodyOk && !FlowEnded)
        checkTasksAwaited(CurrentScope, nullptr, method->getLocation());
      LoopDepth = savedLoopDepth;
      DeclLoopDepth = std::move(savedDeclDepths);
      FlowEnded = savedFlowEnded;
      LoopMoveStack = std::move(savedLoopMoves);
      MovedVars = std::move(savedMoves);
      FunctionScope = savedFunctionScope;
      TaskLocals = std::move(savedTasks);
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
        // A `__super__(...)` anywhere but first was reported at the call, so
        // a body that checked calls it first or not at all.
        if (classCtx.SuperInitRequired && !classCtx.FrontSuperCall) {
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
          ok = false;
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
    // What no instance changes is said once, of the template.
    if (im->getTrait()) {
      std::string what = std::string(kind) + " '" + name;
      for (size_t i = 0; i < params.size(); ++i)
        (what += i ? ", " : "<") += *params[i];
      what += ">'";
      if (!checkImplForm(im, traitAnnotationName(im->getTrait()),
                         namesBuiltinTrait(im->getTrait()), what)) {
        ok = false;
        continue;
      }
    }
    // A block's bounds (`impl List<T: Cloneable>`) name built-in traits: an
    // instance is made before the module's own traits' blocks are known.
    bool bounded = true;
    for (const auto &bs : im->getTypeParamBounds())
      for (ast::Type *b : bs)
        if (!namesBuiltinTrait(b)) {
          error(b->getLocation().isValid() ? b->getLocation()
                                           : im->getLocation(),
                "the bounds of an 'impl' block name built-in traits ('" +
                    std::string(names::kTraitCloneable) + "', '" +
                    names::kTraitCopyable + "', ...), not '" +
                    traitAnnotationName(b) + "'");
          bounded = false;
        }
    if (!bounded) {
      ok = false;
      continue;
    }
    // Every instance is destroyed one way: whether a value has a
    // `__deinit__` is its layout's, and a block that only some instances
    // have is decided after the first of them are laid out (#378).
    const bool conditional = std::any_of(
        im->getTypeParamBounds().begin(), im->getTypeParamBounds().end(),
        [](const auto &bs) { return !bs.empty(); });
    if (conditional) {
      bool deinit = false;
      for (const ast::FuncDecl *fn : im->getMethods())
        if (fn->getName() == names::kMethodDeinit && !fn->isStatic()) {
          std::string plain = name;
          for (size_t i = 0; i < params.size(); ++i)
            (plain += i ? ", " : "<") += *params[i];
          plain += ">";
          std::string msg = "'";
          msg += names::kMethodDeinit;
          msg += "' cannot be in a conditional 'impl' block: every '";
          msg += plain;
          msg += "' is destroyed the same way; declare it in 'impl ";
          msg += plain;
          msg += "'";
          error(fn->getLocation(), msg);
          deinit = true;
        }
      if (deinit) {
        ok = false;
        continue;
      }
    }
    TemplateImpls[tmpl].push_back(im);
    // Its generic methods are the template's (#353): one template for
    // every instance.
    const ast::TypeParamBounds *ownerBounds = nullptr;
    if (const auto *cd = ast::dyn_cast<ast::ClassDecl>(tmpl))
      ownerBounds = &cd->getTypeParamBounds();
    else if (const auto *sd = ast::dyn_cast<ast::StructDecl>(tmpl))
      ownerBounds = &sd->getTypeParamBounds();
    // (Which of them an instance has, and under which names, is settled
    // where the block joins it: joinGenericMembers.)
    for (ast::FuncDecl *fn : im->getMethods()) {
      if (fn->getTypeParams().empty())
        continue;
      if (!checkMethodTemplate(name, fn, im)) {
        ok = false;
        continue;
      }
      noteMethodTemplate(fn, im, ownerBounds);
      TemplateMethodTemplates[tmpl].push_back({fn, im});
    }
  }
  return ok;
}

// -- Generic methods (#353)

std::string Sema::FoundMethodTemplate::ownerName() const {
  return Class ? Class->getName() : Struct->getName();
}

void Sema::noteMethodTemplate(ast::FuncDecl *fn, const ast::ImplDecl *im,
                              const ast::TypeParamBounds *ownerBounds) {
  // The names its code sees: the block's, then its own.
  MethodTemplateNames names;
  const ast::TypeParamBounds &blockBounds = im->getTypeParamBounds();
  for (size_t i = 0; i < im->getTypeParams().size(); ++i) {
    names.Params.push_back(im->getTypeParams()[i]);
    std::vector<ast::Type *> bs;
    if (i < blockBounds.size())
      bs = blockBounds[i];
    if (ownerBounds && i < ownerBounds->size())
      bs.insert(bs.end(), (*ownerBounds)[i].begin(), (*ownerBounds)[i].end());
    names.Bounds.push_back(std::move(bs));
    names.Own.emplace_back();
  }
  const ast::TypeParamBounds &own = fn->getTypeParamBounds();
  for (size_t i = 0; i < fn->getTypeParams().size(); ++i) {
    names.Params.push_back(fn->getTypeParams()[i]);
    names.Bounds.push_back(i < own.size() ? own[i]
                                          : std::vector<ast::Type *>{});
    names.Own.push_back(names.Bounds.back());
  }
  MethodTemplateParams[fn] = std::move(names);
}

bool Sema::checkMethodTemplate(const std::string &ownerName,
                               const ast::FuncDecl *fn,
                               const ast::ImplDecl *im) {
  const std::string &name = fn->getName();
  std::string what = fn->isStatic() ? "generic function '" : "generic method '";
  if (fn->isStatic())
    what += ownerName + names::kQualSep;
  what += name + "'";
  bool ok = true;
  // Its own type parameters: no name twice, none a type's, none the
  // block's (inside the method `T` would mean one or the other).
  StringSet seen;
  for (const std::string *p : fn->getTypeParams()) {
    std::string msg;
    if (!seen.insert(*p).second) {
      msg = "duplicate type parameter '" + *p + "' in " + what;
    } else if (std::any_of(im->getTypeParams().begin(),
                           im->getTypeParams().end(),
                           [&](const std::string *b) { return *b == *p; })) {
      msg = "type parameter '" + *p + "' of " + what +
            " shadows type parameter '" + *p + "' of the 'impl' block for '" +
            im->getTypeName() + "'";
    } else if (Ctx.lookupType(*p) || LocalClassNames.count(*p) ||
               ClassTemplates.count(*p) || StructTemplates.count(*p) ||
               EnumTemplates.count(*p)) {
      msg = "type parameter '" + *p + "' of " + what +
            " shadows a type of the same name";
    }
    if (!msg.empty()) {
      error(fn->getLocation(), msg);
      ok = false;
    }
  }
  // A public member of a public type cannot hand an importer a type it
  // could not name: by its written signature, once, here (the type
  // arguments of an instantiation are whoever's make it).
  if (!names::isPrivateName(name) && !isModuleOwnName(ownerName)) {
    std::string who =
        fn->isStatic() ? "function '" + ownerName + names::kQualSep + name + "'"
                       : "method '" + name + "' of '" + ownerName + "'";
    who += " is public but ";
    const char *fix = fn->isStatic() ? "function" : "method";
    auto written = [&](ast::Type *ty, const std::string &part) {
      const std::string priv = privateTypeIn(ty);
      // (A type parameter is no type of the module's, whatever its name.)
      if (priv.empty() || seen.count(priv) ||
          std::any_of(im->getTypeParams().begin(), im->getTypeParams().end(),
                      [&](const std::string *b) { return *b == priv; }))
        return;
      error(fn->getLocation(), who + part + " names the private type '" + priv +
                                   "'; make the " + fix +
                                   " private or the type public");
      ok = false;
    };
    written(fn->getReturnType(), "its result");
    for (const ast::Param &p : fn->getParams())
      written(p.ParamType, "parameter '" + p.getName() + "'");
  }
  return ok;
}

bool Sema::joinGenericMembers(ast::Type *type, const ast::ImplDecl *block) {
  bool ok = true;
  const std::string owner = typeName(type);
  auto *ct = ast::dyn_cast<ast::ClassType>(type);
  for (ast::FuncDecl *fn : block->getMethods()) {
    // (One rejected where it is declared was not recorded.)
    if (!MethodTemplateParams.count(fn))
      continue;
    const std::string &name = fn->getName();
    // The name is the instance's to give once: to a method, its own or an
    // inherited one, or to one generic member.
    std::string has;
    if (ct && ct->findMethod(name))
      has = methodOwner(ct, name)->getName();
    else if (!ct && ((name == names::kMethodToString && !fn->isStatic()) ||
                     methodOf(type, name)))
      has = owner;
    const bool taken = !has.empty();
    if (taken || methodTemplateOwner(type, name, has)) {
      // The archetype check's instance meets every bound, so it gets one
      // name from each of two bounded blocks: the first stands, and each
      // member is still checked on its own (checkArchetypes).
      if (Archetypes && !taken && has == owner)
        continue;
      std::string msg = typeKindWord(type);
      msg += " '";
      msg += owner;
      msg += "' already has a method '";
      msg += name;
      msg += "'";
      if (has != owner) {
        msg += " (from '";
        msg += has;
        msg += "')";
      }
      error(fn->getLocation(), msg);
      ok = false;
      continue;
    }
    MethodTemplates[owner][name] = MethodTemplate{fn, block};
  }
  return ok;
}

const StringMap<Sema::MethodTemplate> *
Sema::methodTemplatesOf(ast::ClassType *ct, ast::StructType *st,
                        std::vector<ast::Type *> &ownerArgs,
                        ast::SourceLocation loc) {
  ownerArgs.clear();
  const std::string &name = st ? st->getName() : ct->getName();
  // Another module's type: its generic members are in that module's
  // templates, which this module may not have asked for yet (a type an
  // import's interface gives, of a module it does not import itself).
  if (auto origin = ImportedTypeOrigins.find(name);
      origin != ImportedTypeOrigins.end())
    seeTemplatesOf(origin->second, loc);
  const ast::Decl *tmpl = nullptr;
  const ast::Type *type = st ? static_cast<const ast::Type *>(st) : ct;
  if (st) {
    if (const StructInstantiationInfo *info = structInstantiationInfo(st)) {
      tmpl = info->Template;
      ownerArgs = info->Args;
    }
  } else if (const InstantiationInfo *info = instantiationInfo(ct)) {
    tmpl = info->Template;
    ownerArgs = info->Args;
  }
  // An instance another module made had no block join it here: it has the
  // generic members of the blocks whose bounds its arguments meet, which
  // that module checked.
  if (tmpl && !InstancesMadeHere.count(type) &&
      ImportedInstancesJoined.insert(name).second)
    if (auto of = TemplateMethodTemplates.find(tmpl);
        of != TemplateMethodTemplates.end())
      for (const MethodTemplate &mt : of->second)
        if (implBoundsHold(mt.Impl, ownerArgs))
          MethodTemplates[name].try_emplace(mt.Fn->getName(), mt);
  auto t = MethodTemplates.find(name);
  return t == MethodTemplates.end() ? nullptr : &t->second;
}

bool Sema::findMethodTemplate(ast::ClassType *ct, ast::StructType *st,
                              const std::string &name, FoundMethodTemplate &out,
                              bool statics, ast::SourceLocation loc) {
  // The declaring class is the owner: a subclass inherits the method, and
  // its `self` there is the class that declares it.
  for (ast::ClassType *c = ct; c || st; c = c->getSuperClass()) {
    std::vector<ast::Type *> ownerArgs;
    if (const auto *of = methodTemplatesOf(c, st, ownerArgs, loc))
      if (auto it = of->find(name);
          it != of->end() && it->second.Fn->isStatic() == statics) {
        out.T = it->second;
        out.Class = c;
        out.Struct = st;
        out.OwnerArgs = std::move(ownerArgs);
        return true;
      }
    if (st || statics)
      break; // a `static fn` is its type's own
  }
  return false;
}

bool Sema::methodTemplateOwner(ast::Type *ty, const std::string &name,
                               std::string &owner) {
  auto *ct = ty ? ast::dyn_cast<ast::ClassType>(ty) : nullptr;
  auto *st = ty ? ast::dyn_cast<ast::StructType>(ty) : nullptr;
  if (!ct && !st)
    return false;
  FoundMethodTemplate found;
  if (!findMethodTemplate(ct, st, name, found) &&
      !findMethodTemplate(ct, st, name, found, /*statics=*/true))
    return false;
  owner = found.ownerName();
  return true;
}

const ast::FuncDecl *Sema::genericStaticNamed(const std::string &callee) {
  const size_t sep = callee.rfind(names::kQualSep);
  if (sep == std::string::npos)
    return nullptr;
  const std::string owner = callee.substr(0, sep);
  const std::string member = callee.substr(sep + 2);
  const ast::Decl *tmpl = nullptr;
  if (auto c = ClassTemplates.find(owner); c != ClassTemplates.end())
    tmpl = c->second;
  else if (auto s = StructTemplates.find(owner); s != StructTemplates.end())
    tmpl = s->second;
  if (tmpl) {
    // Of a template: whichever block declares it (an instance has the one
    // whose bounds it meets).
    if (auto t = TemplateMethodTemplates.find(tmpl);
        t != TemplateMethodTemplates.end())
      for (const MethodTemplate &mt : t->second)
        if (mt.Fn->isStatic() && mt.Fn->getName() == member)
          return mt.Fn;
    return nullptr;
  }
  if (isOthersPrivateType(owner))
    return nullptr;
  ast::ClassType *ct = Ctx.lookupClassType(owner);
  ast::StructType *st = ct ? nullptr : Ctx.lookupStructType(owner);
  FoundMethodTemplate found;
  return (ct || st) &&
                 findMethodTemplate(ct, st, member, found, /*statics=*/true)
             ? found.T.Fn
             : nullptr;
}

const Sema::MemberInstance *
Sema::instantiateMember(const FoundMethodTemplate &found,
                        const std::vector<ast::Type *> &args,
                        ast::SourceLocation loc, const ast::ASTNode *written) {
  ast::FuncDecl *tmpl = found.T.Fn;
  const bool isStatic = tmpl->isStatic();
  const std::string owner = found.ownerName();
  // Named as the member of its owner the program sees: `Box<int>.map`,
  // `Point::make`.
  std::string shown = owner;
  shown += isStatic ? names::kQualSep : names::kNameSep;
  shown += tmpl->getName();
  if (!checkTypeArgs(shown, args, loc))
    return nullptr;
  if (args.size() != tmpl->getTypeParams().size()) {
    error(loc,
          std::string(isStatic ? "generic function '" : "generic method '") +
              shown + "' expects " +
              std::to_string(tmpl->getTypeParams().size()) +
              " type argument(s), got " + std::to_string(args.size()));
    return nullptr;
  }
  // Its own bounds, which may name its block's parameters (`U: Keyed<T>`):
  // resolved with both lists.  (The owner's held where the owner was made,
  // and its block's where the block joined.)
  const MethodTemplateNames &names = MethodTemplateParams.at(tmpl);
  std::vector<ast::Type *> allArgs = found.OwnerArgs;
  allArgs.insert(allArgs.end(), args.begin(), args.end());
  if (!checkBounds(tmpl, shown, names.Params, names.Own, allArgs, loc))
    return nullptr;

  // The instantiation's name is its owner's and its own: one per program,
  // whichever module asks for it.
  const std::string memberName = instantiationName(tmpl->getName(), args);
  std::string instName = owner;
  instName += isStatic ? names::kQualSep : names::kNameSep;
  instName += memberName;
  noteInstantiationRequest(instName);
  if (auto it = MemberInstantiations.find(instName);
      it != MemberInstantiations.end()) {
    if (it->second.Fn)
      return &it->second;
    ++SuppressedFollowOns; // reported at its first request
    return nullptr;
  }
  auto failed = [&] {
    MemberInstantiations[instName] = MemberInstance{};
    return nullptr;
  };
  ast::Type *ownerTy = found.Class ? static_cast<ast::Type *>(found.Class)
                                   : static_cast<ast::Type *>(found.Struct);
  if (instantiationDepth() >= kMaxInstantiationDepth) {
    error(loc, "instantiating '" + instName +
                   "' exceeds the maximum instantiation depth (" +
                   std::to_string(kMaxInstantiationDepth) + ")");
    return failed();
  }
  const std::vector<uint64_t> *argParams = writtenArgParams(written);
  if (!checkGrowingInstantiation(tmpl, instName, argParams, loc))
    return failed();

  // One copy, with the owner's arguments (by the block's names for them)
  // and the member's own put in together.
  const ModuleTemplates *origin = foreignOrigin(tmpl);
  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < names.Params.size() && i < allArgs.size(); ++i)
    subst[*names.Params[i]] = allArgs[i];
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  // (Its written type arguments are followed by its own parameters, which
  // are what its name is measured by: argumentNestings.)
  cloner.recordTypeArgs(WrittenTypeArgs, tmpl->getTypeParams(), instName);
  ast::FuncDecl *clone =
      cloner.cloneFuncDecl(tmpl, isStatic ? instName : memberName);

  const InstantiationFrame request = frameAt(instName, loc);
  InstantiationStack.push_back(request);
  const InstantiatingTemplate self{tmpl, &Ctx.intern(instName), argParams};
  InstantiatingTemplates.push_back(self);
  bool ok = true;
  ast::MethodDecl *md = nullptr;
  {
    OriginSource inOrigin(*this, origin);
    TypeParamScope scope(*this, names.Params);
    MakingInstance making(*this, tmpl, allArgs);
    if (isStatic)
      ok = declareFunctionSignature(clone);
    else
      md = declareMethod(clone, ownerTy, nullptr, ast::MethodDecl::Impl, ok);
  }
  InstantiatingTemplates.pop_back();
  InstantiationStack.pop_back();
  if (!ok || (!isStatic && !md))
    return failed();

  PendingInstantiation pending;
  pending.Name = instName;
  pending.Depth = instantiationDepth() + 1;
  pending.RequestLoc = loc;
  pending.Origin = origin;
  pending.RequestFile = request.File;
  pending.RequestLines = request.Lines;
  for (const std::string *p : names.Params)
    pending.TypeParams.insert(*p);
  pending.Args = std::move(allArgs);
  pending.Templates = InstantiatingTemplates;
  pending.Templates.push_back(self);
  if (isStatic) {
    // Another module's template's: the instantiation's, not this module's
    // to export (declareInstantiationStatics).
    if (origin)
      ImportedTypeOrigins[clone->getName()] = origin->Module;
    pending.Func = clone;
    (origin ? ForeignInstantiatedFuncDecls : InstantiatedFuncDecls)
        .push_back(clone);
  } else {
    if (found.Class)
      found.Class->addMethod(md);
    else
      found.Struct->addMethod(md);
    // Its body is checked as a method of its owner: a declaration of the
    // owner holding this method alone, which is never the one a walk of
    // the owner's methods is in.
    if (found.Class) {
      pending.Class = Ctx.make<ast::ClassDecl>(
          clone->getLocation(), Ctx.intern(owner), Ctx.intern(""),
          std::vector<ast::VarDecl *>{}, std::vector<ast::FuncDecl *>{clone});
    } else {
      pending.Struct =
          Ctx.make<ast::StructDecl>(clone->getLocation(), Ctx.intern(owner),
                                    std::vector<ast::VarDecl *>{});
      pending.Struct->addMethod(clone);
    }
    InstantiatedMethods.push_back({owner, clone, md, ownerTy});
  }
  if (origin)
    noteForeignSource(clone, *origin);
  queueInstantiation(std::move(pending));
  return &(MemberInstantiations[instName] = MemberInstance{clone, md});
}

bool Sema::inferMemberTypeArgs(const FoundMethodTemplate &found,
                               const std::vector<ast::Expr *> &argExprs,
                               const std::vector<ast::Type *> &argTypes,
                               ast::Type *expected, ast::SourceLocation loc,
                               std::vector<ast::Type *> &out) {
  // The member's own parameters, from the arguments, the result and the
  // type the call flows into; the owner's are the receiver's, or written.
  ast::FuncDecl *tmpl = found.T.Fn;
  const bool isStatic = tmpl->isStatic();
  std::string shown = found.ownerName();
  shown += isStatic ? names::kQualSep : names::kNameSep;
  shown += tmpl->getName();
  std::unordered_map<std::string, ast::Type *> ownerSubst;
  const auto &blockParams = found.T.Impl->getTypeParams();
  for (size_t i = 0; i < blockParams.size() && i < found.OwnerArgs.size(); ++i)
    ownerSubst[*blockParams[i]] = found.OwnerArgs[i];
  ast::ASTCloner inOwner(Ctx, std::move(ownerSubst), nullptr,
                         foreignOrigin(tmpl) != nullptr);
  std::vector<ast::Type *> paramTypes;
  for (const ast::Param &p : tmpl->getParams())
    paramTypes.push_back(inOwner.cloneType(p.ParamType));
  ast::Type *result = tmpl->getReturnType()
                          ? inOwner.cloneType(tmpl->getReturnType())
                          : nullptr;
  const auto forms = inferenceForms(argExprs, [&](size_t i) {
    return i < tmpl->getParams().size() ? tmpl->getParams()[i].Mode
                                        : ast::ParamMode::Value;
  });
  return inferTypeArgs(shown, tmpl->getTypeParams(), paramTypes, argTypes, loc,
                       out, isStatic ? "" : tmpl->getName() + "<...>(...)",
                       result, expected, forms);
}

bool Sema::resolveGenericStatic(ast::CallExpr *node,
                                const FoundMethodTemplate &found,
                                std::vector<ast::Type *> typeArgs,
                                const std::vector<ast::Type *> &argTypes,
                                ast::Type *expected) {
  const ast::SourceLocation loc = node->getLocation();
  if (typeArgs.empty() &&
      !inferMemberTypeArgs(found, node->getArguments(), argTypes, expected, loc,
                           typeArgs))
    return false;
  const MemberInstance *made = instantiateMember(
      found, typeArgs, loc, node->hasTypeArgs() ? node : nullptr);
  if (!made)
    return false;
  node->setCalleeName(made->Fn->getName()); // interned by the ASTContext
  return true;
}

ast::MethodDecl *
Sema::resolveGenericMethodCall(ast::MethodCallExpr *node, ast::ClassType *ct,
                               ast::StructType *st,
                               const std::vector<ast::Type *> &argTypes,
                               ast::Type *expected, bool &failed) {
  failed = false;
  FoundMethodTemplate found;
  if (!findMethodTemplate(ct, st, node->getMethodName(), found))
    return nullptr;
  failed = true;
  const ast::SourceLocation loc = node->getLocation();
  std::vector<ast::Type *> typeArgs;
  for (size_t i = 0; i < node->getTypeArgs().size(); ++i) {
    ast::Type *t = resolveTypeArgument(
        node->getTypeArgs()[i], loc,
        "type argument " + std::to_string(i + 1) + " of '" + found.ownerName() +
            names::kNameSep + found.T.Fn->getName() + "'");
    if (!t)
      return nullptr;
    typeArgs.push_back(t);
  }
  if (typeArgs.empty() &&
      !inferMemberTypeArgs(found, node->getArguments(), argTypes, expected, loc,
                           typeArgs))
    return nullptr;
  const MemberInstance *made = instantiateMember(
      found, typeArgs, loc, node->hasTypeArgs() ? node : nullptr);
  if (!made)
    return nullptr;
  node->setMethodName(made->Method->getName());
  failed = false;
  return made->Method;
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

void Sema::noteForeignSource(const ast::Decl *d,
                             const ModuleTemplates &origin) {
  ForeignInstantiationSources[d] = {origin.File, origin.Lines, origin.Owner,
                                    origin.Module};
}

Sema::TypeParamScope::TypeParamScope(
    Sema &s, const std::vector<const std::string *> &params)
    : S(s), Saved(s.CurrentTypeParams),
      SavedMethod(std::exchange(s.ImplScopeMethod, nullptr)) {
  for (const std::string *p : params)
    Params.insert(*p);
  S.CurrentTypeParams = &Params;
}

void Sema::noteImplTypeParams(const ast::ImplDecl *im,
                              const ast::ImplDecl *copy) {
  for (const ast::FuncDecl *fn : copy->getMethods()) {
    ImplMethodTypeParams[fn] = &im->getTypeParams();
    ImplMethodBlocks[fn] = im;
  }
}

void Sema::implTypeParamScope(std::optional<TypeParamScope> &scope,
                              const ast::FuncDecl *fn) {
  if (auto it = ImplMethodTypeParams.find(fn);
      it != ImplMethodTypeParams.end()) {
    scope.emplace(*this, *it->second);
    ImplScopeMethod = fn; // restored with the scope
  }
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

void Sema::recordInstantiationSite(const ast::Decl *tmpl,
                                   const InstantiationFrame &request) {
  InstantiationSites.try_emplace(
      request.Name, InstantiationSite{tmpl, request, InstantiationStack});
}

/// The name template @p tmpl, a generic class, struct or enum, declares.
static const std::string &templateDeclName(const ast::Decl *tmpl) {
  if (const auto *cd = ast::dyn_cast<ast::ClassDecl>(tmpl))
    return cd->getName();
  if (const auto *sd = ast::dyn_cast<ast::StructDecl>(tmpl))
    return sd->getName();
  return ast::cast<ast::EnumDecl>(tmpl)->getName();
}

std::string Sema::instanceNameOf(const ast::Decl *tmpl, const std::string &name,
                                 const std::vector<ast::Type *> &args) {
  const auto *fn = ast::dyn_cast<ast::FuncDecl>(tmpl);
  if (!fn)
    return instantiationName(templateDeclName(tmpl), args);
  // As instantiateFunction names it: per module.
  const ModuleTemplates *origin = foreignOrigin(tmpl);
  return origin ? hiddenQualifier(origin->Module) + names::kQualSep +
                      instantiationName(fn->getName(), args)
                : instantiationName(name, args);
}

/// How many type parameters template @p tmpl declares.
static size_t typeParamCount(const ast::Decl *tmpl) {
  if (const auto *fn = ast::dyn_cast<ast::FuncDecl>(tmpl))
    return fn->getTypeParams().size();
  if (const auto *cd = ast::dyn_cast<ast::ClassDecl>(tmpl))
    return cd->getTypeParams().size();
  if (const auto *sd = ast::dyn_cast<ast::StructDecl>(tmpl))
    return sd->getTypeParams().size();
  return ast::cast<ast::EnumDecl>(tmpl)->getTypeParams().size();
}

const std::vector<uint64_t> *
Sema::writtenArgParams(const ast::ASTNode *written) const {
  auto it = written ? WrittenTypeArgs.find(written) : WrittenTypeArgs.end();
  if (it == WrittenTypeArgs.end())
    return nullptr;
  const ast::WrittenTypeArgs &w = it->second;
  const std::vector<uint64_t> &params =
      written == OwnerListOf ? w.OwnerParams : w.Params;
  // Written with no type parameter (`L0<int>`), it names the same types
  // wherever it is; with one, the parameters are its copy's, which must be
  // the instantiation it is requested in.
  if (std::all_of(params.begin(), params.end(),
                  [](uint64_t p) { return p == 0; }) ||
      (!InstantiatingTemplates.empty() &&
       InstantiatingTemplates.back().Name == w.Copy))
    return &params;
  return nullptr;
}

// static
std::vector<size_t> Sema::argumentNestings(const std::string &name) {
  std::vector<size_t> nestings;
  // A generic method's or `static fn`'s instantiation (#353) is measured by
  // its own arguments, after its owner's name (`Box<int>.map<Str>`).
  const size_t owner = names::ownerTypeName(name).size();
  const size_t args = name.find('<', owner < name.size() ? owner : 0);
  if (args == std::string::npos)
    return nestings;
  size_t open = 0;
  size_t most = 0;
  // Up to the `>` that closes the arguments, the name's last character.
  for (size_t i = args + 1; i + 1 < name.size(); ++i) {
    const char c = name[i];
    if (c == '(' || c == '<') {
      most = std::max(most, ++open);
    } else if (c == ')' || (c == '>' && name[i - 1] != '-')) {
      --open; // a function type's `->` closes nothing
    } else if (c == ',' && open == 0) {
      nestings.push_back(most);
      most = 0;
    }
  }
  nestings.push_back(most);
  return nestings;
}

bool Sema::argumentsGrow(size_t anchor, const std::string &instName,
                         const std::vector<uint64_t> *argParams) const {
  const std::vector<size_t> was =
      argumentNestings(*InstantiatingTemplates[anchor].Name);
  const std::vector<size_t> now = argumentNestings(instName);
  const size_t n = now.size();
  const size_t deepest =
      was.empty() ? 0 : *std::max_element(was.begin(), was.end());
  if (was.size() != n || n > 64)
    return std::any_of(now.begin(), now.end(),
                       [&](size_t d) { return d > deepest; });
  // Which of the anchor's arguments each argument of the instantiations
  // after it, and then of this one, holds: Known, through arguments
  // written with the type parameters of the one before; Unknown, through
  // one that was not (inferred, say), which may hold any of them.
  struct Held {
    uint64_t Known;
    uint64_t Unknown;
  };
  const uint64_t all = n == 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1;
  std::vector<Held> held(n);
  for (size_t p = 0; p < n; ++p)
    held[p] = {uint64_t{1} << p, 0};
  auto pass = [&](const std::vector<uint64_t> *params, size_t count) {
    std::vector<Held> next(count, Held{0, all});
    if (params && params->size() == count)
      for (size_t i = 0; i < count; ++i) {
        next[i] = {0, 0};
        for (size_t p = 0; p < held.size() && p < 64; ++p)
          if ((*params)[i] >> p & 1) {
            next[i].Known |= held[p].Known;
            next[i].Unknown |= held[p].Unknown;
          }
      }
    held = std::move(next);
  };
  for (size_t k = anchor + 1; k < InstantiatingTemplates.size(); ++k)
    pass(InstantiatingTemplates[k].ArgParams,
         typeParamCount(InstantiatingTemplates[k].Template));
  pass(argParams, n);
  // Argument p of the anchor goes into argument i of this one, and grew on
  // the way when i is nested more deeply: than p, when i holds p as
  // written, which puts p inside it; than every argument, when it may.
  std::vector<uint64_t> into(n, 0);
  std::vector<uint64_t> grew(n, 0);
  for (size_t i = 0; i < n; ++i)
    for (size_t p = 0; p < n; ++p) {
      const bool known = held[i].Known >> p & 1;
      const bool unknown = held[i].Unknown >> p & 1;
      if (!known && !unknown)
        continue;
      into[p] |= uint64_t{1} << i;
      if ((known && now[i] > was[p]) || (unknown && now[i] > deepest))
        grew[p] |= uint64_t{1} << i;
    }
  // It grows ever when what grew goes back to where it came from: `C<(T,
  // T)>` in `C<T>`, or `P<B, (A, A)>` in `P<A, B>`, but not `P<A, (A, A)>`,
  // whose own is itself.  reach[i]: the arguments i goes into, at once or
  // through others.
  std::vector<uint64_t> reach = into;
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      uint64_t r = reach[i];
      for (size_t j = 0; j < n; ++j)
        if (reach[i] >> j & 1)
          r |= reach[j];
      changed |= r != reach[i];
      reach[i] = r;
    }
  }
  for (size_t p = 0; p < n; ++p)
    for (size_t i = 0; i < n; ++i)
      if ((grew[p] >> i & 1) && (i == p || (reach[i] >> p & 1)))
        return true;
  return false;
}

bool Sema::checkGrowingInstantiation(const ast::Decl *tmpl,
                                     const std::string &instName,
                                     const std::vector<uint64_t> *argParams,
                                     ast::SourceLocation loc) {
  if (instName.size() <= kMaxGrowingInstantiationName)
    return true;
  auto inner = std::find_if(
      InstantiatingTemplates.rbegin(), InstantiatingTemplates.rend(),
      [&](const InstantiatingTemplate &t) { return t.Template == tmpl; });
  // Not inside one of its own, or not growing from it: its arguments
  // rearranged (`Pair<B, A>` inside `Pair<A, B>`, whose own `Pair<B, A>`
  // is the first again), or made of none of its (an `X<…>` reached from
  // `X<int>` through `L0<int>`).
  if (inner == InstantiatingTemplates.rend() ||
      !argumentsGrow(InstantiatingTemplates.rend() - inner - 1, instName,
                     argParams))
    return true;
  const std::string shown = displayName(instName);
  if (shown.size() <= kMaxGrowingInstantiationName)
    return true; // only its hidden qualifier is long
  // Shown cut short, at a character's first byte.
  size_t cut = 60;
  while ((static_cast<unsigned char>(shown[cut]) & 0xC0) == 0x80)
    --cut;
  const auto *fn = ast::dyn_cast<ast::FuncDecl>(tmpl);
  const bool method = fn && !fn->isStatic() && MethodTemplateParams.count(fn);
  const char *kind = method                            ? "method"
                     : fn                              ? "function"
                     : ast::isa<ast::ClassDecl>(tmpl)  ? "class"
                     : ast::isa<ast::StructDecl>(tmpl) ? "struct"
                                                       : "enum";
  error(loc, "instantiating '" + shown.substr(0, cut) +
                 "...' inside an instantiation of '" +
                 (fn ? fn->getName() : templateDeclName(tmpl)) +
                 "' exceeds the maximum instantiation name length (" +
                 std::to_string(kMaxGrowingInstantiationName) +
                 " characters; this one has " + std::to_string(shown.size()) +
                 "); a generic " + kind +
                 " may not instantiate itself with an ever-growing type "
                 "argument");
  return false;
}

void Sema::reportInstantiationClash(const ast::Decl *tmpl,
                                    const std::string &instName,
                                    ast::SourceLocation loc) {
  if (!ReportedInstantiationClashes.insert(instName).second) {
    ++SuppressedFollowOns;
    return;
  }
  // A template by its module's name, and the file its declaration is in.
  auto shown = [&](const ast::Decl *t) {
    const ModuleTemplates *origin = foreignOrigin(t);
    return origin ? origin->Module + names::kQualSep + templateDeclName(t)
                  : templateDeclName(t);
  };
  const std::string file = Diags.getSourceName();
  const std::vector<std::string> *lines = Diags.getSourceLines();
  auto noteDecl = [&](const ast::Decl *t) {
    const ModuleTemplates *origin = foreignOrigin(t);
    Diags.setSourceInfo(origin ? origin->File : OwnSourceName,
                        origin ? origin->Lines : OwnSourceLines);
    Diags.note(t->getLocation(), "'" + shown(t) + "' is declared here");
  };
  auto site = InstantiationSites.find(instName);
  if (site == InstantiationSites.end() || site->second.Template == tmpl) {
    // A type an import gave the program, an instantiation of a template of
    // another module (::io's `Result<Str, Error>`, of ::result's).
    const char *kind = Ctx.lookupClassType(instName)    ? "class"
                       : Ctx.lookupStructType(instName) ? "struct"
                       : Ctx.lookupEnumType(instName)   ? "enum"
                                                        : "type";
    error(loc, "instantiation '" + instName + "' of '" + shown(tmpl) +
                   "' conflicts with an imported " + kind +
                   " of the same name (type names are global across imports)");
    auto origin = ImportedTypeOrigins.find(instName);
    if (origin == ImportedTypeOrigins.end())
      return;
    const std::string base = instName.substr(0, instName.find('<'));
    const std::string hidden =
        hiddenQualifier(origin->second) + names::kQualSep + base;
    const ast::Decl *other = nullptr;
    if (auto c = ClassTemplates.find(hidden); c != ClassTemplates.end())
      other = c->second;
    else if (auto s = StructTemplates.find(hidden); s != StructTemplates.end())
      other = s->second;
    else if (auto e = EnumTemplates.find(hidden); e != EnumTemplates.end())
      other = e->second;
    if (other)
      noteDecl(other);
    if (auto at = ImportedTypeImports.find(instName);
        at != ImportedTypeImports.end()) {
      Diags.setSourceInfo(OwnSourceName, OwnSourceLines);
      Diags.note(at->second, std::string("the ") + kind + " '" + instName +
                                 "' of module '" + origin->second +
                                 "' comes in through this import");
    }
    Diags.setSourceInfo(file, lines);
    return;
  }
  // Another template's instantiation: the error is at a request this
  // module's own code made, of its own template when it is one, and the
  // notes at the other template's.
  auto rank = [&](const InstantiationSite &s) {
    const bool own =
        s.Request.Lines == OwnSourceLines && s.Request.File == OwnSourceName;
    return (own ? 2 : 0) + (foreignOrigin(s.Template) ? 0 : 1);
  };
  const InstantiationSite here{tmpl, frameAt(instName, loc),
                               InstantiationStack};
  const bool atOther = rank(site->second) > rank(here);
  const InstantiationSite &subject = atOther ? site->second : here;
  const InstantiationSite &rival = atOther ? here : site->second;
  std::vector<InstantiationFrame> frames =
      std::exchange(InstantiationStack, subject.Frames);
  Diags.setSourceInfo(subject.Request.File, subject.Request.Lines);
  error(subject.Request.RequestLoc,
        "instantiation '" + instName + "' of '" + shown(subject.Template) +
            "' conflicts with the instantiation '" + instName + "' of '" +
            shown(rival.Template) + "' (type names are global across imports)");
  InstantiationStack = std::move(frames);
  noteDecl(rival.Template);
  Diags.setSourceInfo(rival.Request.File, rival.Request.Lines);
  Diags.note(rival.Request.RequestLoc, "'" + shown(rival.Template) +
                                           "' is instantiated as '" + instName +
                                           "' here");
  for (auto it = rival.Frames.rbegin(); it != rival.Frames.rend(); ++it) {
    Diags.setSourceInfo(it->File, it->Lines);
    Diags.note(it->RequestLoc, "in instantiation of '" + displayName(it->Name) +
                                   "' requested here");
  }
  Diags.setSourceInfo(file, lines);
}

ast::ClassType *Sema::instantiateClass(const std::string &name,
                                       const std::vector<ast::Type *> &args,
                                       ast::SourceLocation loc,
                                       const ast::ASTNode *written) {
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
  noteInstantiationRequest(instName);
  if (auto cIt = ClassInstantiations.find(instName);
      cIt != ClassInstantiations.end()) {
    // Cached (nullptr records an earlier failure) -- unless it is another
    // template's of the same name.
    auto info = cIt->second ? ClassInstantiationInfo.find(cIt->second)
                            : ClassInstantiationInfo.end();
    if (info == ClassInstantiationInfo.end() || info->second.Template == tmpl)
      return cIt->second;
    reportInstantiationClash(tmpl, instName, loc);
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
      ClassInstantiations[instName] = existing;
      // Its methods are compiled there; what they do is read from a copy.
      copyImportedInstance(existing, loc);
      return existing;
    }
    reportInstantiationClash(tmpl, instName, loc);
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }
  if (Ctx.lookupType(instName)) {
    // A struct (or another type) of that name, from a template of another
    // module or declared outright: the same rule as for a struct template.
    reportInstantiationClash(tmpl, instName, loc);
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
  const std::vector<uint64_t> *argParams = writtenArgParams(written);
  if (!checkGrowingInstantiation(tmpl, instName, argParams, loc)) {
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }

  // Clone the template with the type parameters substituted.
  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  cloner.recordTypeArgs(WrittenTypeArgs, tmpl->getTypeParams(), instName);
  ast::ClassDecl *clone = cloner.cloneClassDecl(tmpl, instName);
  // And its `impl` blocks, each by its own names for the type parameters.
  const InstantiationFrame request = frameAt(instName, loc);
  const size_t late = LateImpls.size();
  if (auto it = TemplateImpls.find(tmpl); it != TemplateImpls.end())
    for (auto *im : it->second) {
      // A block bounded on its parameters (`impl List<T: Cloneable>`) joins
      // only the instances whose arguments meet the bounds (#369): now, or
      // once what the bounds ask can be told (joinLateImpls).
      if (joinsLater(im, /*ofClass=*/true, args)) {
        LateImpls.push_back(
            {tmpl, im, instName, args, origin, InstantiationStack, {}});
        LateImpls.back().Frames.push_back(request);
        continue;
      }
      if (implBoundsHold(im, args))
        cloneImplFor(im, instName, args, origin != nullptr);
    }

  recordInstantiationSite(tmpl, request);
  InstantiationStack.push_back(request);
  const InstantiatingTemplate self{tmpl, &clone->getName(), argParams};
  InstantiatingTemplates.push_back(self);
  std::optional<MakingInstance> making(std::in_place, *this, tmpl, args);
  OriginSource inOrigin(*this, origin);
  bool ok = true;

  // Superclass (phases 2-3 of checkClassDecls, for this one class).
  ast::ClassType *superClass = nullptr;
  if (clone->hasSuperClass()) {
    const auto &superName = clone->getSuperClassName();
    superClass = Ctx.lookupClassType(superName);
    if (!superClass) {
      reportUndefinedSuperclass(superName, instName, tmpl,
                                clone->getLocation());
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
  ct->setFinal(tmpl->isFinal());
  // Cache before populating: a self-referential field (`next: Node<T>`)
  // resolves to this very type instead of recursing.
  ClassInstantiations[instName] = ct;
  ClassInstantiationInfo[ct] = InstantiationInfo{tmpl, args};
  InstancesMadeHere.insert(ct);
  HalfMade.insert(ct);
  // Another module's template makes a class of that module: so its
  // importers see it, wherever it was instantiated.
  if (origin)
    ImportedTypeOrigins[instName] = origin->Module;

  {
    TypeParamScope params(*this, tmpl->getTypeParams());
    if (!populateClassType(clone, ct))
      ok = false;
    if (ok)
      declareConstructor(clone, ct);
  }
  // A copy: checking one may ask for an instance, whose blocks join the
  // table this list is in.
  const std::vector<ast::ImplDecl *> traitImpls = ImplsByClass[instName];
  for (auto *im : traitImpls) {
    if (!ok || !im->getTrait())
      continue;
    if (!ImplsChecked)
      PendingTraitImpls.push_back({im, ct, request, origin});
    else if (!checkTraitImpl(im, ct))
      ok = false;
  }

  PendingInstantiation pending;
  pending.Class = clone;
  pending.Name = instName;
  pending.Depth = instantiationDepth();
  pending.RequestLoc = loc;
  pending.Origin = origin;
  pending.RequestFile = request.File;
  pending.RequestLines = request.Lines;
  for (auto *p : tmpl->getTypeParams())
    pending.TypeParams.insert(*p);
  pending.Args = args;
  pending.Templates = InstantiatingTemplates;
  // Its blocks that wait are its own from here: before the outermost
  // instance is whole (making), which joins them.
  if (ok)
    for (size_t i = late; i < LateImpls.size(); ++i)
      if (LateImpls[i].Inst == instName)
        LateImpls[i].Base = pending;
  HalfMade.erase(ct);
  making.reset();
  InstantiatingTemplates.pop_back();
  InstantiationStack.pop_back();

  if (!ok) {
    ClassInstantiations[instName] = nullptr;
    return nullptr;
  }

  PendingInstantiations.push_back(pending);
  (origin ? ForeignInstantiatedClassDecls : InstantiatedClassDecls)
      .push_back(clone);
  if (origin)
    noteForeignSource(clone, *origin);
  declareInstantiationStatics(instName, ct, pending);
  return ct;
}

void Sema::declareInstantiationStatics(const std::string &instName,
                                       ast::Type *type,
                                       const PendingInstantiation &base,
                                       const ast::ImplDecl *only) {
  auto found = ImplsByClass.find(instName);
  if (found == ImplsByClass.end())
    return;
  // A copy: a signature may ask for an instance, whose blocks join the
  // table this list is in.
  const std::vector<ast::ImplDecl *> impls = found->second;
  // Declared in the template's module, as the bodies are checked.
  InstantiationStack.push_back(
      {base.Name, base.RequestLoc, base.RequestFile, base.RequestLines});
  std::vector<InstantiatingTemplate> savedTemplates =
      std::exchange(InstantiatingTemplates, base.Templates);
  OriginSource inOrigin(*this, base.Origin);
  std::optional<MakingInstance> making;
  if (!base.Templates.empty())
    making.emplace(*this, base.Templates.back().Template, base.Args);
  for (auto *im : impls) {
    if (im->getTrait() || (only && im != only))
      continue;
    for (auto *fn : im->getMethods()) {
      if (!fn->isStatic())
        continue;
      const std::string name = instName + names::kQualSep + fn->getName();
      if (rejectReservedDestroy(fn, type)) {
        ErroneousNames.insert(name); // its calls are follow-ons
        continue;
      }
      if (std::string has; methodOf(type, fn->getName()) ||
                           methodTemplateOwner(type, fn->getName(), has)) {
        error(fn->getLocation(), std::string(typeKindWord(type)) + " '" +
                                     instName + "' already has a method '" +
                                     fn->getName() + "'");
        ErroneousNames.insert(name); // its calls are follow-ons
        continue;
      }
      fn->setName(Ctx.intern(name));
      std::optional<TypeParamScope> params;
      implTypeParamScope(params, fn);
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
      if (params)
        p.TypeParams = *CurrentTypeParams; // its body sees the block's names
      queueInstantiation(std::move(p));
      (base.Origin ? ForeignInstantiatedFuncDecls : InstantiatedFuncDecls)
          .push_back(fn);
      if (base.Origin)
        noteForeignSource(fn, *base.Origin);
    }
  }
  InstantiatingTemplates = std::move(savedTemplates);
  InstantiationStack.pop_back();
}

std::string Sema::instantiateFunction(const std::string &name,
                                      const std::vector<ast::Type *> &args,
                                      ast::SourceLocation loc,
                                      const ast::ASTNode *written) {
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
  noteInstantiationRequest(instName);
  if (auto fIt = FuncInstantiations.find(instName);
      fIt != FuncInstantiations.end()) {
    if (fIt->second)
      return instName;
    ++SuppressedFollowOns; // reported at its first request
    return "";
  }

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
  const std::vector<uint64_t> *argParams = writtenArgParams(written);
  if (!checkGrowingInstantiation(tmpl, instName, argParams, loc)) {
    FuncInstantiations[instName] = false;
    return "";
  }

  std::unordered_map<std::string, ast::Type *> subst;
  for (size_t i = 0; i < args.size(); ++i)
    subst[*tmpl->getTypeParams()[i]] = args[i];
  ast::ASTCloner cloner(Ctx, std::move(subst), nullptr, origin != nullptr);
  cloner.recordTypeArgs(WrittenTypeArgs, tmpl->getTypeParams(), instName);
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
  const InstantiatingTemplate self{tmpl, &clone->getName(), argParams};
  InstantiatingTemplates.push_back(self);
  bool ok;
  {
    OriginSource inOrigin(*this, origin);
    TypeParamScope params(*this, tmpl->getTypeParams());
    MakingInstance making(*this, tmpl, args);
    ok = declareFunctionSignature(clone);
  }
  InstantiatingTemplates.pop_back();
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
  pending.Args = args;
  pending.Templates = InstantiatingTemplates;
  pending.Templates.push_back(self);
  queueInstantiation(std::move(pending));
  (origin ? ForeignInstantiatedFuncDecls : InstantiatedFuncDecls)
      .push_back(clone);
  if (origin)
    noteForeignSource(clone, *origin);
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
  // `mut T`, `shared T` & co. (the prototype): the form is the slot's, the
  // type parameter is the plain type.
  // Against a part of the same form (`(shared T) -> int` against a
  // `(shared Node) -> int`, #368), T is the plain type, as the form is the
  // pattern's.
  if (auto *qt = ast::dyn_cast<ast::QualifiedType>(pattern)) {
    if (auto *aq = ast::dyn_cast<ast::QualifiedType>(actual);
        aq && aq->getQualifier() == qt->getQualifier())
      actual = aq->getInnerType();
    return unifyTypes(qt->getInnerType(), actual, typeParams, bindings,
                      conflict);
  }

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
    // A unique argument joins a `shared` one's form (#368): it converts
    // into it (`pick(s, Node(2))` is `pick<shared Node>`).
    auto bare = [](ast::Type *t) {
      auto *q = ast::dyn_cast<ast::QualifiedType>(t);
      return q && q->getQualifier().isSharedForm() ? q->getInnerType()
                                                   : nullptr;
    };
    if (bare(it->second) && typesEqual(bare(it->second), actual))
      return true;
    if (bare(actual) && typesEqual(bare(actual), it->second)) {
      it->second = actual;
      return true;
    }
    // A value whose unique parts another argument holds `shared` joins its
    // parts' forms (`replace(t, (0, Node(7)))` with t: `(int, shared mut
    // Node)`): its unique references convert into them.
    if (ast::Type *joined = joinPartForms(it->second, actual)) {
      it->second = joined;
      return true;
    }
    conflict = {ct->getName(), it->second, actual};
    return false;
  }
  // A qualified argument (#368) against anything but a type parameter: a
  // `T?` binds T to the value, qualified as the optional is; any other
  // pattern is unified with the type itself.
  if (auto *aq = ast::dyn_cast<ast::QualifiedType>(actual)) {
    ast::Type *inner = aq->getInnerType();
    if (auto *ot = ast::dyn_cast<ast::OptionalType>(pattern)) {
      if (auto *aot = ast::dyn_cast<ast::OptionalType>(inner))
        inner = aot->getInnerType();
      return unifyTypes(ot->getInnerType(),
                        Ctx.getQualifiedType(aq->getQualifier(), inner),
                        typeParams, bindings, conflict);
    }
    return unifyTypes(pattern, inner, typeParams, bindings, conflict);
  }

  if (auto *at = ast::dyn_cast<ast::ArrayType>(pattern)) {
    // A fixed-size array or a slice passed for a slice (`view T[]`) gives
    // its element type.
    // Its elements' form is the element's (#368): `view T[]` against a
    // `shared Node[2]` binds T to `shared Node`.
    if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(actual))
      return unifyTypes(
          at->getElementType(),
          Ctx.getQualifiedType(ft->getElemQual(), ft->getElementType()),
          typeParams, bindings, conflict);
    if (auto *st = ast::dyn_cast<ast::SliceType>(actual))
      return unifyTypes(
          at->getElementType(),
          Ctx.getQualifiedType(st->getElemQual(), st->getElementType()),
          typeParams, bindings, conflict);
    return true;
  }

  if (auto *tt = ast::dyn_cast<ast::TupleType>(pattern)) {
    // `(T, int)` against a tuple of the same arity: unify element-wise.  A
    // mismatched arity is left to the argument check.
    auto *att = ast::dyn_cast<ast::TupleType>(actual);
    if (!att || att->getArity() != tt->getArity())
      return true;
    for (size_t i = 0; i < tt->getArity(); ++i)
      if (!unifyTypes(
              tt->getElementType(i),
              Ctx.getQualifiedType(att->getElemQual(i), att->getElementType(i)),
              typeParams, bindings, conflict))
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
      if (!unifyTypes(
              ft->getParamType(i),
              Ctx.getQualifiedType(aft->getParamQual(i), aft->getParamType(i)),
              typeParams, bindings, conflict))
        return false;
    }
    return unifyTypes(
        ft->getReturnType(),
        Ctx.getQualifiedType(aft->getResultQual(), aft->getReturnType()),
        typeParams, bindings, conflict);
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

ast::Type *Sema::joinPartForms(ast::Type *a, ast::Type *b) {
  if (a == b)
    return a;
  // The stronger of two parts' forms: one, or a unique one and a `shared`
  // one; any other pair joins to none.
  auto join = [](ast::Qualifier x, ast::Qualifier y, ast::Qualifier &out) {
    if (x == y || y.isUnique())
      out = x;
    else if (x.isUnique() && y.Form == ast::QualForm::Shared)
      out = y;
    else
      return false;
    return true;
  };
  if (auto *oa = ast::dyn_cast<ast::OptionalType>(a))
    if (auto *ob = ast::dyn_cast<ast::OptionalType>(b)) {
      ast::Type *inner = joinPartForms(oa->getInnerType(), ob->getInnerType());
      return inner ? Ctx.getOptionalType(inner) : nullptr;
    }
  if (auto *fa = ast::dyn_cast<ast::FixedArrayType>(a))
    if (auto *fb = ast::dyn_cast<ast::FixedArrayType>(b);
        fb && fa->getLength() == fb->getLength()) {
      ast::Type *elem =
          joinPartForms(fa->getElementType(), fb->getElementType());
      ast::Qualifier q;
      if (!elem || !join(fa->getElemQual(), fb->getElemQual(), q))
        return nullptr;
      return Ctx.getFixedArrayType(elem, fa->getLength(), q);
    }
  auto *ta = ast::dyn_cast<ast::TupleType>(a);
  auto *tb = ast::dyn_cast<ast::TupleType>(b);
  if (!ta || !tb || ast::isa<ast::StructType>(ta) ||
      ast::isa<ast::StructType>(tb) || ta->getArity() != tb->getArity())
    return nullptr;
  std::vector<ast::Type *> elems;
  std::vector<ast::Qualifier> quals;
  for (size_t i = 0; i < ta->getArity(); ++i) {
    ast::Type *e = joinPartForms(ta->getElementType(i), tb->getElementType(i));
    ast::Qualifier q;
    if (!e || !join(ta->getElemQual(i), tb->getElemQual(i), q))
      return nullptr;
    elems.push_back(e);
    quals.push_back(q);
  }
  return Ctx.getTupleType(std::move(elems), std::move(quals));
}

bool Sema::inferTypeArgs(const std::string &templateName,
                         const std::vector<const std::string *> &typeParams,
                         const std::vector<ast::Type *> &paramTypes,
                         const std::vector<ast::Type *> &argTypes,
                         ast::SourceLocation loc, std::vector<ast::Type *> &out,
                         const std::string &spelling, ast::Type *resultPattern,
                         ast::Type *expected,
                         const std::vector<ast::Qualifier> &argForms) {
  // Named as the program spells it, not by another module's hidden
  // qualifier (the name of a call in its template).
  const std::string shown = displayName(templateName);
  const std::string hint =
      "specify the type arguments explicitly: '" +
      (spelling.empty() ? shown + "<...>(...)" : spelling) + "'";
  if (paramTypes.size() != argTypes.size()) {
    error(loc, "cannot infer the type arguments of '" + shown +
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
    // A by-value or `inout` `T` (or `T?`) parameter given a `shared`
    // reference binds T to its form (#368): `twice(s)` with `s: shared Node`
    // is `twice<shared Node>`, and `mem::replace(m, n)` with `m: shared mut
    // Node` gives back a `shared mut Node`, never the object as a unique
    // one.  A `view` binds the plain type.
    ast::Type *actual = argTypes[i];
    if (i < argForms.size() && !argForms[i].isUnique() && actual) {
      // Through a written qualifier too (`x: mut T` given a `shared mut
      // Node` binds T to it, which `mut` joins); one of the argument's own
      // form takes the plain type (unifyTypes).
      ast::Type *p = paramTypes[i];
      if (auto *qt = p ? ast::dyn_cast<ast::QualifiedType>(p) : nullptr;
          qt && qt->getInnerType())
        p = qt->getInnerType();
      if (auto *ot = p ? ast::dyn_cast<ast::OptionalType>(p) : nullptr)
        p = ot->getInnerType();
      if (const auto *ct = p ? ast::dyn_cast<ast::ClassType>(p) : nullptr;
          ct && params.count(ct->getName()))
        actual = Ctx.getQualifiedType(argForms[i], actual);
    }
    if (!unifyTypes(paramTypes[i], actual, params, bindings, conflict)) {
      error(loc, "cannot infer type parameter '" + conflict.Param + "' of '" +
                     shown + "': deduced as both '" + typeName(conflict.First) +
                     "' and '" + typeName(conflict.Second) + "'");
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
      for (auto &[name, ty] : fromResult) {
        auto [it, fresh] = bindings.try_emplace(name, ty);
        // An argument's plain type takes the form the slot gives it
        // (#368): `res::ok(n)` into a `Result<mut Node>`, `Box(Node(1))`
        // into a `Box<shared Node>`; the argument flows into that form.
        if (fresh)
          continue;
        const auto *q = ast::dyn_cast<ast::QualifiedType>(ty);
        if ((q && q->getInnerType() == it->second) ||
            joinPartForms(it->second, ty) == ty)
          it->second = ty;
      }
  }
  out.clear();
  for (auto *p : typeParams) {
    auto it = bindings.find(*p);
    if (it == bindings.end()) {
      error(loc, "cannot infer type parameter '" + *p + "' of '" + shown +
                     "' from the call arguments");
      note(loc, hint);
      return false;
    }
    out.push_back(it->second);
  }
  return true;
}

std::vector<ast::Qualifier>
Sema::inferenceForms(const std::vector<ast::Expr *> &args,
                     const std::function<ast::ParamMode(size_t)> &modeOf) {
  std::vector<ast::Qualifier> forms(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    const ast::Qualifier q = qualifierOf(args[i]);
    if (!q.isSharedForm())
      continue;
    // A `view` binds the plain type, but in a template's code a type
    // argument's form, which the template passes on as its `T`.
    if (modeOf(i) == ast::ParamMode::View &&
        !(inInstanceCode() && q.FromArgument))
      continue;
    forms[i] = q.form();
  }
  return forms;
}

bool Sema::resolveGenericCall(ast::CallExpr *node,
                              const std::vector<ast::Type *> &argTypes,
                              ast::Type *expected) {
  const std::string &callee = node->getCalleeName();
  const ast::SourceLocation loc = node->getLocation();

  // Explicit type arguments (`first<int>(xs)`) resolve like any annotation.
  const ast::ASTNode *written = node->hasTypeArgs() ? node : nullptr;
  std::vector<ast::Type *> typeArgs;
  for (size_t i = 0; i < node->getTypeArgs().size(); ++i) {
    auto *t = resolveTypeArgument(node->getTypeArgs()[i], loc,
                                  "type argument " + std::to_string(i + 1) +
                                      " of '" + displayName(callee) + "'");
    if (!t)
      return false;
    typeArgs.push_back(t);
  }
  // And the ones written after the owner type (`Box<int>::of(1)`, #353),
  // which are the owner's.
  std::vector<ast::Type *> ownerArgs;
  const size_t ownerSep = callee.rfind(names::kQualSep);
  for (size_t i = 0; i < node->getOwnerTypeArgs().size(); ++i) {
    auto *t =
        resolveTypeArgument(node->getOwnerTypeArgs()[i], loc,
                            "type argument " + std::to_string(i + 1) + " of '" +
                                displayName(callee.substr(0, ownerSep)) + "'");
    if (!t)
      return false;
    ownerArgs.push_back(t);
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
      std::vector<ast::ParamMode> modes;
      if (init) {
        for (auto &p : init->getParams()) {
          paramTypes.push_back(here(tmpl, p.ParamType));
          modes.push_back(p.Mode);
        }
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
      const auto forms = inferenceForms(node->getArguments(), [&](size_t i) {
        return i < modes.size() ? modes[i] : ast::ParamMode::Value;
      });
      // The slot's type gives what the arguments leave open, and the forms
      // they take (`h: H<shared Node> = H(Node(2))`, #368).
      std::vector<ast::Type *> named;
      for (const std::string *p : *params)
        named.push_back(Ctx.make<ast::ClassType>(loc, *p));
      ast::Type *pattern =
          Ctx.make<ast::GenericType>(loc, Ctx.intern(callee), std::move(named));
      if (!inferTypeArgs(callee, *params, paramTypes, argTypes, loc, typeArgs,
                         "", pattern, expected, forms))
        return false;
    }
    auto *ct = instantiateClass(callee, typeArgs, loc, written);
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
      std::vector<ast::ParamMode> modes;
      if (init)
        for (auto &p : init->getParams()) {
          paramTypes.push_back(here(tmpl, p.ParamType));
          modes.push_back(p.Mode);
        }
      else
        for (auto *f : tmpl->getFields())
          paramTypes.push_back(here(tmpl, f->getType()));
      const auto forms = inferenceForms(node->getArguments(), [&](size_t i) {
        return i < modes.size() ? modes[i] : ast::ParamMode::Value;
      });
      // The slot's type gives what the arguments leave open, and the forms
      // they take (`h: H<shared Node> = H(Node(2))`, #368).
      std::vector<ast::Type *> named;
      for (const std::string *p : *params)
        named.push_back(Ctx.make<ast::ClassType>(loc, *p));
      ast::Type *pattern =
          Ctx.make<ast::GenericType>(loc, Ctx.intern(callee), std::move(named));
      if (!inferTypeArgs(callee, *params, paramTypes, argTypes, loc, typeArgs,
                         "", pattern, expected, forms))
        return false;
    }
    auto *st = instantiateStruct(callee, typeArgs, loc, written);
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
      const auto forms = inferenceForms(node->getArguments(), [&](size_t i) {
        return i < tmpl->getParams().size() ? tmpl->getParams()[i].Mode
                                            : ast::ParamMode::Value;
      });
      if (!inferTypeArgs(callee, tmpl->getTypeParams(), paramTypes, argTypes,
                         loc, typeArgs, "",
                         result ? here(tmpl, result) : nullptr, expected,
                         forms))
        return false;
    }
    std::string inst = instantiateFunction(callee, typeArgs, loc, written);
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
    // The owner's arguments are written after the owner (`Box<int>::of(1)`),
    // never inferred, and not after the function, where its own go.
    if (ownerArgs.empty()) {
      error(loc, "'" + displayName(callee) + "' is a function of the generic " +
                     (isClass ? "class" : "struct") + " '" +
                     displayName(owner) + "': write '" + displayName(owner) +
                     "<...>::" + member + "(...)' with its type arguments");
      return false;
    }
    std::string instName;
    ast::ClassType *ownerClass = nullptr;
    ast::StructType *ownerStruct = nullptr;
    {
      const ast::ASTNode *outer = std::exchange(OwnerListOf, node);
      if (isClass) {
        ownerClass = instantiateClass(owner, ownerArgs, loc, node);
        instName = ownerClass ? ownerClass->getName() : std::string();
      } else {
        ownerStruct = instantiateStruct(owner, ownerArgs, loc, node);
        instName = ownerStruct ? ownerStruct->getName() : std::string();
      }
      OwnerListOf = outer;
      if (instName.empty())
        return false;
    }
    // A private one is its type's own, as a method is.
    if (names::isPrivateName(member) &&
        !checkMemberAccess(ownerClass ? static_cast<ast::Type *>(ownerClass)
                                      : static_cast<ast::Type *>(ownerStruct),
                           member, loc))
      return false;
    // A generic `static fn` (#353): the function's own arguments, written
    // after its name or inferred.
    if (FoundMethodTemplate found; findMethodTemplate(
            ownerClass, ownerStruct, member, found, /*statics=*/true))
      return resolveGenericStatic(node, found, std::move(typeArgs), argTypes,
                                  expected);
    if (!typeArgs.empty()) {
      error(loc, "'" + displayName(instName) + names::kQualSep + member +
                     "' is not generic and takes no type arguments");
      return false;
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

  // `Point::make<int>(1)`: a generic `static fn` of a class or struct that
  // is not generic (#353).
  if (ownerSep != std::string::npos) {
    const std::string owner = callee.substr(0, ownerSep);
    const bool others = isOthersPrivateType(owner);
    ast::ClassType *ct = others ? nullptr : Ctx.lookupClassType(owner);
    ast::StructType *st = ct || others ? nullptr : Ctx.lookupStructType(owner);
    if (FoundMethodTemplate found;
        (ct || st) && findMethodTemplate(ct, st, callee.substr(ownerSep + 2),
                                         found, /*statics=*/true)) {
      if (!ownerArgs.empty()) {
        error(loc, "'" + displayName(owner) + "' is not a generic " +
                       (ct ? "class" : "struct") +
                       " and takes no type arguments");
        return false;
      }
      return resolveGenericStatic(node, found, std::move(typeArgs), argTypes,
                                  expected);
    }
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

bool Sema::checkBufferElements() {
  bool ok = true;
  std::vector<BufferElementCheck> checks =
      std::exchange(BufferElementChecks, {});
  for (BufferElementCheck &c : checks) {
    if (!ast::holdsDeinit(c.Elem))
      continue;
    // Reported where the buffer was asked for, in that file, under the
    // instantiations that asked.
    const InstantiationFrame own = std::move(c.Frames.back());
    c.Frames.pop_back();
    std::vector<InstantiationFrame> saved =
        std::exchange(InstantiationStack, std::move(c.Frames));
    const std::string savedName = Diags.getSourceName();
    const std::vector<std::string> *savedLines = Diags.getSourceLines();
    if (!own.File.empty())
      Diags.setSourceInfo(own.File, own.Lines);
    error(own.RequestLoc,
          "a buffer cannot hold a '" + typeName(c.Elem) +
              "' yet: its elements run no '__deinit__' when the buffer lets "
              "them go; hold each in a 'mem::Box<" +
              typeName(c.Elem) + ">'");
    if (!own.File.empty())
      Diags.setSourceInfo(savedName, savedLines);
    InstantiationStack = std::move(saved);
    ok = false;
  }
  return ok;
}

bool Sema::checkPendingInstantiations() {
  bool ok = true;
  // Made only for those whose bounds failed: a check made at the request
  // would not have made it either (KF-6).  Held until every body has been
  // checked, as one may still ask for it (`Inner<Foo>` in the body of a
  // `use<int>` queued after it), and then checked with them.
  std::vector<PendingInstantiation> held;
  for (;;) {
    if (!checkPendingInstantiationBodies(held))
      ok = false;
    auto freed = std::stable_partition(held.begin(), held.end(),
                                       [&](const PendingInstantiation &p) {
                                         return madeOnlyForFailedBounds(p.Name);
                                       });
    if (freed == held.end())
      break;
    PendingInstantiations.assign(std::make_move_iterator(freed),
                                 std::make_move_iterator(held.end()));
    held.erase(freed, held.end());
  }
  SuppressedFollowOns += held.size();
  return ok;
}

bool Sema::checkPendingInstantiationBodies(
    std::vector<PendingInstantiation> &held) {
  bool ok = true;
  // A body may request further instantiations, which append to the list, so
  // index (not iterate) and copy the entry (the vector may reallocate).
  for (size_t i = 0; i < PendingInstantiations.size(); ++i) {
    PendingInstantiation p = PendingInstantiations[i];
    // Its bounds failed (reported): its bodies would only repeat that.
    if (FailedBoundInstances.count(p.Name))
      continue;
    if (madeOnlyForFailedBounds(p.Name)) {
      held.push_back(std::move(p));
      continue;
    }
    InstantiationStack.push_back(
        {p.Name, p.RequestLoc, p.RequestFile, p.RequestLines});
    if (const ast::Decl *d = p.Class    ? static_cast<ast::Decl *>(p.Class)
                             : p.Struct ? static_cast<ast::Decl *>(p.Struct)
                                        : static_cast<ast::Decl *>(p.Func))
      InstanceFrames[d] = InstantiationStack;
    const StringSet *savedParams = CurrentTypeParams;
    CurrentTypeParams = &p.TypeParams;
    const size_t savedDepth = CurrentInstantiationDepth;
    CurrentInstantiationDepth = p.Depth;
    std::vector<InstantiatingTemplate> savedTemplates =
        std::exchange(InstantiatingTemplates, p.Templates);
    bool r;
    if (p.Func && p.Func->hasInferredResult()) {
      // Checked where it was first used (inferResultOf), or now.
      InstantiationStack.pop_back();
      CurrentInstantiationDepth = savedDepth;
      CurrentTypeParams = savedParams;
      InstantiatingTemplates = std::move(savedTemplates);
      if (!inferResultOf(p.Func->getName(), p.RequestLoc) ||
          InferredFunctions[p.Func->getName()].State != Inference::Done)
        ok = false;
      continue;
    }
    {
      OriginSource inOrigin(*this, p.Origin);
      InstanceCode instance(*this, &p.Args);
      r = p.Class    ? visitClassDecl(p.Class)
          : p.Struct ? visitStructDecl(p.Struct)
                     : visitFuncDecl(p.Func);
    }
    CurrentInstantiationDepth = savedDepth;
    CurrentTypeParams = savedParams;
    InstantiatingTemplates = std::move(savedTemplates);
    InstantiationStack.pop_back();
    if (!r)
      ok = false;
  }
  PendingInstantiations.clear();
  return ok;
}

void Sema::injectInstantiations(ast::TranslationUnit *tu) {
  // A generic method's instantiations are methods of their owners, lowered
  // with them (#353): added here, once no walk of an owner's methods is in
  // progress.
  for (const MadeMethod &m : InstantiatedMethods) {
    auto named = [&](const auto &decls) {
      for (auto *d : decls)
        if (d->getName() == m.Owner) {
          d->addMethod(m.Fn);
          return true;
        }
      return false;
    };
    if (named(tu->getClassDecls()) || named(InstantiatedClassDecls) ||
        named(ForeignInstantiatedClassDecls) || named(tu->getStructDecls()) ||
        named(InstantiatedStructDecls) || named(ForeignInstantiatedStructDecls))
      continue;
    // Its owner is another module's to emit: the method is this module's,
    // in a declaration of its own by the owner's name.
    if (named(ForeignMethodClassDecls) || named(ForeignMethodStructDecls))
      continue;
    if (ast::isa<ast::ClassType>(m.Type)) {
      ForeignMethodClassDecls.push_back(Ctx.make<ast::ClassDecl>(
          m.Fn->getLocation(), Ctx.intern(m.Owner), Ctx.intern(""),
          std::vector<ast::VarDecl *>{}, std::vector<ast::FuncDecl *>{m.Fn}));
    } else {
      ForeignMethodStructDecls.push_back(
          Ctx.make<ast::StructDecl>(m.Fn->getLocation(), Ctx.intern(m.Owner),
                                    std::vector<ast::VarDecl *>{}));
      ForeignMethodStructDecls.back()->addMethod(m.Fn);
    }
  }
  InstantiatedMethods.clear();
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
