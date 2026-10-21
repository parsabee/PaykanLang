// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// What a module's `.pkm` keeps of its code (see ModuleCode.h).

#include "paykan/modules/ModuleCode.h"

#include "Names.h"
#include "paykan/pir/DeadCode.h"

#include <cctype>
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

void prune(pir::Module &module, pir::Roots roots) {
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
  // each part, the declarations and constants its code uses.
  pir::Roots codeRoots;
  codeRoots.Functions = [](const pir::Module &m, const pir::Function &f) {
    return (!f.IsExtern && f.Shared) ||
           (m.isCompiled(f) && !f.Shared && !synthesized(f.Name));
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

} // namespace paykan::modules
