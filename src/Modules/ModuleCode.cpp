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
#include <unordered_map>
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

/// The types an interface describes, by how a value of one is held: a
/// class's in a `box`, a struct's or a tagged union's in memory, an enum's
/// without payloads in an `i64`.  Each list sorted.
struct DescribedTypes {
  std::vector<std::string_view> Classes, InMemory, Enums;

  explicit DescribedTypes(const pkm::Interface &iface) {
    Classes.reserve(iface.Classes.size());
    for (const pkm::ClassRec &c : iface.Classes)
      Classes.push_back(c.Name);
    InMemory.reserve(iface.Structs.size() + iface.Enums.size());
    for (const pkm::StructRec &st : iface.Structs)
      InMemory.push_back(st.Name);
    for (const pkm::EnumRec &e : iface.Enums)
      (e.PayloadVariants.empty() ? Enums : InMemory).push_back(e.Name);
    for (std::vector<std::string_view> *list : {&Classes, &InMemory, &Enums})
      std::sort(list->begin(), list->end());
  }
};

bool among(const std::vector<std::string_view> &sorted, std::string_view name) {
  return std::binary_search(sorted.begin(), sorted.end(), name);
}

/// How a value of an IFACE type is held: in a PIR value of a type, in
/// memory (ast::isMemType), or as a slice, an address and a length.
struct Held {
  enum Kind { Value, Memory, Slice } How = Value;
  pir::Type Ty = pir::Type::Void; ///< a Value's
};

/// How a value of the IFACE type @p type is held (sema::pirType,
/// ast::isMemType), from its name: an integer type in an `i64`, a
/// floating-point one in an `f64`, `bool`, `char` and `void` in their own,
/// `Str`, `Obj` and a class @p types describes in a `box`, an enum without
/// payloads in an `i64`; a struct, a tagged union, a tuple, a fixed-size
/// array and a function value in memory; `T[]` as a slice; and the optional
/// of a type held in a `box` or in memory as that type is, of one held in
/// a scalar in its optional (`opt_i64` ...).  Nothing for a name that is
/// none of these (one the interface does not describe).
std::optional<Held> heldAs(std::string_view type, const DescribedTypes &types) {
  using pir::Type;
  // A tuple's or a function type's name starts with its parenthesis: what
  // follows the matching one says which (` -> R` a function type's), and
  // whether the name is a slice or an array of it, or its optional.
  if (type.starts_with('(')) {
    size_t depth = 0, close = std::string_view::npos;
    for (size_t i = 0; i < type.size() && close == std::string_view::npos; ++i)
      if (type[i] == '(')
        ++depth;
      else if (type[i] == ')' && --depth == 0)
        close = i;
    if (close == std::string_view::npos)
      return std::nullopt;
    const std::string_view rest = type.substr(close + 1);
    if (!rest.starts_with(" ->") && rest.ends_with("[]"))
      return Held{Held::Slice};
    return Held{Held::Memory};
  }
  if (type.ends_with("[]"))
    return Held{Held::Slice};
  if (type.ends_with(']')) // `T[N]`
    return Held{Held::Memory};
  if (type.ends_with('?')) {
    std::optional<Held> inner = heldAs(type.substr(0, type.size() - 1), types);
    if (!inner || inner->How == Held::Slice)
      return std::nullopt;
    if (inner->How == Held::Memory || inner->Ty == Type::Box)
      return inner;
    const Type optional = pir::optionalOf(inner->Ty);
    if (optional == Type::Void)
      return std::nullopt;
    return Held{Held::Value, optional};
  }
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
      return Held{Held::Value, ty};
  if (type == names::kString || type == names::kObj ||
      among(types.Classes, type))
    return Held{Held::Value, Type::Box};
  if (among(types.Enums, type))
    return Held{Held::Value, Type::I64};
  if (among(types.InMemory, type))
    return Held{Held::Memory};
  return std::nullopt;
}

/// The PIR type of a field of the IFACE type @p type (a `weak` one, @p weak,
/// holds a weak reference whatever its type), or nothing when heldAs knows
/// no PIR type for it.
std::optional<pir::Type> fieldType(const std::string &type, bool weak,
                                   const DescribedTypes &types) {
  if (weak)
    return pir::Type::Weak;
  std::optional<Held> held = heldAs(type, types);
  if (!held || held->How == Held::Slice)
    return std::nullopt;
  return held->How == Held::Memory ? pir::Type::Mem : held->Ty;
}

