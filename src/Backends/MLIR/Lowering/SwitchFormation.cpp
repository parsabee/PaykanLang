// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// Switch formation (SwitchFormation.h).  A chain is
//
//   scf.if (x == c0) { A } else {
//     [constants]  %t = arith.cmpi eq, x, c1
//     scf.if %t { B } else { ...the rest of the chain, or the default... }
//     scf.yield <the nested scf.if's results>
//   }
//
// with the same integer x in every link, and it becomes
//
//   scf.index_switch (index_cast x)
//     case c0 { A }  case c1 { B }  ...  default { ... }
//
// A link testing a value an earlier link tested can never be taken, and is
// dropped.  Every case value is non-negative, so that the C path's switch
// (on a size_t, index's C type) and the LLVM path's (on an i64) take the
// same case for every x.

#include "SwitchFormation.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/Matchers.h>

#include <optional>
#include <utility>

namespace paykan::mlir_backend {

using namespace ::mlir;

namespace {

/// The test of one link: `Subject == Case`.
struct Test {
  Value Subject;
  int64_t Case;
};

/// The test of @p op when it is `x == c` (or `c == x`) for an integer x
/// wider than a bit and a constant c >= 0.
std::optional<Test> equalityTest(scf::IfOp op) {
  auto cmp = op.getCondition().getDefiningOp<arith::CmpIOp>();
  if (!cmp || cmp.getPredicate() != arith::CmpIPredicate::eq)
    return std::nullopt;
  Value x = cmp.getLhs(), c = cmp.getRhs();
  APInt value;
  if (!matchPattern(c, m_ConstantInt(&value))) {
    std::swap(x, c);
    if (!matchPattern(c, m_ConstantInt(&value)))
      return std::nullopt;
  }
  auto type = dyn_cast<IntegerType>(x.getType());
  if (!type || type.getWidth() < 2 || type.getWidth() > 64 ||
      value.isNegative())
    return std::nullopt;
  return Test{x, value.getSExtValue()};
}

/// The next link after @p op: the scf.if its else block ends with, when the
/// block holds nothing else but constants and that if's test, and yields
/// what that if does.
scf::IfOp nextLink(scf::IfOp op) {
  if (op.getElseRegion().empty())
    return {};
  Block &block = op.getElseRegion().front();
  Operation *yield = block.getTerminator();
  auto next = dyn_cast_or_null<scf::IfOp>(yield->getPrevNode());
  if (!next || !llvm::equal(yield->getOperands(), next.getResults()))
    return {};
  Operation *test = next.getCondition().getDefiningOp();
  for (Operation &o : block)
    if (&o != yield && &o != next && !isa<arith::ConstantOp>(o) &&
        !(&o == test && o.hasOneUse()))
      return {};
  return next;
}

/// Rewrite the chain @p links (two or more, outermost first) as an
/// scf.index_switch on @p subject.  Every operation the rewrite erases goes
/// into @p erased.
void rewrite(ArrayRef<std::pair<scf::IfOp, int64_t>> links, Value subject,
             llvm::DenseSet<Operation *> &erased) {
  SmallVector<int64_t> values;
  SmallVector<scf::IfOp> cases;
  llvm::SmallDenseSet<int64_t> seen;
  for (auto [op, value] : links)
    if (seen.insert(value).second) {
      values.push_back(value);
      cases.push_back(op);
    }
  if (values.size() < 2)
    return;

  scf::IfOp head = links.front().first;
  // The constants of the else blocks the switch replaces, which the cases
  // may use: before the switch, which dominates all of them.
  SmallVector<Operation *> hoisted;
  for (auto [op, value] : links.drop_back())
    for (Operation &o : llvm::make_early_inc_range(op.getElseRegion().front()))
      if (isa<arith::ConstantOp>(o)) {
        o.moveBefore(head);
        hoisted.push_back(&o);
      }

  OpBuilder b(head);
  Location loc = head.getLoc();
  Value index = arith::IndexCastOp::create(b, loc, b.getIndexType(), subject);
  auto sw = scf::IndexSwitchOp::create(b, loc, head.getResultTypes(), index,
                                       values, values.size());
  for (auto [region, op] : llvm::zip(sw.getCaseRegions(), cases))
    region.takeBody(op.getThenRegion());
  scf::IfOp last = links.back().first;
  Region &rest = last.getElseRegion();
  if (!rest.empty()) {
    sw.getDefaultRegion().takeBody(rest);
  } else {
    b.createBlock(&sw.getDefaultRegion());
    scf::YieldOp::create(b, loc);
  }

  head.replaceAllUsesWith(sw.getResults());
  Operation *test = head.getCondition().getDefiningOp();
  head->walk([&](Operation *o) { erased.insert(o); });
  head.erase();
  // The tests are gone, and the constants only they used.
  if (test && test->use_empty())
    test->erase();
  for (Operation *o : hoisted)
    if (o->use_empty())
      o->erase();
}

struct SwitchFormationPass
    : public PassWrapper<SwitchFormationPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SwitchFormationPass)

  StringRef getArgument() const override { return "pir-form-switches"; }
  StringRef getDescription() const override {
    return "Turn chains of scf.if testing one integer against constants "
           "into scf.index_switch";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<arith::ArithDialect, scf::SCFDialect>();
  }
  void runOnOperation() override { formSwitches(getOperation()); }
};

} // namespace

void formSwitches(Operation *root) {
  // Outermost first, so a chain is taken whole from its first link.  A case
  // moves into the switch with whatever it holds, which is visited later.
  SmallVector<scf::IfOp> ifs;
  root->walk<WalkOrder::PreOrder>([&](scf::IfOp op) { ifs.push_back(op); });
  llvm::DenseSet<Operation *> erased;
  for (scf::IfOp op : ifs) {
    if (erased.contains(op))
      continue;
    std::optional<Test> test = equalityTest(op);
    if (!test)
      continue;
    SmallVector<std::pair<scf::IfOp, int64_t>> links{{op, test->Case}};
    for (scf::IfOp next = nextLink(op); next; next = nextLink(next)) {
      std::optional<Test> t = equalityTest(next);
      if (!t || t->Subject != test->Subject)
        break;
      links.push_back({next, t->Case});
    }
    if (links.size() >= 2)
      rewrite(links, test->Subject, erased);
  }
}

std::unique_ptr<Pass> createSwitchFormationPass() {
  return std::make_unique<SwitchFormationPass>();
}

} // namespace paykan::mlir_backend
