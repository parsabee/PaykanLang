// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The lending pass (see Lend.h; docs/pir.md §12 has the rules and why they
// are right).
//
// The caller half.  A `retain lend %v` is a copy the lowering made only to
// hand %v to a callee.  The pass first takes stock of the function (Facts):
// where each value is defined, how many operands name it, retain it and may
// take it as their own, and what of a local's it is (a load of the local,
// its address, a pointer into what the pointer it holds points to).  Then
// it judges each mark on the function as it is (Judge).  %v must be used by
// the mark and by one box argument of a call later in the mark's block, an
// argument with a bit in the ownership mask of a lending callee; it must be
// read from a local, the root, through loads, unboxes and field and element
// reads and addresses, all in that block; and no statement from the root's
// load to the call, nor one nested in those (the regions `&&`, `||`, `?:`
// and a match lower to), may let the root's object go.  Only then are the
// lent arguments marked and their retains removed: lending an argument
// removes a retain together with the hand-off it paid for, which no other
// judgement counts on, so the verdicts do not depend on the order they are
// given in.
//
// The callee half, on the function the caller half left.  Each box
// parameter with a bit starts the body with its prologue, which retains it
// when it was lent, so the body owns it either way.  The pass finds the
// prologues at the top (prologues), indexes every use of every value and
// what each local is (Sites), and judges each parameter (ParamJudge): one
// the body never writes, whose own reference (the parameter, a load of its
// slot) it only releases, hands to a call, stores, or returns, at places it
// can tell apart from the copies the body retains first, is lent.  Every
// verdict is taken before anything changes; then each lent parameter's
// prologue loses its `if` and each place that lets go of its reference asks
// its bit (Edits).
#include "paykan/pir/Lend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

  bool operator==(const Place &) const = default;
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

