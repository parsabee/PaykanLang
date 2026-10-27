// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// What a module's `.pkm` keeps of its code (see ModuleCode.h).

#include "paykan/modules/ModuleCode.h"

#include "Names.h"
#include "paykan/pir/DeadCode.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace paykan::modules {

namespace {

bool nameChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' ||
         c == '$';
}

/// Each instantiation `Name<...>` @p type spells, one in another's type
/// arguments too.
void instantiationsIn(std::string_view type, std::vector<std::string> &out) {
  for (size_t i = 0; i < type.size(); ++i) {
    if (type[i] != '<')
      continue;
    size_t start = i;
    while (start > 0 && nameChar(type[start - 1]))
      --start;
    if (start == i)
      continue;
    int depth = 0;
    size_t end = i;
    for (; end < type.size(); ++end) {
      const char c = type[end];
      if (c == '-' && end + 1 < type.size() && type[end + 1] == '>')
        ++end; // a function type's `->`
      else if (c == '<')
        ++depth;
      else if (c == '>' && --depth == 0)
        break;
    }
    if (end < type.size())
      out.emplace_back(type.substr(start, end + 1 - start));
  }
}

/// An item the lowering made for code of the module rather than for one of
/// its declarations: a closure or a function value's code (`fn.`), or an
/// instantiation of one of its generic functions (`first<int>`).  It stays
/// only while something reaches it.
bool synthesized(const std::string &name) {
  return name.starts_with(names::kClosureClassPrefix) ||
         name.find('<') != std::string::npos;
}

/// @p f declared: an extern of its module, or of the one it already names.
pir::Function declaration(const pir::Function &f, const std::string &module) {
  pir::Function d;
  d.Name = f.Name;
  d.Sig = f.Sig;
  d.IsExtern = true;
  d.Module = f.IsExtern ? f.Module : module;
  d.Symbol = f.Symbol;
  d.Shared = f.Shared;
  d.Uses = f.Uses;
  return d;
}

void prune(pir::Module &module, const pir::Roots &roots) {
  pir::Program p;
  p.Modules.push_back(std::move(module));
  pir::dropDeadCode(p, roots);
  module = std::move(p.Modules.front());
}

} // namespace

std::set<std::string> exposedTypes(const pkm::Interface &iface) {
  std::set<std::string> exposed;
  std::vector<std::string> work;
  auto spell = [&](std::string_view type) {
    std::vector<std::string> found;
    instantiationsIn(type, found);
    for (std::string &t : found)
      if (exposed.insert(t).second)
        work.push_back(std::move(t));
  };
  auto members = [&](const std::vector<pkm::ClassRec::Field> &fields,
                     const std::vector<pkm::ClassRec::Method> &methods) {
    for (const pkm::ClassRec::Field &f : fields)
      if (!names::isPrivateName(f.FieldName))
        spell(f.TypeName);
    for (const pkm::ClassRec::Method &m : methods) {
      if (names::isPrivateName(m.Name))
        continue;
      spell(m.ReturnTypeName);
      for (const std::string &p : m.ParamTypeNames)
        spell(p);
    }
  };
  for (const pkm::FuncRec &f : iface.Functions) {
    spell(f.ReturnTypeName);
    for (const std::string &p : f.ParamTypeNames)
      spell(p);
  }
  // The module's own types (an instantiation's members count only once
  // the instantiation is exposed).
  for (const pkm::ClassRec &c : iface.Classes)
    if (c.IsLocal && c.Name.find('<') == std::string::npos) {
      spell(c.SuperClassName);
      members(c.Fields, c.Methods);
    }
  for (const pkm::StructRec &s : iface.Structs)
    if (s.IsLocal && s.Name.find('<') == std::string::npos)
      members(s.Fields, s.Methods);
  while (!work.empty()) {
    std::string t = std::move(work.back());
    work.pop_back();
    for (const pkm::ClassRec &c : iface.Classes)
      if (c.Name == t) {
        spell(c.SuperClassName);
        members(c.Fields, c.Methods);
      }
    for (const pkm::StructRec &s : iface.Structs)
      if (s.Name == t)
        members(s.Fields, s.Methods);
  }
  return exposed;
}

