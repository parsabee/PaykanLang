// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (see Lend.h; docs/pir.md §12 has the rules and why they
// are right).
//
// A `retain lend %v` is a copy the lowering made only to hand %v to a
// callee.  The pass first takes stock of the function (Facts): where each
// value is defined, how many operands name it, retain it and may take it as
// their own, and what of a local's it is (a load of the local, its address,
// a pointer into what the pointer it holds points to).  Then it judges each
// mark on the function as it is (Judge).  %v must be used by the mark and
// by one box argument of a call later in the mark's block, an argument with
// a bit in the ownership mask of a lending callee; it must be read from a
// local, the root, through loads, unboxes and field and element reads and
// addresses, all in that block; and no statement from the root's load to
// the call, nor one nested in those (the regions `&&`, `||`, `?:` and a
// match lower to), may let the root's object go.  Only then are the lent
// arguments marked and their retains removed: lending an argument removes a
// retain together with the hand-off it paid for, which no other judgement
// counts on, so the verdicts do not depend on the order they are given in.

#include "paykan/pir/Lend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace paykan::pir {

namespace {

bool isCall(Opcode op) {
  return op == Opcode::Call || op == Opcode::VCall || op == Opcode::ICall;
}

bool names(const Operand &o, ValueId v) {
  return o.isValue() && o.valueId() == v;
}

/// Whether operand @p i of @p in may take the reference it names as its
/// own: an argument a call does not lend (the callee may release it, as a
/// runtime `equals` does, keep it or hand it on), or the value a `store`, a
/// `field.store` or a `ptr.store` stores.  `ret` is the other one.
bool handsOn(const Instr &in, size_t i) {
  switch (in.Op) {
  case Opcode::Call:
  case Opcode::VCall:
  case Opcode::ICall:
    return in.ownership(i) != ArgOwnership::Lent;
  case Opcode::Store:
    return i == 0;
  case Opcode::FieldStore:
  case Opcode::PtrStore:
    return i == 1;
  default:
    return false;
  }
}

/// What of a local's a value is, as the window check asks: a load of the
/// local, its address (a `local.addr`, or a `ptr.add` of one), or a pointer
/// into what the pointer the local holds points to (a `ptr.add` of a load
/// of it, as of an `inout` name's or a struct method's `self`).
struct Origin {
  enum Kind : uint8_t { None, Loaded, Address, Pointee };
  Kind K = None;
  LocalId Local = 0;
};

/// A statement's place: its block and its index there.
struct Place {
  Block *B = nullptr;
  size_t Index = 0;
};

/// What the pass knows of a value.
struct ValueFacts {
  /// Where it is defined (no block for a parameter), and by what.
  Place Def;
  const Instr *In = nullptr;
  Origin From;
  /// The operands that name it, anywhere in the function.
  uint32_t Uses = 0;
  /// The `retain`s of it.
  uint32_t Retains = 0;
  /// The operands that may take it as their own (handsOn, `ret`).
  uint32_t HandOns = 0;
};

/// The facts of every value of a function, and its marks in body order.
class Facts {
public:
  explicit Facts(Function &f) : Values(f.NextValueId) { block(f.Body); }

  const ValueFacts &of(ValueId v) const {
    return v < Values.size() ? Values[v] : NoFacts;
  }
  const ValueFacts &of(const Operand &o) const {
    return o.isValue() ? of(o.valueId()) : NoFacts;
  }
  const std::vector<Place> &marks() const { return Marks; }

private:
  std::vector<ValueFacts> Values;
  std::vector<Place> Marks;
  static inline const ValueFacts NoFacts{};

  ValueFacts &at(ValueId v) {
    if (v >= Values.size())
      Values.resize(v + 1);
    return Values[v];
  }

  void use(const Operand &o, bool handsOn) {
    if (!o.isValue())
      return;
    ValueFacts &v = at(o.valueId());
    ++v.Uses;
    if (handsOn)
      ++v.HandOns;
  }

  void block(Block &b) {
    for (size_t i = 0; i < b.Stmts.size(); ++i)
      stmt(b, i);
  }