LendCounts lendArgumentsIn(Function &f, const Callees &callees) {
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

// -- The callee half: the parameters

/// Where a value is used: the statement, and how: as operand Index of an
/// instruction, as a passed bit, as the value an if, a while or a switch
/// tests, or as what a `ret` returns.
struct Site {
  enum Kind : uint8_t { Arg, Bit, Test, Ret };
  Place At;
  Kind K = Arg;
  size_t Index = 0;
};

/// Every use of every value of a function, where each is defined, and what
/// each local is.
class Sites {
public:
  explicit Sites(Function &f)
      : Stores(f.Locals.size()), Loads(f.Locals.size()),
        Addresses(f.Locals.size()), Uses(f.NextValueId), Defs(f.NextValueId) {
    block(f.Body);
  }

  const std::vector<Site> &of(ValueId v) const {
    return v < Uses.size() ? Uses[v] : NoSites;
  }
  /// The instruction defining @p v (no block for a parameter).
  Place def(ValueId v) const { return v < Defs.size() ? Defs[v] : Place(); }

  /// By local: the stores into it, and the values loaded from it and
  /// taken as its address.
  std::vector<std::vector<Place>> Stores;
  std::vector<std::vector<ValueId>> Loads;
  std::vector<std::vector<ValueId>> Addresses;

private:
  std::vector<std::vector<Site>> Uses;
  std::vector<Place> Defs;
  static inline const std::vector<Site> NoSites{};

  void use(const Operand &o, Site s) {
    if (!o.isValue())
      return;
    if (o.valueId() >= Uses.size())
      Uses.resize(o.valueId() + 1);
    Uses[o.valueId()].push_back(s);
  }

  void block(Block &b) {
    for (size_t i = 0; i < b.Stmts.size(); ++i)
      stmt(b, i);
  }

  void stmt(Block &b, size_t i) {
    const Place at{&b, i};
    Stmt &s = b.Stmts[i];
    if (auto *in = std::get_if<Instr>(&s)) {
      for (size_t k = 0; k < in->Args.size(); ++k)
        use(in->Args[k], {at, Site::Arg, k});
      for (const ArgOwnership &o : in->Own)
        if (o.K == ArgOwnership::Passed)
          use(o.Bit, {at, Site::Bit, 0});
      const ValueId r = in->Result.Id;
      if (r != kNoValue) {
        if (r >= Defs.size())
          Defs.resize(r + 1);
        Defs[r] = at;
      }
      if (in->Local < Stores.size()) {
        if (in->Op == Opcode::Store)
          Stores[in->Local].push_back(at);
        else if (in->Op == Opcode::Load && r != kNoValue)
          Loads[in->Local].push_back(r);
        else if (in->Op == Opcode::LocalAddr && r != kNoValue)
          Addresses[in->Local].push_back(r);
      }
    } else if (auto *x = std::get_if<If>(&s)) {
      use(x->Cond, {at, Site::Test, 0});
      block(*x->Then);
      if (x->Else)
        block(*x->Else);
    } else if (auto *x = std::get_if<While>(&s)) {
      block(*x->CondBlock);
      use(x->Cond, {at, Site::Test, 0});
      block(*x->Body);
    } else if (auto *x = std::get_if<Switch>(&s)) {
      use(x->Value, {at, Site::Test, 0});
      for (Switch::Case &c : x->Cases)
        block(*c.Body);
      if (x->Default)
        block(*x->Default);
    } else if (auto *x = std::get_if<Return>(&s); x && x->Value) {
      use(*x->Value, {at, Site::Ret, 0});
    }
  }
};

const Instr *instrAt(const Place &p) {
  return p.B && p.Index < p.B->Stmts.size()
             ? std::get_if<Instr>(&p.B->Stmts[p.Index])
             : nullptr;
}

bool isNullBox(const Operand &o) {
  const auto *n = std::get_if<Operand::Null>(&o.V);
  return n && n->Ty == Type::Box;
}

/// Whether no value reaches a switch's absent default: its cases are a
/// `bool`'s `true` and `false`.
bool exhaustive(const Switch &sw) {
  bool seen[2] = {false, false};
  for (const Switch::Case &c : sw.Cases)
    if (const auto *b = std::get_if<bool>(&c.Value.V))
      seen[*b ? 1 : 0] = true;
  return seen[0] && seen[1];
}

/// Whether a path through @p s may leave the block that holds it other
/// than at its end: a `ret`, an `unreachable`, or a `break` or `continue`
/// of a loop around it (not of a loop inside it, when @p inLoop).
bool mayLeave(const Stmt &s, bool inLoop) {
  auto any = [&](const Block *b, bool loop) {
    return b && std::any_of(b->Stmts.begin(), b->Stmts.end(),
                            [&](const Stmt &t) { return mayLeave(t, loop); });
  };
  if (const auto *x = std::get_if<If>(&s))
    return any(x->Then.get(), inLoop) || any(x->Else.get(), inLoop);
  if (const auto *x = std::get_if<Switch>(&s))
    return any(x->Default.get(), inLoop) ||
           std::any_of(x->Cases.begin(), x->Cases.end(),
                       [&](const Switch::Case &c) {
                         return any(c.Body.get(), inLoop);
                       });
  if (const auto *x = std::get_if<While>(&s))
    return any(x->CondBlock.get(), true) || any(x->Body.get(), true);
  if (std::holds_alternative<Break>(s) || std::holds_alternative<Continue>(s))
    return !inLoop;
  return std::holds_alternative<Return>(s) ||
         std::holds_alternative<Unreachable>(s);
}

/// Whether parameter @p k of @p f is a box parameter with a bit in the
/// ownership mask (docs/pir.md §3).
bool hasBit(const Function &f, size_t k) {
  return f.Params[k].Ty == Type::Box && boxIndex(f.Sig, k) < kOwnershipBits;
}

/// A box parameter's prologue at the top of its function's body
/// (docs/pir.md §3): `%o = owned %p`, `%l = not %o`, `if %l { retain %p }`.
struct Prologue {
  size_t Param = 0;
  /// The index of the `owned` in the body; the `not` and the `if` follow.
  size_t At = 0;
  ValueId Own = kNoValue;
  ValueId Lent = kNoValue;
};

/// The prologues at the top of @p f's body.  The `owned` of a parameter
/// the pass lent already stays there without the rest of its prologue
/// (with the `not` of it a keep asks, when the body keeps the parameter).
std::vector<Prologue> prologues(const Function &f) {
  std::vector<Prologue> found;
  const std::vector<Stmt> &body = f.Body.Stmts;
  auto isNotOf = [&](size_t i, ValueId v) {
    const Instr *in = i < body.size() ? std::get_if<Instr>(&body[i]) : nullptr;
    return in && in->Op == Opcode::Not && in->Args.size() == 1 &&
           names(in->Args[0], v) && in->Result.Id != kNoValue;
  };
  for (size_t i = 0; i < body.size();) {
    const auto *own = std::get_if<Instr>(&body[i]);
    if (!own || own->Op != Opcode::Owned || own->Args.size() != 1)
      break;
    size_t k = 0;
    while (k < f.Params.size() && !names(own->Args[0], f.Params[k].Id))
      ++k;
    if (k == f.Params.size())
      break;
    const ValueId o = own->Result.Id;
    if (isNotOf(i + 1, o) && i + 2 < body.size()) {
      const ValueId l = std::get<Instr>(body[i + 1]).Result.Id;
      const auto *x = std::get_if<If>(&body[i + 2]);
      const Instr *retain = x && names(x->Cond, l) &&
                                    (!x->Else || x->Else->Stmts.empty()) &&
                                    x->Then->Stmts.size() == 1
                                ? std::get_if<Instr>(&x->Then->Stmts[0])
                                : nullptr;
      if (retain && retain->Op == Opcode::Retain && retain->Args.size() == 1 &&
          names(retain->Args[0], f.Params[k].Id)) {
        found.push_back({k, i, o, l});
        i += 3;
        continue;
      }
    }
    i += isNotOf(i + 1, o) ? 2 : 1;
  }
  return found;
}

/// Where a body lets go of a parameter's own reference: a `release`; an
/// argument of a lending callee's box parameter with a bit, which can hand
/// it on with the parameter's bit (a forward); or a store into another
/// local, a field.store, a ptr.store, a `ret`, an argument of any other
/// callee, which keep it (a keep).
struct Taking {
  enum Kind : uint8_t { Release, Forward, Keep };
  Place At;
  Kind K = Release;
  /// Forward: the call's argument.
  size_t Arg = 0;
  /// The value let go of.
  ValueId V = kNoValue;
};

/// Whether every path through a function body from its entry to a `ret`
/// lets go of a parameter's own reference exactly once, as the body does
/// when it is right (a path that ends in `unreachable` may let go of none),
/// counting a load of the parameter's slot only while the slot is not
/// null: after the null the moves at last use store at a hand-off, a
/// release of the slot releases nothing.
class Once {
public:
  /// @p at: by statement, how many times it lets go of the parameter
  /// itself; @p loads: the loads of the slot whose value is let go of (in
  /// the load's block, with only instructions between), counted where they
  /// load; @p nulls: the stores of null into the slot.
  Once(std::unordered_map<const Stmt *, unsigned> at,
       std::unordered_set<const Stmt *> loads,
       std::unordered_set<const Stmt *> nulls)
      : At(std::move(at)), Loads(std::move(loads)), Nulls(std::move(nulls)) {}

  bool run(const Block &body) {
    const Paths end = block(body, bit(0, false));
    if (end)
      exit(end);
    return Ok;
  }

private:
  /// The paths at a point: bit 2k + n is one that let go of the reference
  /// k times (2: twice or more), with the slot null (n = 1) or not.
  using Paths = uint8_t;

  struct Loop {
    Paths Breaks = 0;
    Paths Continues = 0;
  };

  std::unordered_map<const Stmt *, unsigned> At;
  std::unordered_set<const Stmt *> Loads;
  std::unordered_set<const Stmt *> Nulls;
  std::vector<Loop> Loops;
  bool Ok = true;

  static constexpr Paths bit(unsigned k, bool null) {
    return static_cast<Paths>(1u << (2 * k + (null ? 1 : 0)));
  }

  /// @p p after letting go @p n times, but on the paths where the slot is
  /// null when @p ofSlot.
  static Paths took(Paths p, unsigned n, bool ofSlot) {
    Paths out = 0;
    for (unsigned k = 0; k < 3; ++k)
      for (bool null : {false, true})
        if (p & bit(k, null))
          out |= ofSlot && null ? bit(k, null) : bit(std::min(k + n, 2u), null);
    return out;
  }

  static Paths nulled(Paths p) {
    Paths out = 0;
    for (unsigned k = 0; k < 3; ++k)
      if (p & (bit(k, false) | bit(k, true)))
        out |= bit(k, true);
    return out;
  }

  /// A path leaving the function: it let go once.
  void exit(Paths p) {
    if (p & ~(bit(1, false) | bit(1, true)))
      Ok = false;
  }

  Paths block(const Block &b, Paths p) {
    for (const Stmt &s : b.Stmts)
      p = stmt(s, p);
    return p;
  }

  Paths stmt(const Stmt &s, Paths p) {
    if (!p)
      return p;
    if (Nulls.contains(&s))
      p = nulled(p);
    if (Loads.contains(&s))
      p = took(p, 1, /*ofSlot=*/true);
    if (auto it = At.find(&s); it != At.end())
      p = took(p, it->second, /*ofSlot=*/false);
    if (std::holds_alternative<Instr>(s))
      return p;
    if (const auto *x = std::get_if<If>(&s))
      return static_cast<Paths>(block(*x->Then, p) |
                                (x->Else ? block(*x->Else, p) : p));
    if (const auto *x = std::get_if<Switch>(&s)) {
      Paths out = x->Default       ? block(*x->Default, p)
                  : exhaustive(*x) ? Paths(0)
                                   : p;
      for (const Switch::Case &c : x->Cases)
        out |= block(*c.Body, p);
      return out;
    }
    if (const auto *x = std::get_if<While>(&s)) {
      Paths head = p;
      for (;;) {
        Loops.emplace_back();
        const Paths cond = block(*x->CondBlock, head);
        const Paths body = block(*x->Body, cond);
        const Loop l = Loops.back();
        Loops.pop_back();
        // The paths only ever grow, over six bits: the loop settles.
        const auto next = static_cast<Paths>(head | body | l.Continues);
        if (next == head)
          return static_cast<Paths>(cond | l.Breaks);
        head = next;
      }
    }
    if (std::holds_alternative<Return>(s)) {
      exit(p);
    } else if (std::holds_alternative<Break>(s) && !Loops.empty()) {
      Loops.back().Breaks |= p;
    } else if (std::holds_alternative<Continue>(s) && !Loops.empty()) {
      Loops.back().Continues |= p;
    }
    // `unreachable`: the program stops.
    return 0;
  }
};

/// The verdicts on a function's box parameters.
class ParamJudge {
public:
  ParamJudge(Function &f, const Sites &sites, const Callees &callees)
      : F(f), S(sites), C(callees) {}

  /// Whether the parameter of @p pr may be lent: the body never writes it
  /// and lets go of its own reference only where @p takings, filled, says,
  /// once on every path.
  bool judge(const Prologue &pr, std::vector<Taking> &takings) const {
    const ValueId p = F.Params[pr.Param].Id;
    const auto &x = std::get<If>(F.Body.Stmts[pr.At + 2]);
    const std::vector<Place> prologue = {{&F.Body, pr.At}, {x.Then.get(), 0}};
    // The slot the body stores the parameter in at its top, if any.
    std::optional<LocalId> slot;
    Place entry;
    for (const Site &u : S.of(p)) {
      const Instr *in = instrAt(u.At);
      if (u.K != Site::Arg || !in || in->Op != Opcode::Store ||
          u.At.B != &F.Body)
        continue;
      if (slot)
        return false;
      slot = in->Local;
      entry = u.At;
    }
    std::vector<Place> skip = prologue;
    std::unordered_set<const Stmt *> nulls;
    if (slot) {
      if (*slot >= F.Locals.size() || F.Locals[*slot].Ty != Type::Box ||
          !S.Addresses[*slot].empty())
        return false;
      // Written: a value but null stored into the slot after its entry.
      for (const Place &at : S.Stores[*slot]) {
        if (at == entry)
          continue;
        if (!isNullBox(instrAt(at)->Args[0]))
          return false;
        nulls.insert(&at.B->Stmts[at.Index]);
      }
      skip.push_back(entry);
    }
    // What the parameter's value itself lets go of: nothing more when its
    // reference is the slot's.
    std::unordered_map<const Stmt *, unsigned> at;
    std::unordered_set<const Stmt *> loads;
    const size_t first = takings.size();
    if (!ownTaking(p, skip, takings) || (slot && takings.size() != first))
      return false;
    for (size_t i = first; i < takings.size(); ++i)
      ++at[&takings[i].At.B->Stmts[takings[i].At.Index]];
    if (slot)
      for (ValueId v : S.Loads[*slot]) {
        const size_t before = takings.size();
        if (!ownTaking(v, {}, takings))
          return false;
        if (takings.size() == before)
          continue;
        // Counted at the load, whose slot is null or not then: what lets
        // go of it must follow in its block, with no way out between.
        const Place def = S.def(v);
        const Place use = takings.back().At;
        if (use.B != def.B || use.Index < def.Index)
          return false;
        for (size_t i = def.Index + 1; i < use.Index; ++i)
          if (mayLeave(def.B->Stmts[i], /*inLoop=*/false))
            return false;
        loads.insert(&def.B->Stmts[def.Index]);
      }
    return Once(std::move(at), std::move(loads), std::move(nulls)).run(F.Body);
  }

  /// Whether the body asks the bit of box parameter @p k (`owned`).
  bool asked(size_t k) const {
    return std::any_of(S.of(F.Params[k].Id).begin(), S.of(F.Params[k].Id).end(),
                       [](const Site &u) {
                         const Instr *in = instrAt(u.At);
                         return u.K == Site::Arg && in &&
                                in->Op == Opcode::Owned;
                       });
  }

  /// Whether a use of box parameter @p k takes its reference (any use but
  /// an unbox, a cmp, a weak.new and a lent argument).
  bool taken(size_t k) const {
    for (const Site &u : S.of(F.Params[k].Id)) {
      Taking t;
      if (classify(u, F.Params[k].Id, t) != UseKind::Borrow)
        return true;
    }
    return false;
  }

private:
  Function &F;
  const Sites &S;
  const Callees &C;

  enum class UseKind : uint8_t { Borrow, Retain, Take, Unknown };

  /// Whether box local @p l only lends what is stored into it to the C
  /// functions its address is given to (a `native fn` body's argument
  /// passed by address, docs/pir.md §3): it is never loaded, and its
  /// address is only an argument of module-less externs, which keep
  /// nothing they are not given a reference for.
  bool borrowingTemporary(LocalId l) const {
    if (l >= F.Locals.size() || F.Locals[l].Ty != Type::Box ||
        !S.Loads[l].empty())
      return false;
    for (ValueId a : S.Addresses[l])
      for (const Site &u : S.of(a)) {
        const Instr *in = instrAt(u.At);
        if (u.K != Site::Arg || !in || in->Op != Opcode::Call ||
            !(isRuntimeName(in->Callee) || isNativeName(in->Callee)))
          return false;
      }
    return true;
  }

  /// What use @p u of the box @p v does with it; a Take is described in
  /// @p t.
  UseKind classify(const Site &u, ValueId v, Taking &t) const {
    t.At = u.At;
    t.V = v;
    if (u.K == Site::Ret) {
      t.K = Taking::Keep;
      return UseKind::Take;
    }
    const Instr *in = instrAt(u.At);
    if (u.K != Site::Arg || !in)
      return UseKind::Unknown;
    switch (in->Op) {
    case Opcode::Retain:
      return UseKind::Retain;
    case Opcode::Release:
      t.K = Taking::Release;
      return UseKind::Take;
    case Opcode::Unbox:
    case Opcode::Cmp:
    case Opcode::WeakNew:
      return UseKind::Borrow;
    case Opcode::Call:
    case Opcode::VCall:
    case Opcode::ICall:
      if (u.Index < firstParamArg(*in))
        return UseKind::Unknown;
      switch (in->ownership(u.Index)) {
      case ArgOwnership::Lent:
        return UseKind::Borrow;
      case ArgOwnership::Passed:
        return UseKind::Unknown;
      case ArgOwnership::Owned:
        break;
      }
      t.K = C.lendable(*in, u.Index) ? Taking::Forward : Taking::Keep;
      t.Arg = u.Index;
      return UseKind::Take;
    case Opcode::Store:
      if (borrowingTemporary(in->Local))
        return UseKind::Borrow;
      t.K = Taking::Keep;
      return UseKind::Take;
    case Opcode::FieldStore:
    case Opcode::PtrStore:
      if (u.Index != 1)
        return UseKind::Unknown;
      t.K = Taking::Keep;
      return UseKind::Take;
    default:
      return UseKind::Unknown;
    }
  }

  /// Add to @p takings the use of @p v (but those at @p skip) that takes
  /// the parameter's own reference: the one no retain of @p v pays for.
  /// With no retain it is the only use that takes a reference, wherever it
  /// is; with some, every such use and retain must be in one block, and it
  /// is the first that takes more than the retains before it made.  False
  /// when @p v has a use the pass does not follow, or more uses that take a
  /// reference than one more than its retains, or fewer than its retains.
  bool ownTaking(ValueId v, const std::vector<Place> &skip,
                 std::vector<Taking> &takings) const {
    struct Event {
      Place At;
      size_t Index = 0;
      bool Retain = false;
      Taking T;
    };
    std::vector<Event> events;
    size_t retains = 0;
    size_t takes = 0;
    for (const Site &u : S.of(v)) {
      if (std::find(skip.begin(), skip.end(), u.At) != skip.end())
        continue;
      Taking t;
      switch (classify(u, v, t)) {
      case UseKind::Borrow:
        continue;
      case UseKind::Unknown:
        return false;
      case UseKind::Retain:
        ++retains;
        events.push_back({u.At, u.Index, true, t});
        continue;
      case UseKind::Take:
        ++takes;
        events.push_back({u.At, u.Index, false, t});
        continue;
      }
    }
    if (takes == retains)
      return true;
    if (takes != retains + 1)
      return false;
    if (retains == 0) {
      takings.push_back(events.front().T);
      return true;
    }
    const Block *b = events.front().At.B;
    if (std::any_of(events.begin(), events.end(),
                    [&](const Event &e) { return e.At.B != b; }))
      return false;
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &e) {
      return a.At.Index != e.At.Index ? a.At.Index < e.At.Index
                                      : a.Index < e.Index;
    });
    size_t held = 0;
    for (const Event &e : events) {
      if (e.Retain) {
        ++held;
      } else if (held == 0) {
        takings.push_back(e.T);
        return true;
      } else {
        --held;
      }
    }
    return false;
  }
};