void trimModuleCode(pir::Module &module, const pkm::Interface &iface) {
  const std::set<std::string> exposed = exposedTypes(iface);
  // A shared copy stays when its type is exposed; anything else of the
  // module's own when it is not synthesized.
  auto kept = [&](const std::string &name, bool shared) {
    return shared ? exposed.count(std::string(names::ownerTypeName(name))) > 0
                  : !synthesized(name);
  };
  pir::Roots roots;
  roots.Functions = [&](const pir::Module &, const pir::Function &f) {
    return !f.IsExtern && kept(f.Name, f.Shared);
  };
  roots.Classes = [&](const pir::Module &, const pir::Class &c) {
    return !c.IsExtern && kept(c.Name, c.Shared);
  };
  prune(module, roots);
}

ModuleCode splitModuleCode(pir::Module module) {
  ModuleCode out;
  pir::Module &code = out.Code;
  pir::Module &compiled = out.Compiled;
  code.Name = compiled.Name = module.Name;
  code.CStrs = compiled.CStrs = module.CStrs;
  code.Datas = compiled.Datas = module.Datas;
  code.Bytes = compiled.Bytes = module.Bytes;
  code.Externs = compiled.Externs = module.Externs;
  // A definition is in one part and declared in the other: a shared one
  // stays in CODE, any other is compiled.  A compiled class keeps its
  // layout and vtable in CODE (its vtable's functions are the compiled
  // code's, or copies).
  for (const pir::Class &c : module.Classes) {
    pir::Class declared = c;
    if (!c.IsExtern) {
      declared.IsExtern = true;
      declared.Module = module.Name;
    }
    code.Classes.push_back(c.Shared ? c : declared);
    compiled.Classes.push_back(c.Shared ? declared : c);
  }
  // What a compiled body may name in CODE: the module's functions (not the
  // runtime's or C's, which CODE need not keep for it) and its classes.
  std::unordered_set<std::string> named;
  for (const pir::Function &f : module.Functions)
    if (!f.IsExtern || !f.Module.empty())
      named.insert(f.Name);
  for (const pir::Class &c : module.Classes)
    named.insert(c.Name);
  for (pir::Function &f : module.Functions) {
    pir::Function declared = declaration(f, module.Name);
    if (f.IsExtern) {
      code.Functions.push_back(declaration(f, module.Name));
      compiled.Functions.push_back(std::move(declared));
    } else if (f.Shared) {
      compiled.Functions.push_back(std::move(declared));
      code.Functions.push_back(std::move(f));
    } else {
      for (const std::string &n : pir::references(f))
        if (named.count(n))
          declared.Uses.push_back(n);
      code.Functions.push_back(std::move(declared));
      compiled.Functions.push_back(std::move(f));
    }
  }

  // CODE keeps its copies, and the compiled declarations an importer may
  // name (synthesized ones only while a copy or a declaration uses them);
  // each part, the declarations and constants its code uses.  CODE also
  // keeps the module's native C functions, which its compiled code calls:
  // a program checks that its modules bind each C symbol with one
  // signature (lowering::checkNativeSymbols).
  pir::Roots codeRoots;
  codeRoots.Functions = [](const pir::Module &m, const pir::Function &f) {
    return (!f.IsExtern && f.Shared) ||
           (m.isCompiled(f) && !f.Shared && !synthesized(f.Name)) ||
           (f.IsExtern && f.Module.empty() && pir::isNativeName(f.Name));
  };
  codeRoots.Classes = [](const pir::Module &m, const pir::Class &c) {
    return (!c.IsExtern && c.Shared) ||
           (m.isCompiled(c) && !c.Shared && !synthesized(c.Name));
  };
  prune(code, codeRoots);
  pir::Roots compiledRoots;
  compiledRoots.Functions = [](const pir::Module &, const pir::Function &f) {
    return !f.IsExtern;
  };
  compiledRoots.Classes = [](const pir::Module &, const pir::Class &c) {
    return !c.IsExtern;
  };
  prune(compiled, compiledRoots);
  return out;
}