  void stmt(Block &b, size_t i) {
    Stmt &s = b.Stmts[i];
    if (auto *in = std::get_if<Instr>(&s)) {
      instr(b, i, *in);
    } else if (auto *x = std::get_if<If>(&s)) {
      use(x->Cond, false);
      block(*x->Then);
      if (x->Else)
        block(*x->Else);
    } else if (auto *x = std::get_if<While>(&s)) {
      block(*x->CondBlock);
      use(x->Cond, false);
      block(*x->Body);
    } else if (auto *x = std::get_if<Switch>(&s)) {
      use(x->Value, false);
      for (Switch::Case &c : x->Cases)
        block(*c.Body);
      if (x->Default)
        block(*x->Default);
    } else if (auto *x = std::get_if<Return>(&s); x && x->Value) {
      use(*x->Value, true);
    }
  }

  void instr(Block &b, size_t i, const Instr &in) {
    for (size_t k = 0; k < in.Args.size(); ++k)
      use(in.Args[k], handsOn(in, k));
    for (const ArgOwnership &o : in.Own)
      if (o.K == ArgOwnership::Passed)
        use(o.Bit, false);
    if (in.Op == Opcode::Retain && in.Lend)
      Marks.push_back({&b, i});
    if (in.Op == Opcode::Retain && in.Args.size() == 1 && in.Args[0].isValue())
      ++at(in.Args[0].valueId()).Retains;
    if (in.Result.Id == kNoValue)
      return;
    const Origin from = origin(in);
    ValueFacts &r = at(in.Result.Id);
    r.Def = {&b, i};
    r.In = &in;
    r.From = from;
  }

  Origin origin(const Instr &in) const {
    switch (in.Op) {
    case Opcode::Load:
      return {Origin::Loaded, in.Local};
    case Opcode::LocalAddr:
      return {Origin::Address, in.Local};
    case Opcode::PtrAdd: {
      if (in.Args.empty())
        return {};
      const Origin base = of(in.Args[0]).From;
      if (base.K == Origin::Address)
        return base;
      if (base.K != Origin::None)
        return {Origin::Pointee, base.Local};
      return {};
    }
    default:
      return {};
    }
  }
};

/// The functions and classes a module declares, by name.
class Callees {
public:
  explicit Callees(const Module &m) {
    for (const Function &f : m.Functions)
      Functions.emplace(f.Name, &f);
    for (const Class &c : m.Classes)
      Classes.emplace(c.Name, &c);
  }

  /// Whether argument @p a of the call @p call may be lent: a box parameter
  /// of a lending callee (docs/pir.md §3) with a bit in its ownership mask,
  /// handed over.
  bool lendable(const Instr &call, size_t a) const {
    const Signature *sig = &call.Sig;
    bool lend = false;
    if (call.Op == Opcode::Call) {
      auto it = Functions.find(call.Callee);
      if (it == Functions.end())
        return false;
      sig = &it->second->Sig;
      lend = lends(*it->second);
    } else {
      if (call.Op == Opcode::VCall && !call.ClassName.empty()) {
        auto it = Classes.find(call.ClassName);
        if (it == Classes.end() || call.Slot >= it->second->VTable.size())
          return false;
        sig = &it->second->VTable[call.Slot].Sig;
      }
      lend = lendsType(*sig);
    }
    const size_t first = firstParamArg(call);
    if (!lend || a < first || call.ownership(a) != ArgOwnership::Owned)
      return false;
    const size_t p = a - first;
    return p < sig->Params.size() && sig->Params[p] == Type::Box &&
           boxIndex(*sig, p) < kOwnershipBits;
  }

private:
  std::unordered_map<std::string_view, const Function *> Functions;
  std::unordered_map<std::string_view, const Class *> Classes;
};

/// An argument to lend: the mark that copied it, and the call.
struct Loan {
  Place Mark;
  Instr *Call = nullptr;
  size_t Arg = 0;
};

enum class Verdict : uint8_t { Lend, KeepForWindow, Keep };

/// The verdicts on a function's marks.
class Judge {
public:
  Judge(const Function &f, const Facts &facts, const Callees &callees)
      : F(f), X(facts), C(callees) {}

