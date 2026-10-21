// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The program's unreachable code, dropped before the shared lowering (see
// DropUnreachable.h).

#include "DropUnreachable.h"

#include "Dialect/PIRDialect.h"
#include "PIRToStandard.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>
#include <mlir/IR/SymbolTable.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Transforms/Passes.h>

#include <vector>

namespace paykan::mlir_backend {

using namespace ::mlir;
namespace P = ::paykan::pir_dialect;

namespace {

/// The classes an op of @p program names, and their superclasses.  Class
/// names are unique across a program, so a name is a class wherever it is
/// defined or declared.
llvm::StringSet<> namedClasses(ModuleOp program) {
  llvm::StringSet<> named;
  llvm::StringMap<StringRef> supers;
  program.walk([&](Operation *op) {
    StringRef name;
    if (auto o = dyn_cast<P::VCallOp>(op))
      name = o.getClassName();
    else if (auto o = dyn_cast<P::NewOp>(op))
      name = o.getClassName();
    else if (auto o = dyn_cast<P::FieldLoadOp>(op))
      name = o.getClassName();
    else if (auto o = dyn_cast<P::FieldStoreOp>(op))
      name = o.getClassName();
    else if (auto o = dyn_cast<P::FieldAddrOp>(op))
      name = o.getClassName();
    else if (auto o = dyn_cast<P::VTableAddrOp>(op))
      name = o.getClassName();
    else if (auto c = dyn_cast<P::ClassOp>(op)) {
      if (std::optional<StringRef> super = c.getSuper())
        supers[c.getName()] = *super;
      return;
    }
    if (!name.empty())
      named.insert(name);
  });
  std::vector<StringRef> work;
  for (const auto &n : named)
    work.push_back(n.getKey());
  while (!work.empty()) {
    StringRef name = work.back();
    work.pop_back();
    if (auto it = supers.find(name);
        it != supers.end() && named.insert(it->second).second)
      work.push_back(it->second);
  }
  return named;
}

size_t countItems(const std::vector<ModuleOp> &modules) {
  size_t n = 0;
  for (ModuleOp m : modules)
    n += m.getBody()->getOperations().size();
  return n;
}

} // namespace

Status dropUnreachable(ModuleOp program) {
  std::vector<ModuleOp> modules(program.getOps<ModuleOp>().begin(),
                                program.getOps<ModuleOp>().end());
  if (modules.empty())
    modules.push_back(program);
  auto symbols = [](ModuleOp m) {
    std::vector<Operation *> ops;
    for (Operation &op : *m.getBody())
      if (isa<SymbolOpInterface>(op))
        ops.push_back(&op);
    return ops;
  };
  // The visibility each symbol had, given back to those that stay.
  const StringRef visibilityAttr = SymbolTable::getVisibilityAttrName();
  llvm::DenseMap<Operation *, StringAttr> visibility;
  for (ModuleOp m : modules)
    for (Operation *op : symbols(m))
      visibility[op] = op->getAttrOfType<StringAttr>(visibilityAttr);

  for (;;) {
    const size_t before = countItems(modules);
    // A class no op names: its vtable is all that kept its functions.
    llvm::StringSet<> classes = namedClasses(program);
    for (ModuleOp m : modules)
      for (auto c : llvm::make_early_inc_range(m.getOps<P::ClassOp>()))
        if (!classes.contains(c.getName()))
          c.erase();
    // The definitions other modules declare, by module.
    llvm::StringMap<llvm::StringSet<>> declared;
    for (ModuleOp m : modules)
      for (auto f : m.getOps<P::FuncOp>())
        if (f.isExternal())
          if (std::optional<StringRef> module = f.getModule())
            declared[*module].insert(
                f.getLinkSymbol().value_or(f.getSymName()));
    // Public: the entry point, and what a declaration elsewhere names.
    for (ModuleOp m : modules) {
      const bool isMain = m->hasAttr(kMainModuleAttr);
      const llvm::StringSet<> &names = declared[m.getName().value_or("")];
      for (Operation *op : symbols(m)) {
        auto f = dyn_cast<P::FuncOp>(op);
        StringRef name = SymbolTable::getSymbolName(op).getValue();
        bool keep = f && !f.isExternal() &&
                    ((isMain && name == "main") || names.contains(name));
        SymbolTable::setSymbolVisibility(
            op, keep ? SymbolTable::Visibility::Public
                     : SymbolTable::Visibility::Private);
      }
    }
    PassManager pm(program.getContext());
    pm.addPass(createSymbolDCEPass());
    if (failed(pm.run(program)))
      return Status::error("dropping the program's unreachable code failed");
    if (countItems(modules) == before)
      break;
  }

  for (ModuleOp m : modules)
    for (Operation *op : symbols(m)) {
      StringAttr v = visibility.lookup(op);
      if (v)
        op->setAttr(visibilityAttr, v);
      else
        op->removeAttr(visibilityAttr);
    }
  return Status::ok();
}

} // namespace paykan::mlir_backend
