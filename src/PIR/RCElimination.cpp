// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The reference-count elimination pass (see RCElimination.h; docs/pir.md
// §12 has the rules and why they are right).
//
// The references a function makes are counted by the value they were made
// on, its *tokens*: `retain %v` makes one on %v, and a value made with one
// (a call's box, `weak.load`, `box`, a box parameter) has one.  A token ends
// at `release %v`, or where %v is handed on: a call's argument, a store
// into a field, into memory or into a local that owns what it holds, `ret`.
// A box local owns what is stored into it unless every load of it only
// borrows (a *borrowing* local: what the pass leaves where it removed a
// local's retain, whose releases went with it).
//
// Each retain is a candidate, the later and deeper first, so that a pair
// nested in another goes before the other is judged.  One walk over the
// structured body from the value's definition (Walk) follows every path in
// three phases: before the retain, in its range (live), and after the
// release that ends it.  On the way it finds those releases (the first on
// each live path), checks that the range is balanced, and keeps, for the
// live paths, the fewest other tokens on the value, the fewest tokens made
// since the retain, whether the local the value was loaded from still holds
// its reference, and whether anything that can let go of a reference came.
// A while is walked until the phases at its head stop changing.  The pass
// sweeps the function until a sweep removes nothing.

#include "paykan/pir/RCElimination.h"

#include "Names.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace paykan::pir {

