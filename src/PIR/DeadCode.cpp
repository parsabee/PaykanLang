// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The dead-code pass (see DeadCode.h).

#include "paykan/pir/DeadCode.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace paykan::pir {

namespace {

/// Collects the names a body refers to, each once.
class ReferenceCollector {
public:
  std::vector<std::string> Names;

  void block(const Block &b) {
    for (const Stmt &s : b.Stmts)
      std::visit([&](const auto &x) { stmt(x); }, s);
  }

private:
  std::unordered_set<std::string> Seen;

  void name(const std::string &n) {
    if (!n.empty() && Seen.insert(n).second)
      Names.push_back(n);
  }
  void operand(const Operand &o) {
    if (const auto *s = std::get_if<SymbolRef>(&o.V))
      name(s->Name);
  }

  void stmt(const Instr &in) {
    for (const Operand &o : in.Args)
      operand(o);
    name(in.Callee);
    name(in.ClassName);
  }
  void stmt(const If &s) {
    operand(s.Cond);
    block(*s.Then);
    if (s.Else)
      block(*s.Else);
  }
  void stmt(const While &s) {
    block(*s.CondBlock);
    operand(s.Cond);
    block(*s.Body);
  }
  void stmt(const Switch &s) {
    operand(s.Value);
    for (const Switch::Case &c : s.Cases)
      block(*c.Body);
    if (s.Default)
      block(*s.Default);
  }
  void stmt(const Return &s) {
    if (s.Value)
      operand(*s.Value);
  }
  void stmt(const Break &) {}
  void stmt(const Continue &) {}
  void stmt(const Unreachable &) {}
};

/// One module's items by name, and which of them are reached.
struct ModuleItems {
  Module *M = nullptr;
  std::unordered_map<std::string, const Function *> Functions;
  std::unordered_map<std::string, const Class *> Classes;
  std::unordered_set<std::string> Globals;
  std::unordered_set<std::string> LiveSymbols; // functions and globals
  std::unordered_set<std::string> LiveClasses;
};

class Reachability {
public:
  explicit Reachability(Program &program) {
    Items.resize(program.Modules.size());
    for (size_t i = 0; i < program.Modules.size(); ++i) {
      Module &m = program.Modules[i];
      ModuleItems &items = Items[i];
      items.M = &m;
      ByName[m.Name] = i;
      for (const Function &f : m.Functions)
        items.Functions.emplace(f.Name, &f);
      for (const Class &c : m.Classes)
        items.Classes.emplace(c.Name, &c);
      for (const CStrGlobal &g : m.CStrs)
        items.Globals.insert(g.Name);
      for (const DataGlobal &g : m.Datas)
        items.Globals.insert(g.Name);
      for (const BytesGlobal &g : m.Bytes)
        items.Globals.insert(g.Name);
      for (const ExternGlobal &g : m.Externs)
        items.Globals.insert(g.Name);
    }
  }

  void run(const Roots &roots) {
    for (size_t i = 0; i < Items.size(); ++i) {
      const Module &m = *Items[i].M;
      if (roots.Functions)
        for (const Function &f : m.Functions)
          if (roots.Functions(m, f))
            symbol(i, f.Name);
      if (roots.Classes)
        for (const Class &c : m.Classes)
          if (roots.Classes(m, c))
            klass(i, c.Name);
    }
    while (!Work.empty()) {
      auto [i, f, c] = Work.back();
      Work.pop_back();
      if (f)
        visit(i, *f);
      else
        visit(i, *c);
    }
  }

  void erase() {
    for (ModuleItems &items : Items) {
      Module &m = *items.M;
      auto dead = [&](const std::string &name) {
        return !items.LiveSymbols.count(name);
      };
      std::erase_if(m.Functions,
                    [&](const Function &f) { return dead(f.Name); });
      std::erase_if(m.Classes, [&](const Class &c) {
        return !items.LiveClasses.count(c.Name);
      });
      std::erase_if(m.CStrs, [&](const CStrGlobal &g) { return dead(g.Name); });
      std::erase_if(m.Datas, [&](const DataGlobal &g) { return dead(g.Name); });
      std::erase_if(m.Bytes,
                    [&](const BytesGlobal &g) { return dead(g.Name); });
      std::erase_if(m.Externs,
                    [&](const ExternGlobal &g) { return dead(g.Name); });
    }
  }

private:
  struct Item {
    size_t Module;
    const Function *F;
    const Class *C;
  };
  std::vector<ModuleItems> Items;
  std::unordered_map<std::string, size_t> ByName;
  std::vector<Item> Work;

  /// @p name in the @-namespace of module @p i: a function or a global.
  void symbol(size_t i, const std::string &name) {
    ModuleItems &items = Items[i];
    if (name.empty() || !items.LiveSymbols.insert(name).second)
      return;
    if (auto it = items.Functions.find(name); it != items.Functions.end())
      Work.push_back({i, it->second, nullptr});
  }
  void klass(size_t i, const std::string &name) {
    ModuleItems &items = Items[i];
    auto it = items.Classes.find(name);
    if (it == items.Classes.end() || !items.LiveClasses.insert(name).second)
      return;
    Work.push_back({i, nullptr, it->second});
  }
  /// A name a body or a Uses list refers to.
  void name(size_t i, const std::string &n) {
    symbol(i, n);
    klass(i, n);
  }
  /// The module named @p module, when it is in the program and is not
  /// module @p self.
  const size_t *other(size_t self, const std::string &module) const {
    auto it = ByName.find(module);
    return it == ByName.end() || it->second == self ? nullptr : &it->second;
  }

  void visit(size_t i, const Function &f) {
    const Module &m = *Items[i].M;
    if (!f.IsExtern) {
      for (const std::string &n : references(f))
        name(i, n);
    } else if (m.isCompiled(f)) {
      for (const std::string &n : f.Uses)
        name(i, n);
    } else if (const size_t *d = other(i, f.Module)) {
      symbol(*d, f.linkName());
    }
  }
  void visit(size_t i, const Class &c) {
    const Module &m = *Items[i].M;
    if (!c.Super.empty())
      klass(i, c.Super);
    for (const VTableEntry &e : c.VTable)
      symbol(i, e.Target);
    if (c.IsExtern && !m.isCompiled(c))
      if (const size_t *d = other(i, c.Module))
        klass(*d, c.Name);
  }
};

class DeadCodePass : public Pass {
public:
  explicit DeadCodePass(Roots roots) : R(std::move(roots)) {}
  std::string_view name() const override { return "dead-code"; }
  Status run(Program &program) override {
    dropDeadCode(program, R);
    return Status::ok();
  }

private:
  Roots R;
};

} // namespace

Roots Roots::entryPoint(const Program &program) {
  Roots r;
  std::string main =
      program.Modules.empty() ? std::string() : program.Modules.front().Name;
  r.Functions = [main](const Module &m, const Function &f) {
    return m.Name == main && !f.IsExtern && f.Name == "main";
  };
  return r;
}

std::vector<std::string> references(const Function &f) {
  ReferenceCollector c;
  c.block(f.Body);
  return std::move(c.Names);
}

void dropDeadCode(Program &program, const Roots &roots) {
  Reachability r(program);
  r.run(roots);
  r.erase();
}

std::unique_ptr<Pass> createDeadCodePass(Roots roots) {
  return std::make_unique<DeadCodePass>(std::move(roots));
}

} // namespace paykan::pir