  /// The verdict on the mark at @p mark; Lend fills @p loan.
  Verdict judge(Place mark, Loan &loan) {
    Block &b = *mark.B;
    const auto &retain = std::get<Instr>(b.Stmts[mark.Index]);
    if (retain.Args.size() != 1 || !retain.Args[0].isValue())
      return Verdict::Keep;
    const ValueId v = retain.Args[0].valueId();
    if (X.of(v).Uses != 2)
      return Verdict::Keep;
    // Its other use: a box argument of a call later in the block, before
    // any terminator (which the verifier lets no statement follow).
    Instr *call = nullptr;
    size_t at = mark.Index + 1;
    for (; at < b.Stmts.size() && !call; ++at) {
      Stmt &s = b.Stmts[at];
      if (std::holds_alternative<Return>(s) ||
          std::holds_alternative<Break>(s) ||
          std::holds_alternative<Continue>(s) ||
          std::holds_alternative<Unreachable>(s))
        return Verdict::Keep;
      auto *in = std::get_if<Instr>(&s);
      if (in && std::any_of(in->Args.begin(), in->Args.end(),
                            [&](const Operand &o) { return names(o, v); }))
        call = in;
    }
    if (!call || !isCall(call->Op))
      return Verdict::Keep;
    const size_t end = at - 1;
    size_t arg = 0;
    while (!names(call->Args[arg], v))
      ++arg;
    if (!C.lendable(*call, arg) || !findRoot(v, b))
      return Verdict::Keep;
    RootTy = F.Locals[Root].Ty;
    if (hazard(b, Start, end))
      return Verdict::KeepForWindow;
    loan = {mark, call, arg};
    return Verdict::Lend;
  }

private:
  const Function &F;
  const Facts &X;
  const Callees &C;
  /// The mark judged: its root, the root's type, the index of the root's
  /// load in the block, and the fields the chain reads.
  LocalId Root = 0;
  Type RootTy = Type::Void;
  size_t Start = 0;
  std::vector<const Instr *> Fields;

  /// Find the root of @p v: a `load` of a local, reached from @p v
  /// through `field.load`, `ptr.load`, `unbox`, `field.addr` and `ptr.add`,
  /// every one in @p b.  False when the chain goes elsewhere (a call's
  /// result, a `select`, a `weak.load`, a parameter, a `local.addr`).
  bool findRoot(ValueId v, const Block &b) {
    Fields.clear();
    for (ValueId cur = v;;) {
      const ValueFacts &d = X.of(cur);
      if (!d.In || d.Def.B != &b)
        return false;
      switch (d.In->Op) {
      case Opcode::Load:
        Root = d.In->Local;
        Start = d.Def.Index;
        return Root < F.Locals.size();
      case Opcode::FieldLoad:
        Fields.push_back(d.In);
        [[fallthrough]];
      case Opcode::PtrLoad:
      case Opcode::Unbox:
      case Opcode::FieldAddr:
      case Opcode::PtrAdd:
        if (d.In->Args.empty() || !d.In->Args[0].isValue())
          return false;
        cur = d.In->Args[0].valueId();
        break;
      default:
        return false;
      }
    }
  }

  /// Whether a statement of @p b from @p from to @p to, both included, or
  /// one nested in them, may let the root's object go.
  bool hazard(const Block &b, size_t from, size_t to) const {
    for (size_t i = from; i <= to; ++i)
      if (hazard(b.Stmts[i]))
        return true;
    return false;
  }

  bool hazard(const Block &b) const {
    return !b.Stmts.empty() && hazard(b, 0, b.Stmts.size() - 1);
  }

  bool hazard(const Stmt &s) const {
    if (const auto *in = std::get_if<Instr>(&s))
      return hazard(*in);
    if (const auto *x = std::get_if<If>(&s))
      return hazard(*x->Then) || (x->Else && hazard(*x->Else));
    if (const auto *x = std::get_if<While>(&s))
      return hazard(*x->CondBlock) || hazard(*x->Body);
    if (const auto *x = std::get_if<Switch>(&s))
      return std::any_of(
                 x->Cases.begin(), x->Cases.end(),
                 [&](const Switch::Case &c) { return hazard(*c.Body); }) ||
             (x->Default && hazard(*x->Default));
    if (const auto *x = std::get_if<Return>(&s); x && x->Value)
      return exposes(nullptr, 0, *x->Value);
    return false;
  }

