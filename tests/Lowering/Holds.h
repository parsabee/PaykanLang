// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The holds of a lowered program (docs/pir.md §7, "Reads a call may
// replace"), which the code passes must keep (docs/pir.md §12): the
// lowering tests check every program they lower, the round trips every
// program of the corpus.

#pragma once

#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/PIR.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/RCElimination.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace paykan::test {

/// The holds of a program: each box read from a field or an element
/// (`field.load`, `ptr.load`, stable or not) that is retained, and used
/// after a call that may run code (of a function, a method, a closure; not
/// of the runtime) made after its first retain.  The callee may let the
/// field or the element go, so some retain must keep the object alive
/// across the call (docs/pir.md §7, "Reads a call may replace").
struct Holds {
  struct Read {
    std::string Module, Function;
    pir::ValueId Value = pir::kNoValue;
    bool operator==(const Read &) const = default;
  };
  /// The holds.
  std::vector<Read> Held;
  /// Every read retained, held or not.
  std::vector<Read> Retained;

  explicit Holds(const pir::Program &p) {
    for (const pir::Module &m : p.Modules)
      for (const pir::Function &f : m.Functions)
        function(m, f);
  }

  bool retained(const Read &r) const {
    return std::find(Retained.begin(), Retained.end(), r) != Retained.end();
  }

private:
  struct Facts {
    std::vector<bool> Read;          // by value: a box read in place
    std::vector<size_t> FirstRetain; // its first retain's position
    std::vector<size_t> Retains;     // how many it has
    std::vector<size_t> LastUse;     // its last use's position
    std::vector<size_t> Calls;       // the calls' positions
    size_t Next = 0;
  };

  static bool runsCode(const pir::Instr &in) {
    return in.Op == pir::Opcode::VCall || in.Op == pir::Opcode::ICall ||
           (in.Op == pir::Opcode::Call && !pir::isRuntimeName(in.Callee));
  }

  static void block(const pir::Block &b, Facts &f) {
    for (const pir::Stmt &s : b.Stmts) {
      const size_t at = f.Next++;
      std::vector<const pir::Operand *> used;
      if (const auto *in = std::get_if<pir::Instr>(&s)) {
        if ((in->Op == pir::Opcode::FieldLoad ||
             in->Op == pir::Opcode::PtrLoad) &&
            in->Result.Ty == pir::Type::Box)
          f.Read[in->Result.Id] = true;
        if (in->Op == pir::Opcode::Retain && in->Args[0].isValue()) {
          const pir::ValueId v = in->Args[0].valueId();
          if (f.Retains[v]++ == 0)
            f.FirstRetain[v] = at;
        }
        if (runsCode(*in))
          f.Calls.push_back(at);
        for (const pir::Operand &o : in->Args)
          used.push_back(&o);
      } else if (const auto *x = std::get_if<pir::If>(&s)) {
        block(*x->Then, f);
        if (x->Else)
          block(*x->Else, f);
      } else if (const auto *x = std::get_if<pir::While>(&s)) {
        block(*x->CondBlock, f);
        block(*x->Body, f);
      } else if (const auto *x = std::get_if<pir::Switch>(&s)) {
        for (const pir::Switch::Case &c : x->Cases)
          block(*c.Body, f);
        if (x->Default)
          block(*x->Default, f);
      } else if (const auto *x = std::get_if<pir::Return>(&s)) {
        if (x->Value)
          used.push_back(&*x->Value);
      }
      for (const pir::Operand *o : used)
        if (o->isValue())
          f.LastUse[o->valueId()] = at;
    }
  }

  void function(const pir::Module &m, const pir::Function &fn) {
    Facts f;
    f.Read.resize(fn.NextValueId);
    f.FirstRetain.resize(fn.NextValueId);
    f.Retains.resize(fn.NextValueId);
    f.LastUse.resize(fn.NextValueId);
    block(fn.Body, f);
    for (pir::ValueId v = 1; v < fn.NextValueId; ++v) {
      if (!f.Read[v] || f.Retains[v] == 0)
        continue;
      Retained.push_back({m.Name, fn.Name, v});
      if (std::any_of(f.Calls.begin(), f.Calls.end(), [&](size_t c) {
            return f.FirstRetain[v] < c && c < f.LastUse[v];
          }))
        Held.push_back(Retained.back());
    }
  }
};

/// @p program, verified, through the code passes the driver runs on every
/// program, verified after each: the moves at last use, then the
/// reference-count elimination (docs/pir.md §12).  "" when every hold the
/// moves leave keeps a retain after the elimination, else what failed.
inline std::string codePassesKeepHolds(pir::Program &program) {
  pir::PassManager moves(/*verifyEach=*/true);
  moves.add(pir::createMovesAtLastUsePass());
  if (Status s = moves.run(program); !s)
    return s.message();
  const Holds holds(program);
  pir::PassManager elimination(/*verifyEach=*/true);
  elimination.add(pir::createRCEliminationPass());
  if (Status s = elimination.run(program); !s)
    return s.message();
  const Holds left(program);
  for (const Holds::Read &r : holds.Held)
    if (!left.retained(r))
      return "the read %" + std::to_string(r.Value) + " of @" + r.Function +
             " lost its hold";
  return "";
}

} // namespace paykan::test