bool providesFor(const pir::Module &code, const pir::Program &program) {
  for (const pir::Module &m : program.Modules) {
    if (m.Name == code.Name)
      continue;
    for (const pir::Function &f : m.Functions)
      if (f.IsExtern && f.Module == code.Name) {
        const pir::Function *def = code.findFunction(f.linkName());
        if (!def || !code.defines(*def))
          return false;
      }
    for (const pir::Class &c : m.Classes)
      if (c.IsExtern && c.Module == code.Name) {
        const pir::Class *def = code.findClass(c.Name);
        if (!def || !code.defines(*def))
          return false;
      }
  }
  return true;
}

namespace {

/// The names of the slots of the vtable of the class @p c describes, in
/// order: IFACE lists them, then the initializer and the `impl` methods,
/// which have none.
std::vector<std::string> slotNames(const pkm::ClassRec &c) {
  std::vector<std::string> slots;
  for (const pkm::ClassRec::Method &m : c.Methods)
    if (!(m.Flags & pkm::kMethodImpl) && m.Name != names::kMethodInit &&
        m.Name != names::kMethodInitModulePrivate)
      slots.push_back(m.Name);
  return slots;
}

/// The PIR type a value of the IFACE type @p type is held in, where its
/// name alone decides it (sema::pirType): every integer type an `i64`,
/// every floating-point type an `f64`, `bool`, `char` and `void` their own,
/// and `Str`, `Obj` and each class the interface describes (@p classes) a
/// `box`.  Nothing for any other type: a struct's, a tuple's or an array's
/// layout, an enum's payloads and an optional's element decide theirs.
std::optional<pir::Type>
pirTypeOfName(const std::string &type,
              const std::vector<std::string_view> &classes) {
  using pir::Type;
  static const std::pair<std::string_view, Type> kBuiltins[] = {
      {names::kTypeInt, Type::I64},   {names::kTypeI8, Type::I64},
      {names::kTypeI16, Type::I64},   {names::kTypeI32, Type::I64},
      {names::kTypeI64, Type::I64},   {names::kTypeU8, Type::I64},
      {names::kTypeU16, Type::I64},   {names::kTypeU32, Type::I64},
      {names::kTypeU64, Type::I64},   {names::kTypeUnsigned, Type::I64},
      {names::kTypeFloat, Type::F64}, {names::kTypeF16, Type::F64},
      {names::kTypeF32, Type::F64},   {names::kTypeF64, Type::F64},
      {names::kTypeBool, Type::Bool}, {names::kTypeChar, Type::Char},
      {names::kTypeVoid, Type::Void}};
  for (const auto &[name, ty] : kBuiltins)
    if (type == name)
      return ty;
  if (type == names::kString || type == names::kObj ||
      std::binary_search(classes.begin(), classes.end(),
                         std::string_view(type)))
    return Type::Box;
  return std::nullopt;
}

/// The signature the lowering gives a function of the parameters @p params,
/// passed as @p modes says, and of the result @p ret, or a method of them
/// whose receiver is passed as @p self (functionSignature, methodSignature):
/// a slice parameter is its address and its length, an `inout` one its
/// address, an `inout` result its storage's address, and every other
/// parameter and the result what pirTypeOfName holds it in.  Nothing when
/// one of those has no pirTypeOfName, or is a `void` parameter.
std::optional<pir::Signature>
derivedSignature(const std::vector<std::string> &params, const std::string &ret,
                 const pkm::ModeRecs &modes, std::optional<pir::Type> self,
                 const std::vector<std::string_view> &classes) {
  pir::Signature sig;
  if (self)
    sig.Params.push_back(*self);
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].ends_with("[]")) {
      sig.Params.push_back(pir::Type::Ptr);
      sig.Params.push_back(pir::Type::I64);
      continue;
    }
    if (i < modes.Modes.size() && modes.Modes[i] == pkm::kModeInout) {
      sig.Params.push_back(pir::Type::Ptr);
      continue;
    }
    std::optional<pir::Type> t = pirTypeOfName(params[i], classes);
    if (!t || *t == pir::Type::Void)
      return std::nullopt;
    sig.Params.push_back(*t);
  }
  if (modes.Result == pkm::kModeInout) {
    sig.Ret = pir::Type::Ptr;
    return sig;
  }
  std::optional<pir::Type> r = pirTypeOfName(ret, classes);
  if (!r)
    return std::nullopt;
  sig.Ret = *r;
  return sig;
}