/// `if %cond { in }`.
Stmt guarded(ValueId cond, Instr in) {
  If x;
  x.Cond = Operand::value(cond);
  x.Then = std::make_unique<Block>();
  x.Then->Stmts.emplace_back(std::in_place_type<Instr>, std::move(in));
  return Stmt(std::in_place_type<If>, std::move(x));
}

/// Statements to remove and to insert, applied together.
class Edits {
public:
  void remove(const Stmt &s) { Removed.insert(&s); }
  void insert(Block &b, size_t before, Stmt s) {
    Inserts[&b].emplace_back(before, std::move(s));
  }

  void apply(Block &b) {
    auto ins = Inserts.find(&b);
    std::vector<std::pair<size_t, Stmt>> none;
    std::vector<std::pair<size_t, Stmt>> &list =
        ins == Inserts.end() ? none : ins->second;
    std::stable_sort(
        list.begin(), list.end(),
        [](const auto &a, const auto &c) { return a.first < c.first; });
    std::vector<Stmt> out;
    out.reserve(b.Stmts.size() + list.size());
    size_t k = 0;
    for (size_t i = 0; i <= b.Stmts.size(); ++i) {
      for (; k < list.size() && list[k].first == i; ++k)
        out.push_back(std::move(list[k].second));
      if (i < b.Stmts.size() && !Removed.contains(&b.Stmts[i]))
        out.push_back(std::move(b.Stmts[i]));
    }
    b.Stmts = std::move(out);
    for (Stmt &s : b.Stmts) {
      if (auto *x = std::get_if<If>(&s)) {
        apply(*x->Then);
        if (x->Else)
          apply(*x->Else);
      } else if (auto *x = std::get_if<While>(&s)) {
        apply(*x->CondBlock);
        apply(*x->Body);
      } else if (auto *x = std::get_if<Switch>(&s)) {
        for (Switch::Case &c : x->Cases)
          apply(*c.Body);
        if (x->Default)
          apply(*x->Default);
      }
    }
  }

private:
  std::unordered_set<const Stmt *> Removed;
  std::unordered_map<const Block *, std::vector<std::pair<size_t, Stmt>>>
      Inserts;
};

