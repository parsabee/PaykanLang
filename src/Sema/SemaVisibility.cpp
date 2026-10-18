// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// Semantic analysis: private members (docs/design/visibility.md).  A name
// starting with `__` is private: a field or method to its class or struct
// (reachable from that type's methods and `static fn`s only, in whatever
// module they are instantiated), a free function or a type to its module
// (never through an import's qualifier).  A public declaration cannot hand
// an importer a value of a type it could not name.

#include "Names.h"
#include "Sema.h"
#include "SemaInternal.h"

namespace paykan {
namespace sema {

namespace {

/// The name before any type argument list.
std::string baseName(const std::string &name) {
  return name.substr(0, name.find('<'));
}

/// The last `::`-separated segment of @p name (before any type arguments).
std::string lastSegment(const std::string &name) {
  const std::string base = baseName(name);
  const size_t sep = base.rfind(names::kQualSep);
  return sep == std::string::npos ? base : base.substr(sep + 2);
}

/// Whether @p a and @p b are the same type, or instantiations of the same
/// template (a template's methods are every instantiation's).
bool sameFamily(ast::Type *a, ast::Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  const std::string an = ast::typeName(a), bn = ast::typeName(b);
  return an.find('<') != std::string::npos &&
         bn.find('<') != std::string::npos && baseName(an) == baseName(bn);
}

} // namespace

ast::ClassType *Sema::fieldOwner(ast::ClassType *ct, const std::string &name) {
  for (ast::ClassType *c = ct; c; c = c->getSuperClass())
    for (const auto &[fname, fty] : c->getFields())
      if (fname == name)
        return c;
  return nullptr;
}

ast::ClassType *Sema::declaringClass(ast::ClassType *ct,
                                     const std::string &name) {
  // The nearest class whose superclass does not find the method, or finds
  // another declaration of it (an override).
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    ast::MethodDecl *m = c->findMethod(name);
    if (!m)
      return nullptr;
    ast::ClassType *super = c->getSuperClass();
    if (!super || super->findMethod(name) != m)
      return c;
  }
  return nullptr;
}

bool Sema::checkMemberAccess(ast::Type *owner, const std::string &member,
                             ast::SourceLocation loc) {
  if (!owner || !names::isPrivateName(member))
    return true;
  ast::Type *here = nullptr;
  if (CurrentClassCtx)
    here = CurrentClassCtx->ClassType
               ? static_cast<ast::Type *>(CurrentClassCtx->ClassType)
               : CurrentClassCtx->Struct;
  if (sameFamily(owner, here) || sameFamily(owner, CurrentStaticOwner))
    return true;
  error(loc, "'" + member + "' is private to '" + typeName(owner) +
                 "': it is reachable only from its own methods");
  return false;
}

bool Sema::checkStaticMemberUse(const std::string &qualified,
                                ast::SourceLocation loc) {
  const std::string base = baseName(qualified);
  const size_t sep = base.rfind(names::kQualSep);
  if (sep == std::string::npos || !names::isPrivateName(base.substr(sep + 2)))
    return true;
  const std::string prefix = base.substr(0, sep);
  ast::Type *owner = Ctx.lookupClassType(prefix);
  if (!owner)
    owner = Ctx.lookupStructType(prefix);
  if (!owner)
    return true; // `m::__f`, or an unknown name: reported by the caller
  return checkMemberAccess(owner, base.substr(sep + 2), loc);
}

std::string Sema::privateTypeIn(ast::Type *ty) {
  if (!ty)
    return "";
  auto named = [](const std::string &name) {
    const std::string seg = lastSegment(name);
    return names::isPrivateName(seg) ? seg : std::string();
  };
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (std::string p = named(ct->getName()); !p.empty())
      return p;
    if (const InstantiationInfo *info = instantiationInfo(ct))
      for (ast::Type *arg : info->Args)
        if (std::string p = privateTypeIn(arg); !p.empty())
          return p;
    return "";
  }
  if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    if (std::string p = named(st->getName()); !p.empty())
      return p;
    if (const StructInstantiationInfo *info = structInstantiationInfo(st))
      for (ast::Type *arg : info->Args)
        if (std::string p = privateTypeIn(arg); !p.empty())
          return p;
    return "";
  }
  if (auto *et = ast::dyn_cast<ast::EnumType>(ty))
    return named(et->getName());
  if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
    return privateTypeIn(ot->getInnerType());
  if (auto *at = ast::dyn_cast<ast::ArrayType>(ty))
    return privateTypeIn(at->getElementType());
  if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(ty))
    return privateTypeIn(ft->getElementType());
  if (auto *sl = ast::dyn_cast<ast::SliceType>(ty))
    return privateTypeIn(sl->getElementType());
  if (auto *tt = ast::dyn_cast<ast::TupleType>(ty)) {
    for (ast::Type *e : tt->getElementTypes())
      if (std::string p = privateTypeIn(e); !p.empty())
        return p;
    return "";
  }
  if (auto *fnt = ast::dyn_cast<ast::FunctionType>(ty)) {
    for (ast::Type *p : fnt->getParamTypes())
      if (std::string n = privateTypeIn(p); !n.empty())
        return n;
    return privateTypeIn(fnt->getReturnType());
  }
  if (auto *gt = ast::dyn_cast<ast::GenericType>(ty)) {
    if (std::string p = named(gt->getName()); !p.empty())
      return p;
    for (ast::Type *arg : gt->getArgs())
      if (std::string p = privateTypeIn(arg); !p.empty())
        return p;
  }
  return "";
}

bool Sema::checkPublicSurface(const std::string &what, const std::string &fix,
                              ast::Type *ty, ast::SourceLocation loc) {
  const std::string priv = privateTypeIn(ty);
  if (priv.empty())
    return true;
  error(loc, what + " names the private type '" + priv + "'; make the " + fix +
                 " private or the type public");
  return false;
}

bool Sema::reportPrivateImport(const std::string &name,
                               ast::SourceLocation loc) {
  const std::string base = baseName(name);
  const size_t sep = base.rfind(names::kQualSep);
  if (sep == std::string::npos || !names::isPrivateName(base.substr(sep + 2)))
    return false;
  auto it = ImportQualifiers.find(base.substr(0, sep));
  if (it == ImportQualifiers.end())
    return false;
  error(loc, "'" + base + "' is private to module '" + it->second.Module + "'");
  return true;
}

bool Sema::isModulePrivateName(const std::string &name) {
  const std::string base = baseName(name);
  const size_t sep = base.find(names::kQualSep);
  return names::isPrivateName(sep == std::string::npos ? base
                                                       : base.substr(0, sep));
}

} // namespace sema
} // namespace paykan