  /// A store into the root or its address; a `field.store` into a field
  /// the chain reads; or an operand exposes the root's object.
  bool hazard(const Instr &in) const {
    if ((in.Op == Opcode::Store || in.Op == Opcode::LocalAddr) &&
        in.Local == Root)
      return true;
    if (in.Op == Opcode::FieldStore &&
        std::any_of(Fields.begin(), Fields.end(), [&](const Instr *load) {
          return load->ClassName == in.ClassName && load->Field == in.Field;
        }))
      return true;
    for (size_t i = 0; i < in.Args.size(); ++i)
      if (exposes(&in, i, in.Args[i]))
        return true;
    return false;
  }

  /// Whether operand @p o, operand @p i of @p in (of a `ret` when @p in is
  /// null), may let the root's object go: the root's address, used at all;
  /// the pointer it holds, used but to read through it (`ptr.load`,
  /// `ptr.add`); a box loaded from it, released, cast, selected, or handed
  /// on where the function retains it fewer times than it hands it on (the
  /// reference handed on may be the root's own); an object loaded from it,
  /// freed.
  bool exposes(const Instr *in, size_t i, const Operand &o) const {
    const ValueFacts &v = X.of(o);
    if (v.From.K == Origin::None || v.From.Local != Root)
      return false;
    if (v.From.K == Origin::Address)
      return true;
    if (RootTy == Type::Ptr)
      return !in || !((in->Op == Opcode::PtrLoad || in->Op == Opcode::PtrAdd) &&
                      i == 0);
    if (v.From.K != Origin::Loaded)
      return true;
    if (RootTy == Type::Obj)
      return in && in->Op == Opcode::Free;
    if (!in || handsOn(*in, i))
      return v.HandOns > v.Retains;
    switch (in->Op) {
    case Opcode::Retain:
    case Opcode::Unbox:
    case Opcode::Cmp:
    case Opcode::WeakNew:
    case Opcode::Call: // an argument the call lends
    case Opcode::VCall:
    case Opcode::ICall:
      return false;
    default:
      return true;
    }
  }
};

LendCounts lendIn(Function &f, const Callees &callees) {
  LendCounts counts;
  if (f.IsExtern)
    return counts;
  const Facts facts(f);
  Judge judge(f, facts, callees);
  std::vector<Loan> loans;
  for (Place mark : facts.marks()) {
    ++counts.Marks;
    Loan loan;
    switch (judge.judge(mark, loan)) {
    case Verdict::Lend:
      loans.push_back(loan);
      ++counts.Lent;
      break;
    case Verdict::KeepForWindow:
      ++counts.KeptForWindow;
      break;
    case Verdict::Keep:
      break;
    }
  }
  for (Place mark : facts.marks())
    std::get<Instr>(mark.B->Stmts[mark.Index]).Lend = false;
  for (const Loan &l : loans) {
    if (l.Call->Own.empty())
      l.Call->Own.resize(l.Call->Args.size());
    l.Call->Own[l.Arg].K = ArgOwnership::Lent;
  }
  // The retains go last, the later first in each block, so that the
  // places and the calls found stay right until then.
  std::sort(loans.begin(), loans.end(), [](const Loan &a, const Loan &b) {
    return a.Mark.Index > b.Mark.Index;
  });
  for (const Loan &l : loans)
    l.Mark.B->Stmts.erase(l.Mark.B->Stmts.begin() +
                          static_cast<std::ptrdiff_t>(l.Mark.Index));
  return counts;
}

class LendingPass : public Pass {
public:
  std::string_view name() const override { return "lending"; }
  Status run(Program &program, Statistics &stats) override {
    LendCounts total;
    for (Module &m : program.Modules) {
      const Callees callees(m);
      for (Function &f : m.Functions) {
        const LendCounts c = lendIn(f, callees);
        total.Marks += c.Marks;
        total.Lent += c.Lent;
        total.KeptForWindow += c.KeptForWindow;
      }
    }
    stats.add(name(), "marks", total.Marks);
    stats.add(name(), "arguments lent", total.Lent);
    stats.add(name(), "copies kept for the window", total.KeptForWindow);
    return Status::ok();
  }
};

} // namespace

LendCounts lend(Function &f, const Module &m) { return lendIn(f, Callees(m)); }

std::unique_ptr<Pass> createLendingPass() {
  return std::make_unique<LendingPass>();
}

} // namespace paykan::pir