/// How the printer writes value @p v.
std::string valueName(const Value &v) {
  return "%" + (v.Name.empty() ? std::to_string(v.Id)
                               : v.Name + "." + std::to_string(v.Id));
}

StatusOr<LendCounts> lendParametersIn(Function &f, const Callees &callees) {
  LendCounts counts;
  if (f.IsExtern || !lends(f))
    return counts;
  const std::vector<Prologue> found = prologues(f);
  const Sites sites(f);
  const ParamJudge judge(f, sites, callees);
  // Every body the lowering makes starts with its parameters' prologues
  // (docs/pir.md §3): one that takes a parameter's reference without one,
  // and without asking the parameter's bit, would release a lent argument.
  for (size_t k = 0; k < f.Params.size(); ++k) {
    if (!hasBit(f, k) || judge.asked(k) ||
        std::any_of(found.begin(), found.end(),
                    [&](const Prologue &pr) { return pr.Param == k; }))
      continue;
    if (judge.taken(k))
      return Status::error(
          "internal compiler error: @" + f.Name + " takes its box parameter " +
          valueName(f.Params[k]) +
          " as its own, but does not start with the parameter's prologue "
          "(docs/pir.md §3): lent, it would be released");
  }
  // The verdicts first, on the function as it is; then the rewrite.
  std::vector<std::pair<const Prologue *, std::vector<Taking>>> lent;
  for (const Prologue &pr : found) {
    std::vector<Taking> takings;
    if (judge.judge(pr, takings))
      lent.emplace_back(&pr, std::move(takings));
    else
      ++counts.ParamsOwnedOnEntry;
  }
  Edits edits;
  for (auto &[pr, takings] : lent) {
    bool forwards = false;
    bool keeps = false;
    for (const Taking &t : takings) {
      Stmt &s = t.At.B->Stmts[t.At.Index];
      switch (t.K) {
      case Taking::Release:
        s = guarded(pr->Own, std::move(std::get<Instr>(s)));
        break;
      case Taking::Forward: {
        auto &call = std::get<Instr>(s);
        if (call.Own.empty())
          call.Own.resize(call.Args.size());
        call.Own[t.Arg] = {ArgOwnership::Passed, Operand::value(pr->Own)};
        forwards = true;
        break;
      }
      case Taking::Keep: {
        Instr retain;
        retain.Op = Opcode::Retain;
        retain.Args = {Operand::value(t.V)};
        edits.insert(*t.At.B, t.At.Index, guarded(pr->Lent, std::move(retain)));
        keeps = true;
        ++counts.RetainsAtKeeps;
        break;
      }
      }
    }
    edits.remove(f.Body.Stmts[pr->At + 2]);
    // The `not` goes too when no keep asks it and nothing else does.
    if (!keeps && sites.of(pr->Lent).size() == 1)
      edits.remove(f.Body.Stmts[pr->At + 1]);
    ++(forwards ? counts.ParamsHandedOn : counts.ParamsLent);
  }
  edits.apply(f.Body);
  return counts;
}

