// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (see Lend.h; docs/pir.md §12).

#include "paykan/pir/Lend.h"

#include <string_view>
#include <variant>

namespace paykan::pir {

namespace {

void clearMarks(Block &b, LendCounts &counts) {
  for (Stmt &s : b.Stmts) {
    if (auto *in = std::get_if<Instr>(&s)) {
      if (in->Op == Opcode::Retain && in->Lend) {
        in->Lend = false;
        ++counts.Marks;
      }
    } else if (auto *x = std::get_if<If>(&s)) {
      clearMarks(*x->Then, counts);
      if (x->Else)
        clearMarks(*x->Else, counts);
    } else if (auto *x = std::get_if<While>(&s)) {
      clearMarks(*x->CondBlock, counts);
      clearMarks(*x->Body, counts);
    } else if (auto *x = std::get_if<Switch>(&s)) {
      for (Switch::Case &c : x->Cases)
        clearMarks(*c.Body, counts);
      if (x->Default)
        clearMarks(*x->Default, counts);
    }
  }
}

class LendingPass : public Pass {
public:
  std::string_view name() const override { return "lending"; }
  Status run(Program &program, Statistics &stats) override {
    LendCounts total;
    for (Module &m : program.Modules)
      for (Function &f : m.Functions)
        total.Marks += lend(f).Marks;
    stats.add(name(), "marks", total.Marks);
    return Status::ok();
  }
};

} // namespace

LendCounts lend(Function &f) {
  LendCounts counts;
  if (!f.IsExtern)
    clearMarks(f.Body, counts);
  return counts;
}

std::unique_ptr<Pass> createLendingPass() {
  return std::make_unique<LendingPass>();
}

} // namespace paykan::pir