/// "(i64, box) -> void".
std::string signatureText(const pir::Signature &sig) {
  std::string s = "(";
  for (size_t i = 0; i < sig.Params.size(); ++i)
    s += (i ? ", " : "") + std::string(pir::typeName(sig.Params[i]));
  return s + ") -> " + pir::typeName(sig.Ret);
}

} // namespace

Status checkInterface(const pkm::Interface &iface) {
  auto initializes = [](const std::vector<pkm::ClassRec::Method> &methods) {
    return std::any_of(methods.begin(), methods.end(),
                       [](const pkm::ClassRec::Method &m) {
                         return m.Name == names::kMethodInit ||
                                m.Name == names::kMethodInitModulePrivate;
                       });
  };
  std::map<std::string, const pkm::ClassRec *> classes;
  for (const pkm::ClassRec &c : iface.Classes)
    classes.emplace(c.Name, &c);
  // The fields of an enum's variant are the struct `Enum::variant`, which
  // has no initializer of its own; every other struct and every class has
  // one, the default one if none is written.
  std::set<std::string> others, variants;
  for (const pkm::EnumRec &e : iface.Enums)
    for (const std::string &v : e.PayloadVariants)
      variants.insert(e.Name + names::kQualSep + v);
  for (const pkm::StructRec &s : iface.Structs) {
    others.insert(s.Name);
    if (!variants.count(s.Name) && !initializes(s.Methods))
      return Status::error("struct '" + s.Name + "' has no initializer");
  }
  for (const pkm::EnumRec &e : iface.Enums)
    if (!others.insert(e.Name).second)
      return Status::error("'" + e.Name + "' is a struct and an enum");
  for (const pkm::ClassRec &c : iface.Classes) {
    if (others.count(c.Name))
      return Status::error("'" + c.Name + "' is a class and a struct or enum");
    if (!initializes(c.Methods))
      return Status::error("class '" + c.Name + "' has no initializer");
    if (c.SuperClassName.empty())
      continue;
    auto super = classes.find(c.SuperClassName);
    if (super == classes.end())
      return Status::error("class '" + c.Name + "' extends '" +
                           c.SuperClassName + "', which it does not describe");
    // A subclass's vtable starts with its superclass's, slot for slot: an
    // importer builds the one from the other.
    // Its ancestors end at the root.
    size_t depth = 0;
    for (auto a = super; a != classes.end() && depth <= classes.size();
         a = classes.find(a->second->SuperClassName))
      ++depth;
    if (depth > classes.size())
      return Status::error("class '" + c.Name + "' is its own ancestor");
    const std::vector<std::string> inherited = slotNames(*super->second);
    const std::vector<std::string> slots = slotNames(c);
    if (slots.size() < inherited.size() ||
        !std::equal(inherited.begin(), inherited.end(), slots.begin()))
      return Status::error("the vtable of class '" + c.Name +
                           "' does not start with the slots of its "
                           "superclass '" +
                           c.SuperClassName + "'");
  }
  return Status::ok();
}