StatusOr<LendCounts> lendIn(Function &f, const Callees &callees) {
  LendCounts counts = lendArgumentsIn(f, callees);
  StatusOr<LendCounts> params = lendParametersIn(f, callees);
  if (!params)
    return params.status();
  counts += *params;
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
        StatusOr<LendCounts> c = lendIn(f, callees);
        if (!c)
          return c.status();
        total += *c;
      }
    }
    stats.add(name(), "marks", total.Marks);
    stats.add(name(), "arguments lent", total.Lent);
    stats.add(name(), "copies kept for the window", total.KeptForWindow);
    stats.add(name(), "parameters lent", total.ParamsLent);
    stats.add(name(), "parameters handed on", total.ParamsHandedOn);
    stats.add(name(), "parameters owned on entry", total.ParamsOwnedOnEntry);
    stats.add(name(), "retains at keeps", total.RetainsAtKeeps);
    return Status::ok();
  }
};

} // namespace

LendCounts lendArguments(Function &f, const Module &m) {
  return lendArgumentsIn(f, Callees(m));
}

StatusOr<LendCounts> lendParameters(Function &f, const Module &m) {
  return lendParametersIn(f, Callees(m));
}

StatusOr<LendCounts> lend(Function &f, const Module &m) {
  return lendIn(f, Callees(m));
}

std::unique_ptr<Pass> createLendingPass() {
  return std::make_unique<LendingPass>();
}

} // namespace paykan::pir