/// The signature the lowering gives a function of the parameters @p params,
/// passed as @p modes says, and of the result @p ret, or a method of them
/// whose receiver is passed as @p self (functionSignature, methodSignature):
/// a slice parameter is its address and its length, an `inout` one or one
/// held in memory its address, an `inout` result its storage's address, a
/// result held in memory written where a last parameter points, and every
/// other parameter and the result the PIR type it is held in (heldAs).
/// Nothing when one of those types is one heldAs does not know, a `void`
/// parameter or a slice result.
std::optional<pir::Signature>
derivedSignature(const std::vector<std::string> &params, const std::string &ret,
                 const pkm::ModeRecs &modes, std::optional<pir::Type> self,
                 const DescribedTypes &types) {
  pir::Signature sig;
  if (self)
    sig.Params.push_back(*self);
  for (size_t i = 0; i < params.size(); ++i) {
    std::optional<Held> held = heldAs(params[i], types);
    if (!held)
      return std::nullopt;
    if (held->How == Held::Slice) {
      sig.Params.push_back(pir::Type::Ptr);
      sig.Params.push_back(pir::Type::I64);
      continue;
    }
    if ((i < modes.Modes.size() && modes.Modes[i] == pkm::kModeInout) ||
        held->How == Held::Memory) {
      sig.Params.push_back(pir::Type::Ptr);
      continue;
    }
    if (held->Ty == pir::Type::Void)
      return std::nullopt;
    sig.Params.push_back(held->Ty);
  }
  if (modes.Result == pkm::kModeInout) {
    sig.Ret = pir::Type::Ptr;
    return sig;
  }
  std::optional<Held> r = heldAs(ret, types);
  if (!r || r->How == Held::Slice)
    return std::nullopt;
  if (r->How == Held::Memory) {
    sig.Params.push_back(pir::Type::Ptr);
    sig.Ret = pir::Type::Void;
    return sig;
  }
  sig.Ret = r->Ty;
  return sig;
}

/// "(i64, box) -> void".
std::string signatureText(const pir::Signature &sig) {
  std::string s = "(";
  for (size_t i = 0; i < sig.Params.size(); ++i)
    s += (i ? ", " : "") + std::string(pir::typeName(sig.Params[i]));
  return s + ") -> " + pir::typeName(sig.Ret);
}

/// The functions and classes of a module's CODE by their names (the first
/// of a name, as pir::Module::findFunction finds it).
class CodeIndex {
public:
  explicit CodeIndex(const pir::Module &code) : Code(code) {
    Functions.reserve(code.Functions.size());
    for (const pir::Function &f : code.Functions)
      Functions.emplace(f.Name, &f);
    Classes.reserve(code.Classes.size());
    for (const pir::Class &c : code.Classes)
      Classes.emplace(c.Name, &c);
  }

  const pir::Module &code() const { return Code; }

  const pir::Function *function(const std::string &name) const {
    auto it = Functions.find(name);
    return it == Functions.end() ? nullptr : it->second;
  }

  /// The function @p name, when the module defines it (in its PIR or
  /// compiled).
  const pir::Function *defined(const std::string &name) const {
    const pir::Function *f = function(name);
    return f && Code.defines(*f) ? f : nullptr;
  }

  const pir::Class *cls(const std::string &name) const {
    auto it = Classes.find(name);
    return it == Classes.end() ? nullptr : it->second;
  }

private:
  const pir::Module &Code;
  std::unordered_map<std::string_view, const pir::Function *> Functions;
  std::unordered_map<std::string_view, const pir::Class *> Classes;
};

/// The runtime's implementation of the root class's method @p method
/// (`$rt.PaykanObject_toString` for `toString`), or "" for a method the
/// root does not have.
std::string rootMethod(const std::string &method) {
  if (method == names::kMethodDestroy)
    return pir::runtimeName(names::kPaykanObjectDestroy);
  if (method == names::kMethodToString)
    return pir::runtimeName(names::kPaykanObjectToString);
  if (method == names::kMethodEquals)
    return pir::runtimeName(names::kPaykanObjectEquals);
  return "";
}