Status checkCodeMatchesInterface(const pir::Module &code,
                                 const pkm::Interface &iface) {
  auto definedFunction = [&](const std::string &name) -> const pir::Function * {
    const pir::Function *f = code.findFunction(name);
    return f && code.defines(*f) ? f : nullptr;
  };
  std::vector<std::string_view> classNames;
  classNames.reserve(iface.Classes.size());
  for (const pkm::ClassRec &c : iface.Classes)
    classNames.push_back(c.Name);
  std::sort(classNames.begin(), classNames.end());
  // A function or method of the module whose signature the interface
  // decides has it in CODE.
  auto checkSignature = [&](const std::string &what, const pir::Function &f,
                            const std::optional<pir::Signature> &derived) {
    if (!derived || f.Sig == *derived)
      return Status::ok();
    return Status::error(what + " is " + signatureText(f.Sig) + " in CODE, " +
                         signatureText(*derived) + " by its types in IFACE");
  };
  auto methodSignature = [&](const pkm::ClassRec::Method &m, pir::Type self) {
    std::optional<pir::Signature> sig = derivedSignature(
        m.ParamTypeNames, m.ReturnTypeName, m.Modes, self, classNames);
    // The virtual `equals` returns the runtime's int64_t.
    if (sig && self == pir::Type::Obj && m.Name == names::kMethodEquals &&
        sig->Ret == pir::Type::Bool)
      sig->Ret = pir::Type::I64;
    return sig;
  };
  // `___init__`, the module's own initializer, is `C.__init__`.
  auto functionOf = [](const std::string &type, const std::string &method) {
    return type + names::kNameSep +
           (method == names::kMethodInitModulePrivate
                ? std::string(names::kMethodInit)
                : method);
  };
  for (const pkm::ClassRec &c : iface.Classes) {
    if (!c.IsLocal)
      continue; // its own module's CODE defines it
    const pir::Class *cls = code.findClass(c.Name);
    if (!cls || !code.defines(*cls))
      return Status::error("class '" + c.Name + "' is not in CODE");
    const std::string what = "class '" + c.Name + "': ";
    if (cls->Super != c.SuperClassName)
      return Status::error(what + "its superclass is '" + cls->Super +
                           "' in CODE, '" + c.SuperClassName + "' in IFACE");
    // CODE lists every field, the superclass's first; IFACE the class's own.
    const pir::Class *super =
        cls->Super.empty() ? nullptr : code.findClass(cls->Super);
    const size_t inherited = super ? super->Fields.size() : 0;
    std::vector<std::string> own, declared;
    for (size_t i = inherited; i < cls->Fields.size(); ++i)
      own.push_back(cls->Fields[i].Name);
    for (const pkm::ClassRec::Field &f : c.Fields)
      declared.push_back(f.FieldName);
    std::sort(own.begin(), own.end());
    std::sort(declared.begin(), declared.end());
    if (own != declared)
      return Status::error(what + "its fields are not the ones in CODE");
    for (const pkm::ClassRec::Field &f : c.Fields) {
      auto field = std::find_if(
          cls->Fields.begin() + static_cast<std::ptrdiff_t>(inherited),
          cls->Fields.end(),
          [&](const pir::Field &pf) { return pf.Name == f.FieldName; });
      // A `weak` field holds a weak reference, whatever its type.
      std::optional<pir::Type> ty =
          f.Weak ? pir::Type::Weak : pirTypeOfName(f.TypeName, classNames);
      if (ty && field->Ty != *ty)
        return Status::error(what + "field '" + f.FieldName + "' is " +
                             pir::typeName(field->Ty) + " in CODE, " +
                             pir::typeName(*ty) + " by its type in IFACE");
    }
    const std::vector<std::string> slots = slotNames(c);
    if (slots.size() != cls->VTable.size())
      return Status::error(
          what + "its vtable has " + std::to_string(cls->VTable.size()) +
          " slots in CODE, " + std::to_string(slots.size()) + " in IFACE");
    for (size_t i = 0; i < slots.size(); ++i) {
      const pir::VTableEntry &e = cls->VTable[i];
      if (slots[i] != e.Slot)
        return Status::error(what + "slot " + std::to_string(i) +
                             " of its vtable is '" + e.Slot + "' in CODE, '" +
                             slots[i] + "' in IFACE");
      // The lowering fills every slot, a pure virtual method's with its
      // trap.
      if (e.Target.empty())
        return Status::error(what + "slot " + std::to_string(i) + " ('" +
                             e.Slot + "') of its vtable is empty in CODE");
    }
    for (const pkm::ClassRec::Method &m : c.Methods) {
      std::optional<pir::Signature> sig = methodSignature(m, pir::Type::Obj);
      if (const pir::Function *f = definedFunction(functionOf(c.Name, m.Name)))
        if (Status s = checkSignature("method '" + c.Name + "." + m.Name + "'",
                                      *f, sig);
            !s)
          return s;
      for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i] == m.Name && sig && !(cls->VTable[i].Sig == *sig))
          return Status::error(
              what + "slot " + std::to_string(i) + " ('" + m.Name +
              "') of its vtable is " + signatureText(cls->VTable[i].Sig) +
              " in CODE, " + signatureText(*sig) + " by its types in IFACE");
    }
    // A class's constructor takes what its initializer takes after the
    // object, and returns the object's box.
    const pir::Function *ctor = definedFunction(c.Name);
    const pir::Function *init =
        definedFunction(c.Name + names::kNameSep + names::kMethodInit);
    if (ctor && init &&
        (ctor->Sig.Ret != pir::Type::Box || init->Sig.Params.empty() ||
         !std::equal(ctor->Sig.Params.begin(), ctor->Sig.Params.end(),
                     init->Sig.Params.begin() + 1, init->Sig.Params.end())))
      return Status::error(
          what + "its constructor is " + signatureText(ctor->Sig) +
          " in CODE, its initializer " + signatureText(init->Sig));
  }
  for (const pkm::StructRec &st : iface.Structs) {
    if (!st.IsLocal)
      continue;
    // A struct's methods are its module's functions `S.m`, which take the
    // struct's address first.
    for (const pkm::ClassRec::Method &m : st.Methods) {
      const pir::Function *f = definedFunction(functionOf(st.Name, m.Name));
      if (!f)
        return Status::error("method '" + st.Name + "." + m.Name +
                             "' is not in CODE");
      if (Status s = checkSignature("method '" + st.Name + "." + m.Name + "'",
                                    *f, methodSignature(m, pir::Type::Ptr));
          !s)
        return s;
    }
  }
  // A class with a pure virtual method has no constructor function to
  // define: nothing makes one of its own.
  std::set<std::string> abstract;
  for (const pkm::ClassRec &c : iface.Classes)
    if (c.IsLocal && std::any_of(c.Methods.begin(), c.Methods.end(),
                                 [](const pkm::ClassRec::Method &m) {
                                   return m.Flags & pkm::kMethodPure;
                                 }))
      abstract.insert(c.Name);
  for (const pkm::FuncRec &fr : iface.Functions) {
    if (abstract.count(fr.Name))
      continue;
    const pir::Function *f = definedFunction(fr.Name);
    if (!f)
      return Status::error("function '" + fr.Name + "' is not in CODE");
    // A native function's is C's, whose types it adapts.
    if (!fr.Native)
      if (Status s = checkSignature(
              "function '" + fr.Name + "'", *f,
              derivedSignature(fr.ParamTypeNames, fr.ReturnTypeName, fr.Modes,
                               std::nullopt, classNames));
          !s)
        return s;
  }
  return Status::ok();
}

} // namespace paykan::modules