namespace {

constexpr LocalId kNoLocal = ~LocalId{0};

bool sameValue(const Operand &o, ValueId v) {
  return o.isValue() && o.valueId() == v;
}

bool isNullBox(const Operand &o) {
  const auto *n = std::get_if<Operand::Null>(&o.V);
  return n && n->Ty == Type::Box;
}

/// Whether a call of @p callee can neither release a reference nor run
/// Paykan code: the runtime's string, hash, rounding, allocation, disposal
/// and panic helpers (docs/pir.md §8).  `PaykanString_equals` is not one: it
/// releases the box it is given.
bool inertCall(std::string_view callee) {
  if (!callee.starts_with(kRuntimePrefix))
    return false;
  callee.remove_prefix(kRuntimePrefix.size());
  static const std::unordered_set<std::string_view> inert = {
      names::kPaykanStringNew,        names::kPaykanStringConcat,
      names::kPaykanStringCharAt,     names::kPaykanStringAt,
      names::kPaykanStringFromInt,    names::kPaykanStringFromUint,
      names::kPaykanStringFromFloat,  names::kPaykanStringFromBool,
      names::kPaykanStringFromChar,   names::kPaykanStringParseInt,
      names::kPaykanStringParseUint,  names::kPaykanStringParseFloat,
      names::kPaykanStringParseBool,  names::kPaykanStringLength,
      names::kPaykanStringToString,   names::kPaykanStringHash,
      names::kPaykanIntHash,          names::kPaykanDispose,
      names::kPaykanMalloc,           names::kPaykanRoundF16,
      names::kPaykanPanicDivByZero,   names::kPaykanPanicDivOverflow,
      names::kPaykanPanicFloatToInt,  names::kPaykanPanicFloatToType,
      names::kPaykanPanicIntToChar,   names::kPaykanPanicIndex,
      names::kPaykanPanicForStep,
  };
  return inert.contains(callee);
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

/// A statement's place: the block that holds it and its index there.
struct Place {
  const Block *B = nullptr;
  size_t Index = 0;
};

/// How an instruction uses a box value it names.
enum class Use : uint8_t {
  Retain,  // a token more
  Release, // a token less
  HandOn,  // a token less: a call's argument, a field or memory store, `ret`
  Store,   // into a local: a token less unless the local is borrowing
  Borrow,  // unbox, cmp, weak.new: none
  Other,   // anything else (select, cast): the value is not followed
};

Use useOf(const Instr &in) {
  switch (in.Op) {
  case Opcode::Retain:
    return Use::Retain;
  case Opcode::Release:
    return Use::Release;
  case Opcode::Call:
  case Opcode::VCall:
  case Opcode::ICall:
  case Opcode::FieldStore:
  case Opcode::PtrStore:
    return Use::HandOn;
  case Opcode::Store:
    return Use::Store;
  case Opcode::Unbox:
  case Opcode::Cmp:
  case Opcode::WeakNew:
    return Use::Borrow;
  default:
    return Use::Other;
  }
}

/// What a load of a box local does to the local's reference.
enum class LoadKind : uint8_t {
  Borrow,  // nothing: what it hands on or releases it retained first
  Release, // `%t = load %x` whose one use is `release %t`: the release
  Taking,  // may release or hand on the local's reference
};

/// What one sweep knows of a function: where each statement is, where
/// each value is defined and used, and what each box local holds.
class Facts {
public:
  explicit Facts(const Function &f) : F(f) {
    size_t n = f.NextValueId;
    for (const Value &p : f.Params)
      n = std::max<size_t>(n, p.Id + 1);
    Def.resize(n);
    Ty.resize(n, Type::Void);
    Param.resize(n);
    Uses.resize(n);
    Stores.resize(f.Locals.size());
    Loads.resize(f.Locals.size());
    Addressed.resize(f.Locals.size());
    for (const Value &p : f.Params) {
      Ty[p.Id] = p.Ty;
      Param[p.Id] = true;
    }
    block(f.Body);
    Kind.resize(Def.size(), LoadKind::Taking);
    for (LocalId l = 0; l < f.Locals.size(); ++l)
      for (ValueId v : Loads[l])
        Kind[v] = loadKind(v);
    Borrowing.resize(f.Locals.size());
    Owned.resize(f.Locals.size());
    for (LocalId l = 0; l < f.Locals.size(); ++l) {
      if (f.Locals[l].Ty != Type::Box || Addressed[l])
        continue;
      Borrowing[l] = std::all_of(Loads[l].begin(), Loads[l].end(), [&](auto v) {
        return Kind[v] == LoadKind::Borrow;
      });
      Owned[l] = !Borrowing[l] &&
                 std::all_of(Stores[l].begin(), Stores[l].end(),
                             [&](const Stmt *s) { return ownedStore(*s); });
    }
  }

  const Function &F;
  /// By statement: its place, its number in the order the body is written
  /// and the last number of the statements inside it.
  std::unordered_map<const Stmt *, Place> PlaceOf;
  std::unordered_map<const Stmt *, uint32_t> Pos;
  std::unordered_map<const Stmt *, uint32_t> End;
  /// The retains, in the order the body is written.
  std::vector<const Stmt *> Retains;
  /// By value: its definition (null for a parameter), its type, and each
  /// statement that names it, once per operand.
  std::vector<const Stmt *> Def;
  std::vector<Type> Ty;
  std::vector<bool> Param;
  std::vector<std::vector<const Stmt *>> Uses;
  /// By local: the stores into it, its loads, and whether its address is
  /// taken.
  std::vector<std::vector<const Stmt *>> Stores;
  std::vector<std::vector<ValueId>> Loads;
  std::vector<bool> Addressed;
  /// By value, for a load of a box local: what it does to the local.
  std::vector<LoadKind> Kind;
  /// By box local: whether it only borrows what it holds, and whether every
  /// store into it holds a reference it owns.  The sweep updates both for
  /// a local whose retain it removed.
  std::vector<bool> Borrowing;
  std::vector<bool> Owned;

  const Instr *def(ValueId v) const {
    return Def[v] ? std::get_if<Instr>(Def[v]) : nullptr;
  }
  /// The local @p v was loaded from, or kNoLocal.
  LocalId loadedFrom(ValueId v) const {
    const Instr *in = def(v);
    return in && in->Op == Opcode::Load ? in->Local : kNoLocal;
  }
  /// Whether @p v was made with a token of its own: a user function's, a
  /// method's or a runtime function's box, `weak.load`, `box`, a box
  /// parameter (owned: the callee releases it).
  bool fresh(ValueId v) const {
    if (Ty[v] != Type::Box)
      return false;
    if (Param[v])
      return true;
    const Instr *in = def(v);
    if (!in)
      return false;
    switch (in->Op) {
    case Opcode::Call:
      return !isNativeName(in->Callee);
    case Opcode::VCall:
    case Opcode::ICall:
    case Opcode::WeakLoad:
    case Opcode::Box:
      return true;
    default:
      return false;
    }
  }
  /// The statement before @p s in its block, or null.
  const Stmt *before(const Stmt *s) const {
    const Place p = PlaceOf.at(s);
    return p.Index ? &p.B->Stmts[p.Index - 1] : nullptr;
  }

private:
  uint32_t Next = 0;

  void use(const Operand &o, const Stmt *s) {
    if (o.isValue() && o.valueId() < Uses.size())
      Uses[o.valueId()].push_back(s);
  }
  void block(const Block &b) {
    for (size_t i = 0; i < b.Stmts.size(); ++i)
      stmt(b, i);
  }
  void stmt(const Block &b, size_t i) {
    const Stmt &s = b.Stmts[i];
    PlaceOf[&s] = {&b, i};
    Pos[&s] = Next++;
    if (const auto *in = std::get_if<Instr>(&s)) {
      if (in->Result.Ty != Type::Void && in->Result.Id < Def.size()) {
        Def[in->Result.Id] = &s;
        Ty[in->Result.Id] = in->Result.Ty;
      }
      for (const Operand &o : in->Args)
        use(o, &s);
      if (in->Op == Opcode::Retain)
        Retains.push_back(&s);
      if (in->Op == Opcode::Store)
        Stores[in->Local].push_back(&s);
      if (in->Op == Opcode::Load && in->Result.Ty == Type::Box)
        Loads[in->Local].push_back(in->Result.Id);
      if (in->Op == Opcode::LocalAddr)
        Addressed[in->Local] = true;
    } else if (const auto *x = std::get_if<If>(&s)) {
      use(x->Cond, &s);
      block(*x->Then);
      if (x->Else)
        block(*x->Else);
    } else if (const auto *x = std::get_if<Switch>(&s)) {
      use(x->Value, &s);
      for (const Switch::Case &c : x->Cases)
        block(*c.Body);
      if (x->Default)
        block(*x->Default);
    } else if (const auto *x = std::get_if<While>(&s)) {
      block(*x->CondBlock);
      use(x->Cond, &s);
      block(*x->Body);
    } else if (const auto *x = std::get_if<Return>(&s)) {
      if (x->Value)
        use(*x->Value, &s);
    }
    End[&s] = Next - 1;
  }

  /// A load releases the local's reference when its one use is a release;
  /// it borrows when its uses are in its own block and, in their order, it
  /// never hands on or releases more than it retained.
  LoadKind loadKind(ValueId v) const {
    const std::vector<const Stmt *> &uses = Uses[v];
    if (uses.size() == 1)
      if (const auto *in = std::get_if<Instr>(uses.front());
          in && in->Op == Opcode::Release)
        return LoadKind::Release;
    const Place def = PlaceOf.at(Def[v]);
    std::vector<std::pair<size_t, Use>> events;
    for (const Stmt *s : uses) {
      const Place at = PlaceOf.at(s);
      const auto *in = std::get_if<Instr>(s);
      if (at.B != def.B || !in)
        return LoadKind::Taking;
      events.emplace_back(at.Index, useOf(*in));
    }
    std::sort(events.begin(), events.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    int held = 0;
    for (const auto &[at, u] : events) {
      if (u == Use::Other)
        return LoadKind::Taking;
      if (u == Use::Retain)
        ++held;
      else if (u != Use::Borrow && --held < 0)
        return LoadKind::Taking;
    }
    return LoadKind::Borrow;
  }

  /// Whether the value @p s stores holds a reference of its own: null, a
  /// fresh value, or one the statement before retained.
  bool ownedStore(const Stmt &s) const {
    const Operand &o = std::get<Instr>(s).Args[0];
    if (isNullBox(o))
      return true;
    if (!o.isValue())
      return false;
    if (fresh(o.valueId()))
      return true;
    const Stmt *prev = before(&s);
    const auto *in = prev ? std::get_if<Instr>(prev) : nullptr;
    return in && in->Op == Opcode::Retain && sameValue(in->Args[0], o.valueId());
  }
};

/// One retain and what its range is judged by.
struct Candidate {
  const Stmt *R = nullptr;
  /// The value retained.
  ValueId V = kNoValue;
  /// For a local's retain: the store right after it, and the local, whose
  /// releases end the range.
  const Stmt *Into = nullptr;
  LocalId L = kNoLocal;
  /// The local V was loaded from, when it may keep the object alive.
  LocalId M = kNoLocal;
  /// Where the walk starts: the block that defines V and the statement
  /// after the definition; and the last statement of that block it walks.
  const Block *B = nullptr;
  size_t From = 0;
  size_t To = 0;
};

/// The walk for one candidate (see the top of the file).
class Walk {
public:
  Walk(const Facts &facts, const std::unordered_set<const Stmt *> &removed,
       const Candidate &c)
      : F(facts), Removed(removed), C(c), K2(c.M != kNoLocal) {}

  /// Whether the candidate's retain and the releases in Ends can go.
  bool run() {
    State s;
    s.P[kBefore].Reached = true;
    s.P[kBefore].N = F.fresh(C.V) ? 1 : 0;
    s = block(*C.B, C.From, C.To, s);
    if (s.P[kLive].Reached)
      Failed = true;
    return !Failed && (K1 || K2 || K3);
  }

  /// The releases that end the range.
  std::vector<const Stmt *> Ends;

private:
  // A count below kFloor fails every keeper and one above kCeiling enables
  // nothing more, so both are clamped: the walk of a loop then ends.
  static constexpr int kFloor = -1;
  static constexpr int kCeiling = 4;
  static constexpr int kMaxRounds = 32;

  enum : size_t { kBefore, kLive, kAfter };

  /// The paths in one phase at a point.  N: the fewest tokens on V, R's
  /// not counted.  D (live): the fewest made since R, less those ended.
  /// Intact: whether M holds its reference on every one.
  struct Phase {
    bool Reached = false;
    int N = 0;
    int D = 0;
    bool Intact = true;

    bool operator==(const Phase &) const = default;
  };
  struct State {
    std::array<Phase, 3> P;

    bool operator==(const State &) const = default;
  };
  /// The paths that leave the while loops being walked.
  struct Loop {
    State Breaks;
    State Continues;
  };

  const Facts &F;
  const std::unordered_set<const Stmt *> &Removed;
  const Candidate &C;
  std::vector<Loop> Loops;
  bool Failed = false;
  // Which keeper still holds: the enclosing reference, the local, nothing
  // that can let go.
  bool K1 = true;
  bool K2;
  bool K3 = true;

  static Phase merge(const Phase &a, const Phase &b) {
    if (!a.Reached)
      return b;
    if (!b.Reached)
      return a;
    return {true, std::min(a.N, b.N), std::min(a.D, b.D),
            a.Intact && b.Intact};
  }
  static State merge(const State &a, const State &b) {
    State s;
    for (size_t i = 0; i < 3; ++i)
      s.P[i] = merge(a.P[i], b.P[i]);
    return s;
  }

  static int clamp(int n) { return std::clamp(n, kFloor, kCeiling); }

  /// A token more or less on V.
  void count(State &s, int delta) {
    for (Phase &p : s.P)
      if (p.Reached)
        p.N = clamp(p.N + delta);
    if (s.P[kLive].Reached)
      s.P[kLive].D = clamp(s.P[kLive].D + delta);
    check(s);
  }

  /// The keepers that fail where the live paths are now.
  void check(const State &s) {
    const Phase &live = s.P[kLive];
    if (!live.Reached)
      return;
    K1 = K1 && live.N >= 1;
    K2 = K2 && live.N >= 0 && live.Intact;
    K3 = K3 && live.D >= 0;
  }

  /// Something that may release or hand on M's reference.
  void keeperTouched(State &s) {
    for (Phase &p : s.P)
      if (p.Reached)
        p.Intact = false;
    check(s);
  }

  /// The retain: every path through it is live.
  State enter(State s) {
    if (s.P[kLive].Reached) {
      // Reached again before the range ended: a loop.
      Failed = true;
      return s;
    }
    State out;
    out.P[kLive] = merge(s.P[kBefore], s.P[kAfter]);
    out.P[kLive].D = 0;
    check(out);
    return out;
  }

  /// A release that ends the range on the live paths.
  State end(const Stmt &st, State s) {
    if (s.P[kBefore].Reached || s.P[kAfter].Reached) {
      // It would go from a path on which it has a reference to release.
      Failed = true;
      return s;
    }
    // A loop's body is walked more than once.
    if (std::find(Ends.begin(), Ends.end(), &st) == Ends.end())
      Ends.push_back(&st);
    s.P[kAfter] = s.P[kLive];
    s.P[kLive] = Phase();
    return s;
  }

  /// Whether @p o is a box: a box value or a null one.
  bool isBox(const Operand &o) const {
    return isNullBox(o) || (o.isValue() && o.valueId() < F.Ty.size() &&
                            F.Ty[o.valueId()] == Type::Box);
  }

  bool isLoadOf(const Operand &o, LocalId l) const {
    return o.isValue() && o.valueId() < F.Def.size() &&
           F.loadedFrom(o.valueId()) == l;
  }

  /// Whether @p o is a load of M (not V) that may release or hand on M's
  /// reference.
  bool takesFromKeeper(const Operand &o) const {
    return C.M != kNoLocal && isLoadOf(o, C.M) && !sameValue(o, C.V) &&
           F.Kind[o.valueId()] != LoadKind::Borrow;
  }

  /// Whether @p in, which is neither a release of V nor one that ends the
  /// range, may let go of a reference or overwrite one.
  bool letsGo(const Instr &in) const {
    switch (in.Op) {
    case Opcode::Release:
    case Opcode::Free:
    case Opcode::VCall:
    case Opcode::ICall:
    case Opcode::MemCopy:
    case Opcode::MemZero:
      return true;
    case Opcode::Call:
      return !inertCall(in.Callee);
    case Opcode::FieldStore:
    case Opcode::PtrStore:
      return isBox(in.Args[1]);
    default:
      return false;
    }
  }

  State instr(const Stmt &st, const Instr &in, State s) {
    if (&st == C.R)
      return enter(s);
    if (&st == C.Into)
      return s;
    if (in.Op == Opcode::Release) {
      if (C.L != kNoLocal && isLoadOf(in.Args[0], C.L)) {
        // The local holds nothing to release off the range.
        if (!s.P[kLive].Reached)
          Failed = true;
        return end(st, s);
      }
      if (sameValue(in.Args[0], C.V)) {
        if (C.L == kNoLocal && s.P[kLive].Reached)
          return end(st, s);
        count(s, -1);
        return s;
      }
    }
    for (const Operand &o : in.Args) {
      if (!sameValue(o, C.V))
        continue;
      switch (useOf(in)) {
      case Use::Retain:
        count(s, 1);
        break;
      case Use::Store:
        if (!F.Borrowing[in.Local])
          count(s, -1);
        break;
      case Use::HandOn:
        count(s, -1);
        break;
      default:
        break;
      }
    }
    if (s.P[kLive].Reached && letsGo(in))
      K3 = false;
    if (C.M != kNoLocal) {
      const bool touches =
          (in.Op == Opcode::Store && in.Local == C.M) ||
          ((useOf(in) == Use::Release || useOf(in) == Use::HandOn ||
            useOf(in) == Use::Store) &&
           std::any_of(in.Args.begin(), in.Args.end(),
                       [&](const Operand &o) { return takesFromKeeper(o); }));
      if (touches)
        keeperTouched(s);
    }
    return s;
  }

  State block(const Block &b, size_t from, size_t to, State s) {
    for (size_t i = from; i <= to && i < b.Stmts.size(); ++i)
      s = stmt(b.Stmts[i], s);
    return s;
  }
  State block(const Block &b, State s) {
    return b.Stmts.empty() ? s : block(b, 0, b.Stmts.size() - 1, s);
  }

  State loop(const While &w, const State &entry) {
    State head = entry;
    for (int round = 0; round < kMaxRounds; ++round) {
      Loops.emplace_back();
      const State cond = block(*w.CondBlock, head);
      const State body = block(*w.Body, cond);
      const Loop out = Loops.back();
      Loops.pop_back();
      const State next = merge(entry, merge(body, out.Continues));
      if (next == head)
        return merge(cond, out.Breaks);
      head = next;
    }
    Failed = true;
    return {};
  }

  /// A path leaving the walk with @p s: it must not be live.
  void leave(const State &s) {
    if (s.P[kLive].Reached)
      Failed = true;
  }

  State stmt(const Stmt &st, const State &s) {
    if (Removed.contains(&st))
      return s;
    if (const auto *in = std::get_if<Instr>(&st))
      return instr(st, *in, s);
    if (const auto *x = std::get_if<If>(&st)) {
      const State t = block(*x->Then, s);
      return merge(t, x->Else ? block(*x->Else, s) : s);
    }
    if (const auto *x = std::get_if<Switch>(&st)) {
      State out = x->Default ? block(*x->Default, s)
                  : exhaustive(*x) ? State()
                                   : s;
      for (const Switch::Case &c : x->Cases)
        out = merge(out, block(*c.Body, s));
      return out;
    }
    if (const auto *x = std::get_if<While>(&st))
      return loop(*x, s);
    if (const auto *x = std::get_if<Return>(&st)) {
      State r = s;
      if (x->Value && sameValue(*x->Value, C.V))
        count(r, -1);
      if (x->Value && takesFromKeeper(*x->Value))
        keeperTouched(r);
      leave(r);
      return {};
    }
    if (std::holds_alternative<Break>(st) ||
        std::holds_alternative<Continue>(st)) {
      if (Loops.empty()) {
        leave(s);
      } else {
        Loop &l = Loops.back();
        State &to =
            std::holds_alternative<Break>(st) ? l.Breaks : l.Continues;
        to = merge(to, s);
      }
      return {};
    }
    // `unreachable`: the path ends; a range open on it is never closed.
    return {};
  }
};

/// One sweep of the pass over a function.
class Sweep {
public:
  explicit Sweep(Function &f) : Fn(f), F(f) {}

  /// Remove what can go; false when nothing could.
  bool run(RCCounts &counts) {
    for (auto it = F.Retains.rbegin(); it != F.Retains.rend(); ++it)
      if (!Removed.contains(*it))
        candidate(*it, counts);
    if (Removed.empty())
      return false;
    erase(Fn.Body);
    return true;
  }

private:
  Function &Fn;
  Facts F;
  std::unordered_set<const Stmt *> Removed;

  void erase(Block &b) {
    std::erase_if(b.Stmts, [&](const Stmt &s) { return Removed.contains(&s); });
    for (Stmt &s : b.Stmts) {
      if (auto *x = std::get_if<If>(&s)) {
        erase(*x->Then);
        if (x->Else)
          erase(*x->Else);
      } else if (auto *x = std::get_if<Switch>(&s)) {
        for (Switch::Case &c : x->Cases)
          erase(*c.Body);
        if (x->Default)
          erase(*x->Default);
      } else if (auto *x = std::get_if<While>(&s)) {
        erase(*x->CondBlock);
        erase(*x->Body);
      }
    }
  }

  /// Whether every use of @p v is one the walk follows.
  bool followed(ValueId v) const {
    return std::all_of(F.Uses[v].begin(), F.Uses[v].end(),
                       [&](const Stmt *s) {
                         if (const auto *in = std::get_if<Instr>(s))
                           return useOf(*in) != Use::Other;
                         return std::holds_alternative<Return>(*s);
                       });
  }

  /// The statement after @p s in its block that is still there, or null.
  const Stmt *after(const Stmt *s) const {
    const Place p = F.PlaceOf.at(s);
    for (size_t i = p.Index + 1; i < p.B->Stmts.size(); ++i)
      if (!Removed.contains(&p.B->Stmts[i]))
        return &p.B->Stmts[i];
    return nullptr;
  }

  /// Whether local @p l, stored only by @p store, has releases that can
  /// end a range: never addressed, and each of its loads releasing it or
  /// only borrowing.
  bool rangeLocal(LocalId l, const Stmt *store) const {
    if (Fn.Locals[l].Ty != Type::Box || F.Addressed[l] ||
        F.Stores[l].size() != 1 || F.Stores[l].front() != store)
      return false;
    bool releases = false;
    for (ValueId v : F.Loads[l]) {
      if (F.Kind[v] == LoadKind::Taking)
        return false;
      releases = releases || (F.Kind[v] == LoadKind::Release &&
                              !Removed.contains(F.Def[v]));
    }
    return releases;
  }

  /// The top-level statement of @p b, at or after @p from, that holds the
  /// statement numbered @p pos.
  size_t holding(const Block &b, size_t from, uint32_t pos) const {
    size_t lo = from, hi = b.Stmts.size() - 1;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (F.End.at(&b.Stmts[mid]) < pos)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  /// Where the walk for @p c runs: from V's definition to the statement of
  /// its block that holds the last use of V, or of the local and its
  /// loads; false when one of those is not after the retain inside it.
  bool region(Candidate &c) const {
    const Stmt *def = F.Def[c.V];
    if (def) {
      const Place p = F.PlaceOf.at(def);
      c.B = p.B;
      c.From = p.Index + 1;
    } else {
      c.B = &Fn.Body;
      c.From = 0;
    }
    if (c.From >= c.B->Stmts.size())
      return false;
    const uint32_t first = F.Pos.at(&c.B->Stmts[c.From]);
    const uint32_t last = F.End.at(&c.B->Stmts.back());
    const uint32_t r = F.Pos.at(c.R);
    uint32_t end = r;
    auto within = [&](const Stmt *s, uint32_t from) {
      const uint32_t at = F.Pos.at(s);
      end = std::max(end, at);
      return from <= at && at <= last;
    };
    for (const Stmt *s : F.Uses[c.V])
      if (!within(s, first))
        return false;
    if (c.L != kNoLocal)
      for (ValueId v : F.Loads[c.L]) {
        if (!within(F.Def[v], r))
          return false;
        for (const Stmt *s : F.Uses[v])
          if (!within(s, r))
            return false;
      }
    c.To = holding(*c.B, c.From, end);
    return true;
  }

  bool tryRemove(Candidate c, RCCounts &counts) {
    if (!region(c))
      return false;
    Walk walk(F, Removed, c);
    if (!walk.run())
      return false;
    Removed.insert(c.R);
    for (const Stmt *s : walk.Ends) {
      Removed.insert(s);
      // A local's release goes with its load.
      if (c.L != kNoLocal)
        Removed.insert(F.Def[std::get<Instr>(*s).Args[0].valueId()]);
    }
    if (c.L != kNoLocal) {
      F.Borrowing[c.L] = true;
      F.Owned[c.L] = false;
    }
    ++counts.Retains;
    counts.Releases += walk.Ends.size();
    return true;
  }

  void candidate(const Stmt *r, RCCounts &counts) {
    const Instr &in = std::get<Instr>(*r);
    if (!in.Args[0].isValue())
      return;
    const ValueId v = in.Args[0].valueId();
    if (v >= F.Ty.size() || F.Ty[v] != Type::Box || !followed(v))
      return;
    Candidate c;
    c.R = r;
    c.V = v;
    // A retain stored right away into a local: the local's releases end
    // its range; failing that, the value's.
    if (const Stmt *next = after(r)) {
      const auto *store = std::get_if<Instr>(next);
      if (store && store->Op == Opcode::Store &&
          sameValue(store->Args[0], v) && rangeLocal(store->Local, next)) {
        Candidate local = c;
        local.Into = next;
        local.L = store->Local;
        local.M = keeper(v, local.L);
        if (tryRemove(local, counts))
          return;
      }
    }
    c.M = keeper(v, kNoLocal);
    tryRemove(c, counts);
  }

  /// The local @p v was loaded from when it may keep the object alive:
  /// one whose every store holds a reference it owns.
  LocalId keeper(ValueId v, LocalId l) const {
    const LocalId m = F.loadedFrom(v);
    return m != kNoLocal && m != l && F.Owned[m] ? m : kNoLocal;
  }
};

class RCEliminationPass : public Pass {
public:
  std::string_view name() const override { return "rc-elimination"; }
  Status run(Program &program, Statistics &stats) override {
    RCCounts total;
    for (Module &m : program.Modules)
      for (Function &f : m.Functions) {
        const RCCounts c = eliminateRedundantRC(f);
        total.Retains += c.Retains;
        total.Releases += c.Releases;
      }
    stats.add(name(), "retains removed", total.Retains);
    stats.add(name(), "releases removed", total.Releases);
    return Status::ok();
  }
};

} // namespace

RCCounts eliminateRedundantRC(Function &f) {
  RCCounts counts;
  if (f.IsExtern)
    return counts;
  while (Sweep(f).run(counts)) {
  }
  return counts;
}

std::unique_ptr<Pass> createRCEliminationPass() {
  return std::make_unique<RCEliminationPass>();
}

} // namespace paykan::pir