/// Whether each slot `m` of the vtable of @p cls, a class @p code defines,
/// names the function an importer's lowering finds for it, which the
/// importer's subclasses inherit (ModuleLowering::findConcreteMethodFuncName):
/// `X.m` for the first class X from @p cls up that has one, in @p code
/// while X is a class it defines; past one of another module, an extern
/// `Y.m` of a function there (which the link checks); and past the root,
/// the runtime's (rootMethod).  The first slot that does not when not.
Status checkSlotTargets(const CodeIndex &index, const pir::Class &cls) {
  const pir::Module &code = index.code();
  for (size_t i = 0; i < cls.VTable.size(); ++i) {
    const pir::VTableEntry &e = cls.VTable[i];
    // "class 'C': slot 3 ('m') of its vtable", for an error.
    auto slot = [&] {
      return "class '" + cls.Name + "': slot " + std::to_string(i) + " ('" +
             e.Slot + "') of its vtable";
    };
    if (e.Target.empty())
      return Status::error(slot() + " is empty in CODE");
    const std::string method = names::kNameSep + e.Slot;
    std::string found;
    const pir::Class *x = &cls;
    for (size_t depth = 0; x && code.defines(*x) && found.empty(); ++depth) {
      if (depth > code.Classes.size())
        return Status::error("class '" + cls.Name +
                             "' is its own ancestor in CODE");
      if (const pir::Function *f = index.defined(x->Name + method))
        found = f->Name;
      else
        x = x->Super.empty() ? nullptr : index.cls(x->Super);
    }
    // The slot names another function, which @p why and @p function say.
    auto wrongTarget = [&](const char *why, const std::string &function) {
      std::string message = slot();
      message += " names '@";
      message += e.Target;
      message += "' in CODE, ";
      message += why;
      message += function;
      message += "'";
      return Status::error(message);
    };
    if (found.empty() && x) {
      const pir::Function *t = index.function(e.Target);
      if (t && !code.defines(*t) &&
          (e.Target.ends_with(method) || e.Target == rootMethod(e.Slot)))
        continue;
      return wrongTarget("which is not the class's or an ancestor's method '",
                         e.Slot);
    }
    if (found.empty())
      found = rootMethod(e.Slot);
    if (found.empty())
      return wrongTarget("which is not the class's or an ancestor's method '",
                         e.Slot);
    if (e.Target != found)
      return wrongTarget("where an importer finds '@", found);
  }
  return Status::ok();
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
  const CodeIndex index(code);
  const DescribedTypes types(iface);
  // A function or method of the module (the @p kind @p name) whose
  // signature the interface decides has it in CODE.
  auto checkSignature = [&](const char *kind, const std::string &name,
                            const pir::Function &f,
                            const std::optional<pir::Signature> &derived) {
    if (!derived || f.Sig == *derived)
      return Status::ok();
    return Status::error(std::string(kind) + " '" + name + "' is " +
                         signatureText(f.Sig) + " in CODE, " +
                         signatureText(*derived) + " by its types in IFACE");
  };
  auto methodSignature = [&](const pkm::ClassRec::Method &m, pir::Type self) {
    std::optional<pir::Signature> sig = derivedSignature(
        m.ParamTypeNames, m.ReturnTypeName, m.Modes, self, types);
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
  auto list = [](const std::vector<std::string> &items) {
    std::string s;
    for (const std::string &item : items)
      s += (s.empty() ? "" : ", ") + item;
    return "(" + s + ")";
  };
  for (const pkm::ClassRec &c : iface.Classes) {
    // The module's own classes are in its CODE; a copy of an instantiation
    // it holds (a `shared class`) is checked as one of them, for what it
    // has.
    const pir::Class *cls = index.cls(c.Name);
    if (!cls || !code.defines(*cls)) {
      if (c.IsLocal)
        return Status::error("class '" + c.Name + "' is not in CODE");
      continue;
    }
    if (!c.IsLocal && !cls->Shared)
      return Status::error("class '" + c.Name + "' is of module '" +
                           c.OriginModule +
                           "' in IFACE, of this module in CODE");
    const std::string what = "class '" + c.Name + "': ";
    if (cls->Super != c.SuperClassName)
      return Status::error(what + "its superclass is '" + cls->Super +
                           "' in CODE, '" + c.SuperClassName + "' in IFACE");
    // CODE lists every field in the order of the layout, the superclass's
    // first; IFACE the class's own, in the order an importer lays them out.
    const pir::Class *super =
        cls->Super.empty() ? nullptr : index.cls(cls->Super);
    const size_t inherited = super ? super->Fields.size() : 0;
    std::vector<std::string> own, declared;
    for (size_t i = inherited; i < cls->Fields.size(); ++i)
      own.push_back(cls->Fields[i].Name);
    for (const pkm::ClassRec::Field &f : c.Fields)
      declared.push_back(f.FieldName);
    if (own != declared)
      return Status::error(what + "its own fields are " + list(own) +
                           " in CODE, " + list(declared) + " in IFACE");
    for (size_t i = 0; i < c.Fields.size(); ++i) {
      const pkm::ClassRec::Field &f = c.Fields[i];
      const pir::Field &field = cls->Fields[inherited + i];
      std::optional<pir::Type> ty = fieldType(f.TypeName, f.Weak, types);
      if (ty && field.Ty != *ty)
        return Status::error(what + "field '" + f.FieldName + "' is " +
                             pir::typeName(field.Ty) + " in CODE, " +
                             pir::typeName(*ty) + " by its type in IFACE");
    }
    const std::vector<std::string> slots = slotNames(c);
    if (slots.size() != cls->VTable.size())
      return Status::error(
          what + "its vtable has " + std::to_string(cls->VTable.size()) +
          " slots in CODE, " + std::to_string(slots.size()) + " in IFACE");
    for (size_t i = 0; i < slots.size(); ++i)
      if (slots[i] != cls->VTable[i].Slot)
        return Status::error(what + "slot " + std::to_string(i) +
                             " of its vtable is '" + cls->VTable[i].Slot +
                             "' in CODE, '" + slots[i] + "' in IFACE");
    // A class the module compiles has its initializer and each `impl`
    // method, which an importer calls by name (a subclass's initializer
    // calls its superclass's); a copy has those of them its module uses.
    const bool compiled = !cls->Shared;
    for (const pkm::ClassRec::Method &m : c.Methods) {
      std::optional<pir::Signature> sig = methodSignature(m, pir::Type::Obj);
      const bool slot =
          std::find(slots.begin(), slots.end(), m.Name) != slots.end();
      const std::string function = functionOf(c.Name, m.Name);
      const pir::Function *f = index.defined(function);
      if (!f && compiled && !slot)
        return Status::error("method '" + function + "' is not in CODE");
      if (f)
        if (Status s = checkSignature("method", function, *f, sig); !s)
          return s;
      for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i] == m.Name && sig && !(cls->VTable[i].Sig == *sig))
          return Status::error(
              what + "slot " + std::to_string(i) + " ('" + m.Name +
              "') of its vtable is " + signatureText(cls->VTable[i].Sig) +
              " in CODE, " + signatureText(*sig) + " by its types in IFACE");
    }
    // A class's constructor takes what its initializer takes after the
    // object, and returns the object's box.  A class with a pure virtual
    // method has none: nothing makes one of its own.
    const pir::Function *ctor = index.defined(c.Name);
    const pir::Function *init =
        index.defined(c.Name + names::kNameSep + names::kMethodInit);
    const bool abstract = std::any_of(c.Methods.begin(), c.Methods.end(),
                                      [](const pkm::ClassRec::Method &m) {
                                        return m.Flags & pkm::kMethodPure;
                                      });
    if (!ctor && compiled && !abstract)
      return Status::error(what + "its constructor is not in CODE");
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
      const std::string function = functionOf(st.Name, m.Name);
      const pir::Function *f = index.defined(function);
      if (!f)
        return Status::error("method '" + function + "' is not in CODE");
      if (Status s = checkSignature("method", function, *f,
                                    methodSignature(m, pir::Type::Ptr));
          !s)
        return s;
    }
  }
  // An abstract class has no constructor function to define.
  std::set<std::string> abstract;
  for (const pkm::ClassRec &c : iface.Classes)
    if (c.IsLocal && std::any_of(c.Methods.begin(), c.Methods.end(),
                                 [](const pkm::ClassRec::Method &m) {
                                   return m.Flags & pkm::kMethodPure;
                                 }))
      abstract.insert(c.Name);
  // Each function, a native one's too: CODE has the function that calls C,
  // which takes and returns what any other does.
  for (const pkm::FuncRec &fr : iface.Functions) {
    if (abstract.count(fr.Name))
      continue;
    const pir::Function *f = index.defined(fr.Name);
    if (!f)
      return Status::error("function '" + fr.Name + "' is not in CODE");
    if (Status s = checkSignature("function", fr.Name, *f,
                                  derivedSignature(fr.ParamTypeNames,
                                                   fr.ReturnTypeName, fr.Modes,
                                                   std::nullopt, types));
        !s)
      return s;
  }
  // Every class the module defines, the copies its interface does not
  // describe too: a program may take its vtable.
  for (const pir::Class &cls : code.Classes)
    if (code.defines(cls))
      if (Status s = checkSlotTargets(index, cls); !s)
        return s;
  return Status::ok();
}

} // namespace paykan::modules
