// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// Where a `view fn` may be declared (docs/language/05-classes.md, "Methods
// that don't change self"): only a method, never `__init__`, and a method and
// its overrides are all `view fn` or none is, so that a call through a base
// class knows what the method does to its object.  A method that could be one
// but is not gets a warning, so that `view` parameters can call it.

#include "Names.h"
#include "Sema.h"

#include <algorithm>
#include <map>
#include <set>

namespace paykan {
namespace sema {

namespace {

/// The class, from @p ct up, that introduces method @p name: where a method
/// and all its overrides start.
ast::ClassType *introducingClass(ast::ClassType *ct, const std::string &name) {
  while (ct->getSuperClass() && ct->getSuperClass()->findMethod(name))
    ct = ct->getSuperClass();
  return ct;
}

} // namespace

bool Sema::checkViewFnDecl(const ast::FuncDecl *method,
                           const ast::MethodDecl *base) {
  if (method->isView() && method->getName() == names::kMethodInit) {
    error(method->getLocation(), "'" + std::string(names::kMethodInit) +
                                     "' cannot be a 'view fn': it sets up "
                                     "'self'");
    return false;
  }
  // A `shared fn`: `self` is shared, which an initializer's is not (it is
  // the new object, the constructor's result).  An override hands `self`
  // on as the method it overrides does.
  if (method->isShared() && method->getName() == names::kMethodInit) {
    error(method->getLocation(),
          "'" + std::string(names::kMethodInit) +
              "' cannot be a 'shared fn': the new object is the "
              "constructor's result, held as its caller declares");
    return false;
  }
  if (base && base->isShared() != method->isShared()) {
    error(method->getLocation(),
          "override of '" + method->getName() + "' " +
              (base->isShared() ? "must be a 'shared fn'"
                                : "cannot be a 'shared fn'") +
              ", like the method it overrides: a call of either takes its "
              "receiver as the overridden method says");
    return false;
  }
  if (!base || base->isView() == method->isView())
    return true;
  if (base->isView())
    error(method->getLocation(),
          "override of '" + method->getName() +
              "' must be a 'view fn', like the method it overrides");
  else
    error(method->getLocation(),
          "override of '" + method->getName() +
              "' cannot be a 'view fn': the method it overrides may change "
              "'self'");
  return false;
}

bool Sema::rejectFreeViewFns(ast::TranslationUnit *tu) {
  bool ok = true;
  for (auto *fns : {&tu->getFuncDecls(), &tu->getGenericFuncDecls()})
    for (auto *fn : *fns)
      if (fn->isView() || fn->isShared()) {
        error(fn->getLocation(), std::string("only a method can be a '") +
                                     (fn->isShared() ? "shared" : "view") +
                                     " fn': '" + fn->getName() +
                                     "' is a free function, with no 'self'");
        ok = false;
      }
  return ok;
}

void Sema::warnMissingViewFns() {
  // A method and its overrides form a family, keyed by the class that
  // introduces it: all of them are `view fn` or none is.  A family could be
  // one when no member changes `self` and every `self.m()` in its members
  // calls a family that could be one too (a fixed point, so that a getter
  // calling another getter is found in one compile).  A struct's method is
  // a family of its own (a struct inherits nothing).
  // A generic method's instantiations (`map<Str>`, `map<int>`, #353) are
  // one method, by its template's name: called directly and never
  // overridden, it is a family of its own in the class that declares it.
  using Family = std::pair<ast::Type *, std::string>;
  auto familyOf = [](const MethodUse &use, const std::string &name) -> Family {
    const size_t args = name.find('<');
    if (use.Struct)
      return {use.Struct, name.substr(0, args)};
    if (args != std::string::npos) {
      ast::ClassType *owner = use.Class->implOwner(name);
      return {owner ? owner : use.Class, name.substr(0, args)};
    }
    return {introducingClass(use.Class, name), name};
  };
  auto ownerOf = [](const MethodUse &use) -> ast::Type * {
    return use.Struct ? static_cast<ast::Type *>(use.Struct) : use.Class;
  };
  std::map<Family, std::vector<const MethodUse *>> families;
  for (const MethodUse &use : MethodUses)
    families[familyOf(use, use.Decl->getName())].push_back(&use);
  std::set<Family> could;
  for (const auto &[family, members] : families) {
    // A family started outside this module (Obj's toString, an imported
    // class's method, another module's template) keeps its marker: there is
    // nothing to suggest.
    const auto *intro = methodOf(family.first, family.second);
    FoundMethodTemplate generic;
    const bool isGeneric =
        !intro &&
        findMethodTemplate(ast::dyn_cast<ast::ClassType>(family.first),
                           ast::dyn_cast<ast::StructType>(family.first),
                           family.second, generic);
    if (ImportedTypeOrigins.count(typeName(family.first)) ||
        (isGeneric && foreignOrigin(generic.T.Fn)))
      continue;
    bool introducedHere = false;
    bool changes = false;
    for (const MethodUse *m : members) {
      introducedHere |= ownerOf(*m) == family.first;
      changes |= m->ChangesSelf;
      // A `shared fn` hands on a `shared mut` self, which a `view fn`'s is
      // not: it is no family to suggest the marker for.
      changes |= m->Decl->isShared();
    }
    if (introducedHere && !changes &&
        (intro ? !intro->isView() : isGeneric && !generic.T.Fn->isView()))
      could.insert(family);
  }
  // A generic type's instances share their template's declarations: its
  // method is one for all of them, warned about only when it could be a
  // `view fn` in every instance, and then once.  So it is not when it
  // returns or stores part of `self` holding a type argument: `Box<int>`'s
  // `get` could, but `Box<Str>`'s returns a reference its caller may change
  // (or keeps one in a local, an array or a tuple), and an importer of the
  // module may make a `Box<Str>` the module never sees.
  auto templateOf = [&](ast::Type *t) -> const void * {
    if (auto *ct = ast::dyn_cast<ast::ClassType>(t))
      if (const InstantiationInfo *info = instantiationInfo(ct))
        return info->Template;
    if (auto *st = ast::dyn_cast<ast::StructType>(t))
      if (const StructInstantiationInfo *info = structInstantiationInfo(st))
        return info->Template;
    return t; // not an instance: its own
  };
  using Declared = std::pair<const void *, std::string>;
  std::set<Declared> cannot;
  for (const auto &[family, members] : families)
    if (!could.count(family) ||
        std::any_of(members.begin(), members.end(),
                    [](const MethodUse *m) { return m->CopiesTypeArgument; }))
      cannot.insert({templateOf(family.first), family.second});
  // A family stays one only while its declaration does, and each `self.m()`
  // its members make calls one that stays.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = could.begin(); it != could.end();) {
      const Declared declared{templateOf(it->first), it->second};
      bool stays = !cannot.count(declared);
      for (const MethodUse *m : families[*it])
        for (const std::string &callee : m->SelfCalls)
          if (!could.count(familyOf(*m, callee)))
            stays = false;
      if (stays) {
        ++it;
      } else {
        cannot.insert(declared);
        it = could.erase(it);
        changed = true;
      }
    }
  }
  std::set<std::pair<size_t, size_t>> warned;
  for (const Family &family : could) {
    for (const MethodUse *m : families[family]) {
      if (ownerOf(*m) != family.first)
        continue;
      // `__deinit__` is declared as it is, whatever it does.
      if (family.second == names::kMethodDeinit)
        continue;
      ast::SourceLocation loc = m->Decl->getLocation();
      if (!warned.insert({loc.getLineStart(), loc.getColumnStart()}).second)
        continue;
      // (A generic method's members are its instantiations.)
      bool overridden = families[family].size() > 1 &&
                        m->Decl->getName().find('<') == std::string::npos;
      warning(loc, "'" + family.second +
                       "' never changes 'self': make it a 'view fn'" +
                       (overridden ? " (and its overrides)" : "") +
                       " so 'view' parameters can call it");
    }
  }
}

} // namespace sema
} // namespace paykan
