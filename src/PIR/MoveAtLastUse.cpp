// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The moves-at-last-use pass (see MoveAtLastUse.h; docs/pir.md §12 has the
// correctness argument).
//
// A function's statements are first numbered (Graph): a node per
// instruction, per test (an if's or a switch's value, a while's condition),
// per `ret` and per `unreachable`, and one for the end of the body.  With
// them come, for every value, where it is defined and which nodes use it
// (Values), and what it may refer to without a reference of its own
// (Origins): a box loaded from a local, the object unboxed from that box, a
// field of the object, an address into a `mem` local and what is read
// through it are all aliases of that local.
//
// A copy is moved when every path from it reaches the release of the
// slot it copied (a box local, or a reference held in a `mem` local at a
// constant offset) before anything else touches the slot or uses an alias
// of it, or ends in `unreachable`; and when, from where the destination
// holds the reference to that release, no node that may release a
// reference comes while a borrowed reference (Origins::borrowed: one no
// local of the function holds the object of) is in use across it
// (Analysis::exposed); a release the moves leave releasing nothing is no
// such node, as long as every move it counts on is made.  Moving removes
// the copy's retains and stores null into the slot; the cleanup that
// follows removes what that made useless.  Each of these questions is one
// dataflow over the structured body (Flow), a bit per slot, so a round of
// the pass takes time in proportion to the function's size times the depth
// of its loops, not to the number of copies times its size.  Rounds repeat
// while one moves something; a later one finds more only where a cleanup
// moved a release into an arm.

#include "paykan/pir/MoveAtLastUse.h"

#include "Names.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace paykan::pir {

namespace {

constexpr LocalId kNoLocal = ~LocalId{0};

/// The types of the values that point at something: what an alias is.
bool refersTo(Type t) {
  return t == Type::Box || t == Type::Obj || t == Type::Ptr || t == Type::Weak;
}

std::optional<int64_t> constantI64(const Operand &o) {
  if (const auto *v = std::get_if<int64_t>(&o.V))
    return *v;
  return std::nullopt;
}

bool isNullBox(const Operand &o) {
  const auto *n = std::get_if<Operand::Null>(&o.V);
  return n && n->Ty == Type::Box;
}

bool sameValue(const Operand &o, ValueId v) {
  return o.isValue() && o.valueId() == v;
}

/// Whether the bytes [at, at + size) hold part of the reference at offset
/// @p ref.
bool overlaps(int64_t at, int64_t size, int64_t ref) {
  return at < ref + 8 && ref < at + size;
}

/// Whether the bytes [at, at + size) hold all of the reference at @p ref.
bool covers(int64_t at, int64_t size, int64_t ref) {
  return at <= ref && ref + 8 <= at + size;
}

/// A closure's class (docs/pir.md §7): its fields are captures.
bool isClosureClass(const std::string &name) {
  return name.starts_with(names::kClosureClassPrefix);
}

/// Whether a call of @p callee may release a reference, or run Paykan code
/// that may: any callee but the runtime's string, hash, rounding,
/// allocation, disposal and panic helpers (docs/pir.md §8), which do
/// neither.
bool callMayRelease(std::string_view callee) {
  if (!callee.starts_with(kRuntimePrefix))
    return true;
  callee.remove_prefix(kRuntimePrefix.size());
  static const std::unordered_set<std::string_view> inert = {
      names::kPaykanStringNew,        names::kPaykanStringConcat,
      names::kPaykanStringCharAt,     names::kPaykanStringAt,
      names::kPaykanStringFromInt,    names::kPaykanStringFromUint,
      names::kPaykanStringFromFloat,  names::kPaykanStringFromBool,
      names::kPaykanStringFromChar,   names::kPaykanStringParseInt,
      names::kPaykanStringParseUint,  names::kPaykanStringParseFloat,
      names::kPaykanStringParseBool,  names::kPaykanStringParseF32,
      names::kPaykanStringParseF16,   names::kPaykanStringEquals,
      names::kPaykanStringLength,     names::kPaykanStringToString,
      names::kPaykanStringHash,       names::kPaykanIntHash,
      names::kPaykanDispose,          names::kPaykanMalloc,
      names::kPaykanRoundF16,         names::kPaykanIntToF32,
      names::kPaykanUintToF32,        names::kPaykanPanicDivByZero,
      names::kPaykanPanicDivOverflow, names::kPaykanPanicFloatToInt,
      names::kPaykanPanicFloatToType, names::kPaykanPanicIntToChar,
      names::kPaykanPanicToChar,      names::kPaykanPanicIndex,
      names::kPaykanPanicForStep,
  };
  return !inert.contains(callee);
}

/// Whether @p in may release a reference: a `release`, a `free`, or a call
/// that may (callMayRelease).  An object can only die at one of these.
bool mayRelease(const Instr &in) {
  switch (in.Op) {
  case Opcode::Release:
  case Opcode::Free:
  case Opcode::VCall:
  case Opcode::ICall:
    return true;
  case Opcode::Call:
    return callMayRelease(in.Callee);
  default:
    return false;
  }
}

bool isCall(Opcode op) {
  return op == Opcode::Call || op == Opcode::VCall || op == Opcode::ICall;
}

/// Whether no value reaches a switch's absent default: its cases are a
/// `bool`'s `true` and `false`.  Otherwise an absent default is the path on
/// which no case matches.
bool exhaustive(const Switch &sw) {
  bool seen[2] = {false, false};
  for (const Switch::Case &c : sw.Cases)
    if (const auto *b = std::get_if<bool>(&c.Value.V))
      seen[*b ? 1 : 0] = true;
  return seen[0] && seen[1];
}

/// A statement's place: the block that holds it and its index there.
struct Place {
  Block *B = nullptr;
  size_t Index = 0;

  Stmt &stmt() const { return B->Stmts[Index]; }
  const Instr *instr() const { return std::get_if<Instr>(&stmt()); }
};

struct Node {
  enum Kind : uint8_t {
    Instruction, // an instruction statement
    Test,        // an if's or a switch's value, a while's condition
    Return,      // `ret`: the function ends
    Dead,        // `unreachable`: no path goes on
    Exit,        // the end of the function's body
  };
  Kind K = Exit;
  /// The statement (Instruction, Test, Return, Dead).
  Place At;
  /// The operand a Test or a Return uses (none for a `ret` of nothing).
  const Operand *Use = nullptr;
};

constexpr uint32_t kNoLoop = ~uint32_t{0};

/// A function body's nodes, numbered in the reverse of the order its
/// statements run in: a block from its end, an if's arms before its test.
/// The flows (Flow) walk the structured body and step through them.
class Graph {
public:
  std::vector<Node> Nodes;
  /// The end of the function's body.
  uint32_t End = 0;
  /// The while loops: the nodes of each, which are numbered together (its
  /// condition, test and body), and the loop around it.
  struct Loop {
    uint32_t First = 0;
    uint32_t Last = 0;
    uint32_t Parent = kNoLoop;
  };
  std::vector<Loop> Loops;
  /// By node: the innermost loop it is in, or kNoLoop.
  std::vector<uint32_t> LoopOf;

  explicit Graph(Function &f) {
    End = add(Node::Exit);
    block(f.Body);
  }

  const Instr *instr(uint32_t n) const {
    return Nodes[n].K == Node::Instruction ? Nodes[n].At.instr() : nullptr;
  }
  /// The node of an instruction, a `ret` or an `unreachable`, or the test
  /// of an if, a switch or a while.
  uint32_t of(const Stmt &s) const { return NodeOf.at(&s); }
  /// Node @p n's place in the order the body is written: a block's
  /// statements from its first, an if's or a switch's test before its
  /// arms, a while's condition before its body.  Each arm and each loop is
  /// a run of places.
  uint32_t pos(uint32_t n) const {
    return static_cast<uint32_t>(Nodes.size() - 1 - n);
  }
  bool inLoop(uint32_t n, uint32_t loop) const {
    return Loops[loop].First <= n && n <= Loops[loop].Last;
  }
  /// The last place in loop @p loop.
  uint32_t loopEnd(uint32_t loop) const { return pos(Loops[loop].First); }

private:
  std::unordered_map<const Stmt *, uint32_t> NodeOf;
  /// The loops around the statement numbered, innermost last.
  std::vector<uint32_t> Open;

  uint32_t add(Node::Kind k, Place at = {}, const Operand *use = nullptr) {
    Node n;
    n.K = k;
    n.At = at;
    n.Use = use;
    Nodes.push_back(n);
    LoopOf.push_back(Open.empty() ? kNoLoop : Open.back());
    return static_cast<uint32_t>(Nodes.size() - 1);
  }

  void block(Block &b) {
    for (size_t i = b.Stmts.size(); i-- > 0;)
      stmt(b, i);
  }

  void stmt(Block &b, size_t i) {
    Stmt &s = b.Stmts[i];
    const Place at{&b, i};
    if (std::holds_alternative<Instr>(s)) {
      NodeOf[&s] = add(Node::Instruction, at);
    } else if (auto *x = std::get_if<If>(&s)) {
      block(*x->Then);
      if (x->Else)
        block(*x->Else);
      NodeOf[&s] = add(Node::Test, at, &x->Cond);
    } else if (auto *x = std::get_if<Switch>(&s)) {
      for (Switch::Case &c : x->Cases)
        block(*c.Body);
      if (x->Default)
        block(*x->Default);
      NodeOf[&s] = add(Node::Test, at, &x->Value);
    } else if (auto *x = std::get_if<While>(&s)) {
      const auto loop = static_cast<uint32_t>(Loops.size());
      Loops.push_back({static_cast<uint32_t>(Nodes.size()), 0,
                       Open.empty() ? kNoLoop : Open.back()});
      Open.push_back(loop);
      block(*x->Body);
      NodeOf[&s] = add(Node::Test, at, &x->Cond);
      block(*x->CondBlock);
      Open.pop_back();
      Loops[loop].Last = static_cast<uint32_t>(Nodes.size() - 1);
    } else if (auto *x = std::get_if<Return>(&s)) {
      NodeOf[&s] = add(Node::Return, at, x->Value ? &*x->Value : nullptr);
    } else if (std::holds_alternative<Unreachable>(s)) {
      NodeOf[&s] = add(Node::Dead, at);
    }
  }
};

/// Where each value of a function is defined, its type, and the nodes that
/// use it (a node once per operand that names it).
class Values {
public:
  Values(const Function &f, const Graph &g) {
    size_t n = f.NextValueId;
    for (const Value &p : f.Params)
      n = std::max<size_t>(n, p.Id + 1);
    for (const Node &node : g.Nodes)
      if (node.K == Node::Instruction)
        n = std::max<size_t>(n, node.At.instr()->Result.Id + 1);
    Defs.resize(n);
    Types.resize(n, Type::Void);
    Users.resize(n);
    for (const Value &p : f.Params)
      Types[p.Id] = p.Ty;
    for (uint32_t i = 0; i < g.Nodes.size(); ++i) {
      const Node &node = g.Nodes[i];
      if (node.K != Node::Instruction) {
        if (node.Use)
          use(*node.Use, i);
        continue;
      }
      const Instr &in = *node.At.instr();
      if (in.Result.Ty != Type::Void) {
        Defs[in.Result.Id] = node.At;
        Types[in.Result.Id] = in.Result.Ty;
      }
      for (const Operand &o : in.Args)
        use(o, i);
      for (const ArgOwnership &o : in.Own)
        if (o.K == ArgOwnership::Passed)
          use(o.Bit, i);
    }
  }

  /// One more than the largest value id.
  size_t size() const { return Types.size(); }
  /// The instruction defining @p v (null for a parameter).
  const Instr *def(ValueId v) const {
    return Defs[v].B ? Defs[v].instr() : nullptr;
  }
  Place defAt(ValueId v) const { return Defs[v]; }
  const std::vector<uint32_t> &users(ValueId v) const { return Users[v]; }

  Type type(const Operand &o) const {
    struct Of {
      const Values &V;
      Type operator()(ValueId v) const { return V.Types[v]; }
      Type operator()(int64_t) const { return Type::I64; }
      Type operator()(double) const { return Type::F64; }
      Type operator()(bool) const { return Type::Bool; }
      Type operator()(char) const { return Type::Char; }
      Type operator()(const Operand::Null &n) const { return n.Ty; }
      Type operator()(const SymbolRef &) const { return Type::Ptr; }
    };
    return std::visit(Of{*this}, o.V);
  }

private:
  std::vector<Place> Defs;
  std::vector<Type> Types;
  std::vector<std::vector<uint32_t>> Users;

  void use(const Operand &o, uint32_t node) {
    if (o.isValue() && o.valueId() < Users.size())
      Users[o.valueId()].push_back(node);
  }
};

/// An address into a local's storage: the local, and the byte offset when
/// it is a constant.
struct Address {
  LocalId L = kNoLocal;
  std::optional<int64_t> Offset;
};

/// What the values and locals of a function may refer to without holding a
/// reference of their own, which of them are borrowed, and which locals
/// cannot be moved from.
class Origins {
public:
  Origins(const Function &f, const Graph &g, const Values &v)
      : F(f), G(g), V(v), From(v.size()), Addr(v.size()), Held(f.Locals.size()),
        AddressTaken(f.Locals.size()), Escaped(f.Locals.size()),
        Borrowed(v.size()), HoldsBorrowed(f.Locals.size()) {
    // `self` and an `inout` parameter point at what the caller holds, which
    // the caller may let go of during the call.  An `__init__`'s `self` is
    // the object or the storage its constructor made, the only caller it
    // can have (docs/language/05-classes.md), which holds it until the
    // call returns.
    const bool init = f.Name.ends_with(std::string(".") + names::kMethodInit);
    for (size_t i = 0; i < f.Params.size(); ++i)
      if (const Value &p = f.Params[i];
          (p.Ty == Type::Obj || p.Ty == Type::Ptr) && !(init && i == 0))
        Borrowed[p.Id] = true;
    // What a local holds depends on stores that may come later (a loop):
    // until nothing changes.
    for (bool changed = true; changed;) {
      changed = false;
      walk(f.Body, changed);
    }
  }

  /// The locals @p o may refer to, or into the storage of, sorted.
  const std::vector<LocalId> &locals(const Operand &o) const {
    static const std::vector<LocalId> none;
    return o.isValue() && o.valueId() < From.size() ? From[o.valueId()] : none;
  }
  /// Whether @p o may refer to local @p l, or into its storage.
  bool from(const Operand &o, LocalId l) const {
    const std::vector<LocalId> &ls = locals(o);
    return std::binary_search(ls.begin(), ls.end(), l);
  }
  Address address(const Operand &o) const {
    return o.isValue() && o.valueId() < Addr.size() ? Addr[o.valueId()]
                                                    : Address{};
  }
  /// Whether @p o is an address into local @p l at a constant offset, set
  /// in @p off.
  bool offsetIn(const Operand &o, LocalId l, int64_t &off) const {
    const Address a = address(o);
    if (a.L != l || !a.Offset)
      return false;
    off = *a.Offset;
    return true;
  }
  /// Whether some user of @p o retains it: the reference it is stored as
  /// is then one of its own.
  bool retained(const Operand &o) const {
    if (!o.isValue())
      return false;
    for (uint32_t u : V.users(o.valueId()))
      if (const Instr *in = G.instr(u); in && in->Op == Opcode::Retain)
        return true;
    return false;
  }

  /// A box local is moved from only when its slot is never addressed and
  /// no alias of it is stored away; a mem local, when no alias of it is
  /// stored away; neither when it is a `view`'s, which names what another
  /// variable holds.
  bool movable(LocalId l) const {
    if (Escaped[l] || F.Locals[l].View)
      return false;
    return F.Locals[l].Ty == Type::Mem ||
           (F.Locals[l].Ty == Type::Box && !AddressTaken[l]);
  }

  /// Whether value @p v is a *borrowed* reference: one that may refer to
  /// an object, or into one, that no local of the function holds and that
  /// it holds no reference to of its own.  A field's or an element's value
  /// read in place, what is reached through `self` or an `inout`
  /// parameter, and what is computed from one are; what is reached
  /// through a box or a mem local, a call's box, a retained copy and a
  /// stable load's box (its slot keeps it while it is used) are not.  A
  /// local keeps what it holds until a store into it, or a call given its
  /// address, whose statement's other reads of it the lowering gives
  /// references of their own (docs/pir.md §7).
  bool borrowed(ValueId v) const { return v < Borrowed.size() && Borrowed[v]; }
  bool borrowed(const Operand &o) const {
    return o.isValue() && borrowed(o.valueId());
  }
  /// Whether local @p l may hold a borrowed reference (`self`, say).
  bool holdsBorrowed(LocalId l) const { return HoldsBorrowed[l]; }

private:
  const Function &F;
  const Graph &G;
  const Values &V;
  /// By value: the locals it may refer to, sorted.
  std::vector<std::vector<LocalId>> From;
  /// By value: the address into a local it is, if it is one.
  std::vector<Address> Addr;
  /// By local: the locals what it holds may refer to (an alias stored in
  /// it).
  std::vector<std::vector<LocalId>> Held;
  std::vector<bool> AddressTaken;
  std::vector<bool> Escaped;
  std::vector<bool> Borrowed;
  std::vector<bool> HoldsBorrowed;

  /// Whether box %v is retained before anything else uses it, and nothing
  /// between its definition and that retain may release a reference: it
  /// holds a reference of its own wherever it is used.
  bool retainedFirst(ValueId v) const {
    const Place def = V.defAt(v);
    if (!def.B)
      return false;
    // The first retain in the block: nodes are numbered backward, so the
    // largest, and a use after it has a smaller number.
    const std::vector<uint32_t> &users = V.users(v);
    std::optional<uint32_t> r;
    for (uint32_t u : users)
      if (const Instr *in = G.instr(u);
          in && in->Op == Opcode::Retain && G.Nodes[u].At.B == def.B)
        r = std::max(r.value_or(u), u);
    if (!r || std::any_of(users.begin(), users.end(),
                          [&](uint32_t u) { return u > *r; }))
      return false;
    for (size_t i = def.Index + 1; i < G.Nodes[*r].At.Index; ++i) {
      const auto *in = std::get_if<Instr>(&def.B->Stmts[i]);
      if (!in || mayRelease(*in))
        return false;
    }
    return true;
  }
  /// Whether what @p in computes from its operands is borrowed: when one
  /// of them is.
  bool anyBorrowed(const Instr &in) const {
    return std::any_of(in.Args.begin(), in.Args.end(),
                       [&](const Operand &o) { return borrowed(o); });
  }
  /// Whether @p in's result is borrowed.
  bool borrowedResult(const Instr &in) const {
    const Type t = in.Result.Ty;
    if (t != Type::Box && t != Type::Obj && t != Type::Ptr && t != Type::I64)
      return false;
    const bool ref = t != Type::I64;
    switch (in.Op) {
    case Opcode::Load:
      return HoldsBorrowed[in.Local];
    case Opcode::LocalAddr:
      return false;
    case Opcode::FieldLoad:
      // A field's value, read in place, unless its slot keeps the reference
      // while the value is used (a stable load, docs/pir.md §6).
      return ref && !in.Stable && !retainedFirst(in.Result.Id);
    case Opcode::PtrLoad:
      // Memory a mem local owns holds references of its own (docs/pir.md
      // §7); any other is a field's, an element's or the caller's, which a
      // stable load's slot keeps.
      return ref && !in.Stable && !retainedFirst(in.Result.Id) &&
             (borrowed(in.Args[0]) || address(in.Args[0]).L == kNoLocal);
    case Opcode::Call:
    case Opcode::VCall:
    case Opcode::ICall:
      // A box returned is the caller's own and a runtime helper's object a
      // new one; another object or address may be one into an argument,
      // or into what the callee reached.
      return t != Type::Box && t != Type::I64 &&
             !(in.Op == Opcode::Call && in.Callee.starts_with(kRuntimePrefix));
    case Opcode::Box:
    case Opcode::New:
    case Opcode::WeakLoad:
    case Opcode::FnAddr:
    case Opcode::VTableAddr:
    case Opcode::VTableLoad:
      return false;
    default:
      return anyBorrowed(in);
    }
  }

  static bool merge(std::vector<LocalId> &into,
                    const std::vector<LocalId> &from) {
    bool changed = false;
    for (LocalId l : from) {
      auto it = std::lower_bound(into.begin(), into.end(), l);
      if (it == into.end() || *it != l) {
        into.insert(it, l);
        changed = true;
      }
    }
    return changed;
  }
  void walk(const Block &b, bool &changed) {
    for (const Stmt &s : b.Stmts) {
      if (const auto *in = std::get_if<Instr>(&s)) {
        visit(*in, changed);
      } else if (const auto *x = std::get_if<If>(&s)) {
        walk(*x->Then, changed);
        if (x->Else)
          walk(*x->Else, changed);
      } else if (const auto *x = std::get_if<While>(&s)) {
        walk(*x->CondBlock, changed);
        walk(*x->Body, changed);
      } else if (const auto *x = std::get_if<Switch>(&s)) {
        for (const Switch::Case &c : x->Cases)
          walk(*c.Body, changed);
        if (x->Default)
          walk(*x->Default, changed);
      }
    }
  }

  void visit(const Instr &in, bool &changed) {
    std::vector<LocalId> from;
    Address addr;
    switch (in.Op) {
    case Opcode::Load:
      if (refersTo(F.Locals[in.Local].Ty))
        from.push_back(in.Local);
      merge(from, Held[in.Local]);
      break;
    case Opcode::LocalAddr:
      from.push_back(in.Local);
      addr = {in.Local, 0};
      if (F.Locals[in.Local].Ty != Type::Mem && !AddressTaken[in.Local]) {
        AddressTaken[in.Local] = true;
        changed = true;
      }
      break;
    case Opcode::PtrAdd: {
      merge(from, locals(in.Args[0]));
      merge(from, locals(in.Args[1]));
      const Address base = address(in.Args[0]);
      addr.L = base.L;
      if (std::optional<int64_t> c = constantI64(in.Args[1]);
          base.Offset && c && *base.Offset + *c >= 0)
        addr.Offset = *base.Offset + *c;
      break;
    }
    case Opcode::Store:
      // A box local owns the reference stored in it (docs/pir.md §7: a
      // copy retained, or a fresh or handed-on one); anything else that
      // refers to something is an alias the local now holds.
      if (F.Locals[in.Local].Ty != Type::Box) {
        changed |= merge(Held[in.Local], locals(in.Args[0]));
        if (borrowed(in.Args[0]) && !HoldsBorrowed[in.Local]) {
          HoldsBorrowed[in.Local] = true;
          changed = true;
        }
      }
      return;
    case Opcode::PtrStore:
    case Opcode::FieldStore:
      // An alias stored in memory: what it refers to is no longer followed.
      if (!retained(in.Args[1]))
        for (LocalId l : locals(in.Args[1]))
          if (!Escaped[l]) {
            Escaped[l] = true;
            changed = true;
          }
      return;
    case Opcode::Box:
    case Opcode::New:
    case Opcode::WeakLoad:
    case Opcode::FnAddr:
    case Opcode::VTableAddr:
    case Opcode::VTableLoad:
      // A reference of its own, or a constant address.
      break;
    case Opcode::Call:
    case Opcode::VCall:
    case Opcode::ICall:
      // A box returned is the caller's reference; an object or an address
      // may be one into an argument.
      if (in.Result.Ty == Type::Obj || in.Result.Ty == Type::Ptr)
        for (const Operand &o : in.Args)
          merge(from, locals(o));
      break;
    case Opcode::Cast:
      // A reference cast to an integer is still one.
      merge(from, locals(in.Args[0]));
      break;
    default:
      if (refersTo(in.Result.Ty))
        for (const Operand &o : in.Args)
          merge(from, locals(o));
      break;
    }
    if (in.Result.Ty == Type::Void)
      return;
    std::vector<LocalId> &mine = From[in.Result.Id];
    changed |= merge(mine, from);
    Addr[in.Result.Id] = addr;
    if (!Borrowed[in.Result.Id] && borrowedResult(in)) {
      Borrowed[in.Result.Id] = true;
      changed = true;
    }
  }
};

/// What a node does to a slot.
enum class Event : uint8_t {
  None,    // nothing
  Release, // releases the slot's reference: the path ends well
  Bad,     // reads or writes the slot, uses an alias of it, or returns
};

/// A slot: a box local (Offset 0), or the reference at byte Offset of a
/// mem local.
struct Slot {
  LocalId L = kNoLocal;
  int64_t Offset = 0;
};

/// A set of slots, a bit each.
class Bits {
public:
  Bits(size_t n, bool all) : N(n), W((n + 63) / 64) { fill(all); }

  bool test(size_t i) const { return (W[i / 64] >> (i % 64) & 1) != 0; }
  void set(size_t i, bool v) {
    const uint64_t bit = uint64_t{1} << (i % 64);
    W[i / 64] = v ? W[i / 64] | bit : W[i / 64] & ~bit;
  }
  /// Every bit set to @p v.
  void fill(bool v) {
    std::fill(W.begin(), W.end(), v ? ~uint64_t{0} : 0);
    if (v && N % 64 != 0)
      W.back() = (uint64_t{1} << (N % 64)) - 1;
  }
  /// Each bit "and" (@p all) or "or" the same bit of @p o.
  void meet(const Bits &o, bool all) {
    for (size_t i = 0; i < W.size(); ++i)
      W[i] = all ? W[i] & o.W[i] : W[i] | o.W[i];
  }
  /// Set the bits of @p o, adding to @p added those that were clear.
  void add(const Bits &o, std::vector<size_t> &added) {
    for (size_t i = 0; i < W.size(); ++i)
      for (uint64_t fresh = o.W[i] & ~W[i]; fresh != 0; fresh &= fresh - 1)
        added.push_back(i * 64 + static_cast<size_t>(std::countr_zero(fresh)));
    meet(o, /*all=*/false);
  }
  bool operator==(const Bits &o) const { return W == o.W; }

private:
  size_t N;
  std::vector<uint64_t> W;
};

/// The slots a flow follows: a bit each, and the bits of each local's.
class Slots {
public:
  explicit Slots(size_t locals) : OfLocal(locals) {}

  /// The bit of @p s, made the first time.
  size_t add(Slot s) {
    if (std::optional<size_t> i = find(s))
      return *i;
    OfLocal[s.L].push_back(List.size());
    List.push_back(s);
    return List.size() - 1;
  }
  std::optional<size_t> find(Slot s) const {
    for (size_t i : OfLocal[s.L])
      if (List[i].Offset == s.Offset)
        return i;
    return std::nullopt;
  }
  size_t size() const { return List.size(); }
  const Slot &operator[](size_t i) const { return List[i]; }
  /// The bits of local @p l's slots.
  const std::vector<size_t> &of(LocalId l) const { return OfLocal[l]; }

private:
  std::vector<Slot> List;
  std::vector<std::vector<size_t>> OfLocal;
};

/// A dataflow over a function's structured body, a bit per slot: one walk
/// over its statements, forward from the entry or backward from the end,
/// that walks a loop again until the bits at its head stop changing.  A
/// must-flow's bit holds where it holds on every path (the meet is "and",
/// and a loop's head starts with every bit set); a may-flow's where it does
/// on some path ("or", from none set).
class Flow {
public:
  struct State {
    Bits B;
    /// Whether some path reaches the point (a forward flow's: not one
    /// after a `ret`).
    bool Reached = true;

    bool operator==(const State &o) const {
      return Reached == o.Reached && B == o.B;
    }
  };
  /// What node @p n does: called with the state before it (forward) or
  /// after it (backward), which it makes the state after (before) it.  A
  /// node in a loop is stepped through on every walk of the loop, the last
  /// time with its final state.
  using Step = std::function<void(uint32_t n, State &s)>;
  /// The state at the end of arm @p k of the if or switch @p s (forward):
  /// the arms in order, then and else, or the cases and the default, an
  /// absent else or default an arm too (not called for an absent default
  /// no value reaches, exhaustive).
  using ArmEnd = std::function<void(const Stmt &s, size_t k, const State &)>;

  Flow(const Graph &g, size_t bits, bool must, Step step,
       ArmEnd armEnd = nullptr)
      : G(g), N(bits), Must(must), StepFn(std::move(step)),
        ArmEndFn(std::move(armEnd)) {}

  /// Forward from the entry of @p body, where no bit is set.
  void forward(const Block &body) {
    State s{Bits(N, false), true};
    fwdBlock(body, s);
  }
  /// Backward from the end of @p body, which is stepped through first.
  void backward(const Block &body) {
    State s = start();
    StepFn(G.End, s);
    bwdBlock(body, s);
  }

private:
  const Graph &G;
  size_t N;
  bool Must;
  Step StepFn;
  ArmEnd ArmEndFn;
  /// The loops around the statement walked: forward, the states that
  /// `continue` and `break` leave them with; backward, the states at their
  /// head and after them.
  struct Loop {
    State Head;
    State Exit;
  };
  std::vector<Loop> Loops;
  /// The state each loop's head settled on the last time it was walked.
  /// A loop inside another is walked again on each walk of the outer one,
  /// whose states only ever move one way (down for a must-flow, up for a
  /// may-flow): starting from where it settled, it settles in a walk or two
  /// rather than in as many as the first time.
  std::unordered_map<const Stmt *, State> Heads;

  /// No path: the meet's identity.
  State none() const { return {Bits(N, Must), false}; }
  /// What a loop's head starts from, and the state after a `ret` or an
  /// `unreachable` before it is stepped through (backward).
  State start() const { return {Bits(N, Must), true}; }

  void meet(State &into, const State &o) const {
    if (!o.Reached)
      return;
    if (!into.Reached) {
      into = o;
      return;
    }
    into.B.meet(o.B, Must);
  }

  void fwdBlock(const Block &b, State &s) {
    for (const Stmt &st : b.Stmts)
      fwdStmt(st, s);
  }

  void fwdArm(const Stmt &st, size_t k, const Block *arm, const State &in,
              State &out) {
    State a = in;
    if (arm)
      fwdBlock(*arm, a);
    if (ArmEndFn)
      ArmEndFn(st, k, a);
    meet(out, a);
  }

  void fwdStmt(const Stmt &st, State &s) {
    if (std::holds_alternative<Instr>(st)) {
      StepFn(G.of(st), s);
    } else if (const auto *x = std::get_if<If>(&st)) {
      StepFn(G.of(st), s);
      State out = none();
      fwdArm(st, 0, x->Then.get(), s, out);
      fwdArm(st, 1, x->Else.get(), s, out);
      s = std::move(out);
    } else if (const auto *x = std::get_if<Switch>(&st)) {
      StepFn(G.of(st), s);
      State out = none();
      for (size_t k = 0; k < x->Cases.size(); ++k)
        fwdArm(st, k, x->Cases[k].Body.get(), s, out);
      if (x->Default || !exhaustive(*x))
        fwdArm(st, x->Cases.size(), x->Default.get(), s, out);
      s = std::move(out);
    } else if (const auto *x = std::get_if<While>(&st)) {
      const State entry = s;
      State head = entry;
      if (auto it = Heads.find(&st); it != Heads.end()) {
        head = it->second;
        meet(head, entry);
      }
      for (;;) {
        State test = head;
        fwdBlock(*x->CondBlock, test);
        StepFn(G.of(st), test);
        Loops.push_back({none(), none()});
        State body = test;
        fwdBlock(*x->Body, body);
        const Loop loop = std::move(Loops.back());
        Loops.pop_back();
        State next = entry;
        meet(next, body);
        meet(next, loop.Head);
        if (next == head) {
          Heads.insert_or_assign(&st, std::move(head));
          s = std::move(test);
          meet(s, loop.Exit);
          return;
        }
        head = std::move(next);
      }
    } else if (std::holds_alternative<Break>(st)) {
      meet(Loops.back().Exit, s);
      s = none();
    } else if (std::holds_alternative<Continue>(st)) {
      meet(Loops.back().Head, s);
      s = none();
    } else {
      StepFn(G.of(st), s); // `ret`, `unreachable`
      s = none();
    }
  }

  void bwdBlock(const Block &b, State &s) {
    for (size_t i = b.Stmts.size(); i-- > 0;)
      bwdStmt(b.Stmts[i], s);
  }

  void bwdArm(const Block *arm, const State &after, State &out) {
    State a = after;
    if (arm)
      bwdBlock(*arm, a);
    meet(out, a);
  }

  void bwdStmt(const Stmt &st, State &s) {
    if (std::holds_alternative<Instr>(st)) {
      StepFn(G.of(st), s);
    } else if (const auto *x = std::get_if<If>(&st)) {
      State out = none();
      bwdArm(x->Then.get(), s, out);
      bwdArm(x->Else.get(), s, out);
      s = std::move(out);
      StepFn(G.of(st), s);
    } else if (const auto *x = std::get_if<Switch>(&st)) {
      State out = none();
      for (const Switch::Case &c : x->Cases)
        bwdArm(c.Body.get(), s, out);
      if (x->Default || !exhaustive(*x))
        bwdArm(x->Default.get(), s, out);
      s = std::move(out);
      StepFn(G.of(st), s);
    } else if (const auto *x = std::get_if<While>(&st)) {
      const State exit = s;
      auto it = Heads.find(&st);
      State head = it != Heads.end() ? it->second : start();
      for (;;) {
        Loops.push_back({head, exit});
        State before = head;
        bwdBlock(*x->Body, before);
        Loops.pop_back();
        meet(before, exit);
        StepFn(G.of(st), before);
        bwdBlock(*x->CondBlock, before);
        if (before == head)
          break;
        head = std::move(before);
      }
      Heads.insert_or_assign(&st, head);
      s = std::move(head);
    } else if (std::holds_alternative<Break>(st)) {
      s = Loops.back().Exit;
    } else if (std::holds_alternative<Continue>(st)) {
      s = Loops.back().Head;
    } else {
      s = start(); // `ret`, `unreachable`: nothing runs after them
      StepFn(G.of(st), s);
    }
  }
};

/// A copy found: its retains, which a move removes, and null to store into
/// the slot's references before Before.  It is moved when every path from
/// the point after node Last reaches the release of each of them before
/// anything else happens to the slot.
struct Transfer {
  Slot S;
  /// The references moved (offsets in the slot's local; {0} for a box).
  std::vector<int64_t> Parts;
  std::vector<Place> Retains;
  Place Before;
  /// A mem local's address the null stores go through (its copy's source).
  Operand Base;
  /// The node the copy is complete at.
  uint32_t Last = 0;
  /// The node from which the destination holds the reference (and may let
  /// it go): a box's consumer, a mem local's Last.
  uint32_t Holds = 0;
};

/// One analysis of a function: its nodes and what its values refer to.
class Analysis {
public:
  explicit Analysis(Function &f) : F(f), G(f), V(f, G), O(f, G, V) {}

  Function &F;
  Graph G;
  Values V;
  Origins O;

  /// Whether the instruction at @p at is a `load` or a `ptr.load` whose
  /// value's one use is the `release` right after it.
  bool releaseLoad(Place at) const {
    const Instr *in = at.instr();
    if (!in || (in->Op != Opcode::Load && in->Op != Opcode::PtrLoad) ||
        in->Result.Ty != Type::Box || at.Index + 1 >= at.B->Stmts.size())
      return false;
    const auto *next = std::get_if<Instr>(&at.B->Stmts[at.Index + 1]);
    return next && next->Op == Opcode::Release &&
           sameValue(next->Args[0], in->Result.Id) &&
           V.users(in->Result.Id).size() == 1;
  }

  /// Whether @p o, used by node @p n, holds a reference of its own there:
  /// its one other use is a `retain` earlier in @p n's block.  The box a
  /// copy hands its consumer does, and so does an earlier copy of the same
  /// slot wherever it is used (`f(a, a)`: the first argument).  A copy made
  /// after a slot's copy reads the slot first, which ends that copy's last
  /// use before it.
  bool ownReference(const Operand &o, uint32_t n) const {
    if (!o.isValue())
      return false;
    const std::vector<uint32_t> &users = V.users(o.valueId());
    if (users.size() != 2 || (users[0] == n) == (users[1] == n))
      return false;
    const uint32_t other = users[0] == n ? users[1] : users[0];
    const Instr *in = G.instr(other);
    const Place at = G.Nodes[other].At;
    const Place here = G.Nodes[n].At;
    return in && in->Op == Opcode::Retain && at.B == here.B &&
           at.Index < here.Index;
  }

  /// What node @p n does to slot @p s.
  Event event(uint32_t n, Slot s) const {
    const Node &node = G.Nodes[n];
    switch (node.K) {
    case Node::Dead:
      // Nothing runs after it: the program has stopped.
      return Event::Release;
    case Node::Exit:
    case Node::Return:
      return Event::Bad;
    case Node::Test:
      return O.from(*node.Use, s.L) ? Event::Bad : Event::None;
    case Node::Instruction:
      break;
    }
    const Instr &in = *node.At.instr();
    if (F.Locals[s.L].Ty == Type::Box) {
      if (in.Op == Opcode::Load && in.Local == s.L)
        return releaseLoad(node.At) ? Event::Release : Event::Bad;
      if ((in.Op == Opcode::Store || in.Op == Opcode::LocalAddr) &&
          in.Local == s.L)
        return Event::Bad;
      for (const Operand &o : in.Args)
        if (O.from(o, s.L) && !ownReference(o, n))
          return Event::Bad;
      return Event::None;
    }
    // A reference in a mem local: what an address into it reads or writes.
    // The release of another of the local's references is no use of this
    // one.
    if (in.Op == Opcode::Release && releasesOtherPart(in.Args[0], s))
      return Event::None;
    for (size_t i = 0; i < in.Args.size(); ++i) {
      const Operand &o = in.Args[i];
      if (!O.from(o, s.L))
        continue;
      int64_t at = 0;
      const bool known = O.offsetIn(o, s.L, at);
      switch (in.Op) {
      case Opcode::PtrAdd:
        if (i == 0)
          continue; // an address computed, nothing touched
        return Event::Bad;
      case Opcode::PtrLoad:
        if (!known)
          return Event::Bad;
        if (at == s.Offset && releaseLoad(node.At))
          return Event::Release;
        if (overlaps(at, storeSize(in.Result.Ty), s.Offset))
          return Event::Bad;
        continue;
      case Opcode::PtrStore:
        if (i != 0 || !known ||
            overlaps(at, storeSize(V.type(in.Args[1])), s.Offset))
          return Event::Bad;
        continue;
      case Opcode::MemCopy:
      case Opcode::MemZero: {
        const Operand &len = in.Args.back();
        const std::optional<int64_t> n = constantI64(len);
        if (&o == &len || !known || !n || overlaps(at, *n, s.Offset))
          return Event::Bad;
        continue;
      }
      default:
        return Event::Bad;
      }
    }
    return Event::None;
  }

  /// By node: whether it may release a reference (mayRelease) while a
  /// borrowed reference (Origins::borrowed) is in use across it: one made
  /// before it and used after it, one a call it makes is given (a method's
  /// receiver read from a field), or a local holding one (`self`) read
  /// after it.  An object such a reference depends on may die there.  A
  /// use in a loop the reference was made outside of is in use at every
  /// node of the loop; one in another arm may be counted too.
  std::vector<bool> exposed() const {
    const size_t n = G.Nodes.size();
    // Each borrow's places [from, to], as +1 at from and -1 after to.
    std::vector<int64_t> starts(n + 1);
    std::vector<bool> given(n);
    // The last place before a use at node @p u by which a reference made
    // at @p def (or at the entry) is in use.
    auto until = [&](uint32_t u, std::optional<uint32_t> def) {
      auto to = static_cast<int64_t>(G.pos(u)) - 1;
      for (uint32_t l = G.LoopOf[u];
           l != kNoLoop && !(def && G.inLoop(*def, l)); l = G.Loops[l].Parent)
        to = std::max<int64_t>(to, G.loopEnd(l));
      return to;
    };
    auto span = [&](int64_t from, int64_t to) {
      if (from > to)
        return;
      ++starts[from];
      --starts[to + 1];
    };
    for (ValueId v = 0; v < V.size(); ++v) {
      if (!O.borrowed(v))
        continue;
      std::optional<uint32_t> def;
      if (const Place d = V.defAt(v); d.B)
        def = G.of(d.stmt());
      int64_t to = -1;
      for (uint32_t u : V.users(v)) {
        if (const Instr *in = G.instr(u); in && isCall(in->Op))
          given[u] = true;
        to = std::max(to, until(u, def));
      }
      span(def ? G.pos(*def) + 1 : 0, to);
    }
    std::unordered_map<LocalId, int64_t> held;
    for (uint32_t u = 0; u < n; ++u)
      if (const Instr *in = G.instr(u);
          in && in->Op == Opcode::Load && O.holdsBorrowed(in->Local)) {
        int64_t &to = held.try_emplace(in->Local, -1).first->second;
        to = std::max(to, until(u, std::nullopt));
      }
    for (const auto &[l, to] : held)
      span(0, to);
    std::vector<bool> out(n);
    int64_t open = 0;
    for (uint32_t p = 0; p < n; ++p) {
      open += starts[p];
      const uint32_t u = static_cast<uint32_t>(n - 1 - p);
      const Instr *in = G.instr(u);
      out[u] = in && mayRelease(*in) && (open > 0 || given[u]);
    }
    return out;
  }

  /// Whether @p o is a reference of @p s's local other than @p s, loaded
  /// to be released (releaseLoad).
  bool releasesOtherPart(const Operand &o, Slot s) const {
    if (!o.isValue())
      return false;
    const Instr *d = V.def(o.valueId());
    if (!d || d->Op != Opcode::PtrLoad || !releaseLoad(V.defAt(o.valueId())))
      return false;
    const Address a = O.address(d->Args[0]);
    return a.L == s.L && a.Offset && !overlaps(*a.Offset, 8, s.Offset);
  }

  /// Calls @p fn with the bit of each of @p slots that node @p n may touch:
  /// a slot of a local it loads, stores or addresses, or of one a value it
  /// uses may refer to.  Nothing else happens to the others there.
  template <typename Fn>
  void touched(uint32_t n, const Slots &slots, Fn fn) const {
    std::vector<LocalId> ls;
    auto add = [&](const Operand &o) {
      const std::vector<LocalId> &from = O.locals(o);
      ls.insert(ls.end(), from.begin(), from.end());
    };
    if (const Instr *in = G.instr(n)) {
      if (in->Op == Opcode::Load || in->Op == Opcode::Store ||
          in->Op == Opcode::LocalAddr)
        ls.push_back(in->Local);
      for (const Operand &o : in->Args)
        add(o);
    } else if (G.Nodes[n].Use) {
      add(*G.Nodes[n].Use);
    }
    std::sort(ls.begin(), ls.end());
    ls.erase(std::unique(ls.begin(), ls.end()), ls.end());
    for (LocalId l : ls)
      for (size_t i : slots.of(l))
        fn(i);
  }

  /// The step of the flow that finds the last uses (backward, must): given
  /// in @p ok, for each of @p slots, whether every path from the point
  /// after node @p n reaches the slot's release (or `unreachable`) before
  /// anything else happens to it, make it the same for the point before.
  void lastUse(uint32_t n, const Slots &slots, Bits &ok) const {
    switch (G.Nodes[n].K) {
    case Node::Dead:
      ok.fill(true);
      return;
    case Node::Exit:
    case Node::Return:
      ok.fill(false);
      return;
    case Node::Test:
    case Node::Instruction:
      break;
    }
    touched(n, slots, [&](size_t i) {
      switch (event(n, slots[i])) {
      case Event::None:
        break;
      case Event::Release:
        ok.set(i, true);
        break;
      case Event::Bad:
        ok.set(i, false);
        break;
      }
    });
  }

  /// The offset of address @p p from address @p base, when @p p is @p base
  /// plus constants.
  std::optional<int64_t> offsetFrom(const Operand &p,
                                    const Operand &base) const {
    if (!p.isValue() || !base.isValue())
      return std::nullopt;
    int64_t off = 0;
    for (ValueId v = p.valueId(); v != base.valueId();) {
      const Instr *d = V.def(v);
      if (!d || d->Op != Opcode::PtrAdd || !d->Args[0].isValue())
        return std::nullopt;
      std::optional<int64_t> c = constantI64(d->Args[1]);
      if (!c)
        return std::nullopt;
      off += *c;
      v = d->Args[0].valueId();
    }
    return off;
  }

  /// A box local's copy at the retain @p r: `%v = load %x`, `retain %v`,
  /// and `%v` handed to its consumer, all in one block.  Whether it is the
  /// slot's last use is for the flow to tell (Mover::transfer).
  std::optional<Transfer> boxCopy(uint32_t r) const {
    const Instr &ret = *G.instr(r);
    const Place at = G.Nodes[r].At;
    if (!ret.Args[0].isValue())
      return std::nullopt;
    const ValueId v = ret.Args[0].valueId();
    const Instr *load = V.def(v);
    if (!load || load->Op != Opcode::Load)
      return std::nullopt;
    const LocalId x = load->Local;
    if (F.Locals[x].Ty != Type::Box || !O.movable(x))
      return std::nullopt;
    const std::vector<uint32_t> &users = V.users(v);
    if (users.size() != 2 || users[0] == users[1])
      return std::nullopt;
    const uint32_t c = users[0] == r ? users[1] : users[0];
    const Place def = V.defAt(v);
    const Place use = G.Nodes[c].At;
    if (def.B != at.B || use.B != at.B || def.Index > at.Index ||
        use.Index < at.Index)
      return std::nullopt;
    // The slot still holds %v at the retain.
    for (size_t i = def.Index + 1; i < at.Index; ++i) {
      const auto *in = std::get_if<Instr>(&at.B->Stmts[i]);
      if (!in || (in->Op == Opcode::Store && in->Local == x) ||
          in->Op == Opcode::Release)
        return std::nullopt;
    }
    if (!consumes(c, v))
      return std::nullopt;
    Transfer t;
    t.S = {x, 0};
    t.Parts = {0};
    t.Retains = {at};
    t.Before = {at.B, at.Index + 1};
    t.Last = r;
    t.Holds = c;
    return t;
  }

  /// Whether node @p c takes the reference %v as its own: a call's
  /// argument it does not lend (the callee releases it, or hands on what
  /// its caller handed over), a store into a local, a field (not a
  /// closure's capture) or memory, or the value returned.
  bool consumes(uint32_t c, ValueId v) const {
    const Node &node = G.Nodes[c];
    if (node.K == Node::Return)
      return node.Use && sameValue(*node.Use, v);
    const Instr *in = G.instr(c);
    if (!in)
      return false;
    switch (in->Op) {
    case Opcode::Store:
      return true;
    case Opcode::Call:
    case Opcode::VCall:
    case Opcode::ICall:
      for (size_t i = in->Op == Opcode::Call ? 0 : 1; i < in->Args.size(); ++i)
        if (sameValue(in->Args[i], v) && in->ownership(i) != ArgOwnership::Lent)
          return true;
      return false;
    case Opcode::FieldStore:
      return sameValue(in->Args[1], v) && !isClosureClass(in->ClassName);
    case Opcode::PtrStore:
      return sameValue(in->Args[1], v);
    default:
      return false;
    }
  }

  /// Whether address @p p is into a closure's field.
  bool intoClosure(const Operand &p) const {
    for (Operand o = p; o.isValue();) {
      const Instr *d = V.def(o.valueId());
      if (!d)
        return false;
      if (d->Op == Opcode::FieldAddr)
        return isClosureClass(d->ClassName);
      if (d->Op != Opcode::PtrAdd)
        return false;
      o = d->Args[0];
    }
    return false;
  }

  /// The retains a `mem.copy` at @p copy makes of the references it
  /// duplicates, by their offset in the bytes copied: loads from the
  /// destination right after it, or from the source before it (an
  /// assignment takes the new references before releasing the old ones).
  /// @p last is set to the place the copy is complete at.
  std::vector<std::pair<int64_t, Place>> copyRetains(Place copy,
                                                     Place &last) const {
    std::vector<std::pair<int64_t, Place>> out;
    const Instr &mc = *copy.instr();
    const std::optional<int64_t> len = constantI64(mc.Args[2]);
    if (!len)
      return out;
    const int64_t size = *len;
    auto inBytes = [size](std::optional<int64_t> k) {
      return k.has_value() && *k >= 0 && *k + 8 <= size;
    };
    // After: [ptr.add] ptr.load box %dst+k, retain.
    for (size_t i = copy.Index + 1; i < copy.B->Stmts.size(); ++i) {
      const auto *in = std::get_if<Instr>(&copy.B->Stmts[i]);
      if (!in)
        break;
      if (in->Op == Opcode::PtrAdd && offsetFrom(in->Args[0], mc.Args[0]))
        continue;
      if (in->Op == Opcode::PtrLoad && in->Result.Ty == Type::Box &&
          inBytes(offsetFrom(in->Args[0], mc.Args[0])))
        continue;
      if (in->Op != Opcode::Retain || !in->Args[0].isValue())
        break;
      const ValueId box = in->Args[0].valueId();
      const Instr *d = V.def(box);
      const Place dp = V.defAt(box);
      if (!d || d->Op != Opcode::PtrLoad || dp.B != copy.B ||
          dp.Index < copy.Index || V.users(box).size() != 1)
        break;
      const std::optional<int64_t> k = offsetFrom(d->Args[0], mc.Args[0]);
      if (!k || !inBytes(k))
        break;
      out.emplace_back(*k, Place{copy.B, i});
      last = {copy.B, i};
    }
    if (!out.empty())
      return out;
    // Before: [ptr.add] ptr.load box %src+k, retain, then the destination's
    // old references released.
    last = copy;
    for (size_t i = copy.Index; i-- > 0;) {
      const auto *in = std::get_if<Instr>(&copy.B->Stmts[i]);
      if (!in)
        break;
      if (in->Op == Opcode::Retain && in->Args[0].isValue()) {
        const Instr *d = V.def(in->Args[0].valueId());
        const Place dp = V.defAt(in->Args[0].valueId());
        if (d && d->Op == Opcode::PtrLoad && dp.B == copy.B &&
            V.users(in->Args[0].valueId()).size() == 1)
          if (std::optional<int64_t> k = offsetFrom(d->Args[0], mc.Args[1]);
              k && inBytes(k))
            out.emplace_back(*k, Place{copy.B, i});
        continue;
      }
      switch (in->Op) {
      case Opcode::Release:
      case Opcode::PtrLoad:
      case Opcode::PtrAdd:
      case Opcode::LocalAddr:
      case Opcode::FieldAddr:
      case Opcode::Load:
      case Opcode::Unbox:
        continue;
      default:
        break;
      }
      break;
    }
    return out;
  }

  /// A whole mem local's copy at the `mem.copy` node @p n (its last use or
  /// not, as for boxCopy).
  std::optional<Transfer> memCopy(uint32_t n) const {
    const Instr &mc = *G.instr(n);
    const Place at = G.Nodes[n].At;
    const Address src = O.address(mc.Args[1]);
    if (src.L == kNoLocal || src.Offset != 0)
      return std::nullopt;
    const LocalId m = src.L;
    const std::optional<int64_t> len = constantI64(mc.Args[2]);
    if (F.Locals[m].Ty != Type::Mem || !O.movable(m) ||
        len != F.Locals[m].Size || O.from(mc.Args[0], m) ||
        intoClosure(mc.Args[0]))
      return std::nullopt;
    Place last;
    std::vector<std::pair<int64_t, Place>> retains = copyRetains(at, last);
    if (retains.empty())
      return std::nullopt;
    // One retain per reference the copy duplicates; a second one would be
    // a reference of someone else's.
    std::set<int64_t> parts;
    for (const auto &[k, p] : retains)
      if (!parts.insert(k).second)
        return std::nullopt;
    // Taken before the copy: nothing else touches the local in between,
    // so the bytes copied are the references retained.
    if (last.Index == at.Index) {
      std::unordered_set<ValueId> group;
      size_t first = at.Index;
      for (const auto &[k, p] : retains) {
        const ValueId box = p.instr()->Args[0].valueId();
        group.insert(box);
        first = std::min(first, V.defAt(box).Index);
        for (Operand a = V.def(box)->Args[0]; a.isValue();) {
          const Instr *d = V.def(a.valueId());
          if (!group.insert(a.valueId()).second || !d ||
              d->Op != Opcode::PtrAdd)
            break;
          a = d->Args[0];
        }
      }
      for (size_t i = first; i < at.Index; ++i) {
        // What an if or a loop in between does is not looked into.
        const Instr *in = std::get_if<Instr>(&at.B->Stmts[i]);
        if (!in)
          return std::nullopt;
        if (in->Result.Ty != Type::Void && group.count(in->Result.Id))
          continue;
        if (in->Op == Opcode::Retain && in->Args[0].isValue() &&
            group.count(in->Args[0].valueId()))
          continue;
        for (const Operand &o : in->Args)
          if (O.from(o, m))
            return std::nullopt;
      }
    }
    Transfer t;
    t.S = {m, 0};
    t.Base = mc.Args[1];
    for (const auto &[k, p] : retains) {
      t.Parts.push_back(k);
      t.Retains.push_back(p);
    }
    t.Before = {last.B, last.Index + 1};
    t.Last = G.of(last.stmt());
    t.Holds = t.Last;
    return t;
  }
};

/// Changes to a function's blocks, made at once: statements removed, and
/// instructions inserted before an index of a block (in the order they
/// were asked for at one index).
class Edits {
public:
  void remove(const Stmt &s) { Removed.insert(&s); }
  bool removed(const Stmt &s) const { return Removed.count(&s) > 0; }
  void insert(Block &b, size_t before, Instr in) {
    Inserts[&b].emplace_back(before, std::move(in));
  }

  void apply(Block &b) {
    auto ins = Inserts.find(&b);
    std::vector<std::pair<size_t, Instr>> none;
    std::vector<std::pair<size_t, Instr>> &list =
        ins == Inserts.end() ? none : ins->second;
    std::stable_sort(
        list.begin(), list.end(),
        [](const auto &a, const auto &c) { return a.first < c.first; });
    std::vector<Stmt> out;
    out.reserve(b.Stmts.size() + list.size());
    size_t k = 0;
    for (size_t i = 0; i <= b.Stmts.size(); ++i) {
      for (; k < list.size() && list[k].first == i; ++k)
        out.emplace_back(std::in_place_type<Instr>, std::move(list[k].second));
      if (i < b.Stmts.size() && !removed(b.Stmts[i]))
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
  std::unordered_map<Block *, std::vector<std::pair<size_t, Instr>>> Inserts;
};

/// Removes instructions, and with them the loads and address computations
/// nothing uses any more.
class Remover {
public:
  Remover(const Analysis &a, Edits &e) : A(a), E(e) {}

  void remove(Place at) {
    const Instr *in = at.instr();
    if (E.removed(at.stmt()))
      return;
    E.remove(at.stmt());
    if (!in)
      return;
    for (const Operand &o : in->Args) {
      if (!o.isValue())
        continue;
      const ValueId v = o.valueId();
      const Instr *d = A.V.def(v);
      if (!d || !pure(*d))
        continue;
      auto it = Remaining.try_emplace(v, A.V.users(v).size()).first;
      if (--it->second == 0)
        remove(A.V.defAt(v));
    }
  }

private:
  const Analysis &A;
  Edits &E;
  /// The uses left of each value an instruction removed used.
  std::unordered_map<ValueId, size_t> Remaining;

  static bool pure(const Instr &in) {
    switch (in.Op) {
    case Opcode::Load:
    case Opcode::LocalAddr:
    case Opcode::PtrAdd:
    case Opcode::PtrLoad:
      return true;
    default:
      return false;
    }
  }
};

class Mover {
public:
  explicit Mover(Function &f) : F(f) {}

  MoveCounts run() {
    // A cleanup may take away what kept another copy a copy (a release a
    // borrowed reference was in use across, moved into an arm): until a
    // round moves nothing, so that the pass changes nothing on its own
    // output.
    while (transfer()) {
      // Each round of the cleanup may make more of it possible (a release
      // moved into an arm leaves the null stores of the others unread).
      for (bool changed = true; changed;) {
        changed = dropNullReleases();
        changed = dropDeadNullStores() || changed;
      }
    }
    return Counts;
  }

private:
  Function &F;
  MoveCounts Counts;
  /// The slots null was stored into: a box local's (offset 0) and the
  /// references of mem locals.
  std::set<std::pair<LocalId, int64_t>> Moved;

  bool transfer() {
    Analysis a(F);
    std::vector<Transfer> copies;
    for (uint32_t n = 0; n < a.G.Nodes.size(); ++n) {
      const Instr *in = a.G.instr(n);
      std::optional<Transfer> t;
      if (in && in->Op == Opcode::Retain)
        t = a.boxCopy(n);
      else if (in && in->Op == Opcode::MemCopy)
        t = a.memCopy(n);
      if (t)
        copies.push_back(std::move(*t));
    }
    if (copies.empty())
      return false;
    // Which copies are last uses: one flow back from the end of the body,
    // a bit for each slot a copy is of, read at the point after each copy.
    Slots slots(F.Locals.size());
    std::vector<std::vector<size_t>> bits(copies.size());
    std::unordered_map<uint32_t, std::vector<size_t>> after;
    for (size_t i = 0; i < copies.size(); ++i) {
      for (int64_t k : copies[i].Parts)
        bits[i].push_back(slots.add({copies[i].S.L, k}));
      after[copies[i].Last].push_back(i);
    }
    std::vector<bool> last(copies.size());
    Flow(a.G, slots.size(), /*must=*/true, [&](uint32_t n, Flow::State &s) {
      if (auto it = after.find(n); it != after.end())
        for (size_t i : it->second)
          last[i] = std::all_of(bits[i].begin(), bits[i].end(),
                                [&](size_t b) { return s.B.test(b); });
      a.lastUse(n, slots, s.B);
    }).backward(F.Body);
    // Which of them would let an object die under a borrowed reference:
    // after a move the destination may let the object go before the slot
    // would have, so a node that may release a reference while a borrowed
    // one is in use across it (Analysis::exposed), on a path from where the
    // destination holds the reference to the slot's release, keeps the
    // slot's copies copies.  One flow forward from the entry: a slot's bits
    // are set from the destination on and cleared at its release.  A
    // release that the moves leave releasing nothing (idleReleases) is no
    // such node while every move it counts on is made: the windows open at
    // it are noted, and become risky if one of those moves is not.
    const std::vector<bool> exposed = a.exposed();
    const std::vector<std::optional<size_t>> idle =
        idleReleases(a, copies, bits, slots, last);
    std::unordered_map<uint32_t, std::vector<size_t>> holds;
    for (size_t i = 0; i < copies.size(); ++i)
      if (last[i])
        holds[copies[i].Holds].push_back(i);
    Bits risky(slots.size(), false);
    std::unordered_map<uint32_t, Bits> openAt;
    Flow(a.G, slots.size(), /*must=*/false, [&](uint32_t n, Flow::State &s) {
      if (exposed[n] && idle[n])
        openAt.try_emplace(n, slots.size(), false)
            .first->second.meet(s.B, /*all=*/false);
      else if (exposed[n])
        risky.meet(s.B, /*all=*/false);
      if (auto it = holds.find(n); it != holds.end())
        for (size_t i : it->second)
          for (size_t b : bits[i]) {
            // A call given the reference may let it go before it returns.
            if (exposed[n])
              risky.set(b, true);
            s.B.set(b, true);
          }
      a.touched(n, slots, [&](size_t i) {
        if (a.event(n, slots[i]) == Event::Release)
          s.B.set(i, false);
      });
    }).forward(F.Body);
    // The copies kept copies: those not at a last use, those with a risky
    // bit, and those whose retain an earlier copy has taken.  Keeping one at
    // a last use makes its slots' idle releases releases again, and the
    // windows open at them risky.
    std::vector<std::vector<size_t>> copiesOf(slots.size());
    for (size_t i = 0; i < copies.size(); ++i)
      for (size_t b : bits[i])
        copiesOf[b].push_back(i);
    std::vector<std::vector<uint32_t>> idleOf(slots.size());
    for (const auto &[n, open] : openAt)
      if (const std::optional<size_t> &bit = idle[n])
        idleOf[*bit].push_back(n);
    std::vector<bool> kept(copies.size());
    for (size_t i = 0; i < copies.size(); ++i)
      kept[i] = !last[i];
    std::vector<bool> lost(slots.size());
    std::vector<size_t> work;
    auto keep = [&](size_t i) {
      if (kept[i])
        return;
      kept[i] = true;
      for (size_t b : bits[i])
        if (!lost[b]) {
          lost[b] = true;
          for (uint32_t n : idleOf[b])
            risky.add(openAt.at(n), work);
        }
    };
    auto settle = [&] {
      while (!work.empty()) {
        const size_t b = work.back();
        work.pop_back();
        for (size_t i : copiesOf[b])
          keep(i);
      }
    };
    for (size_t b = 0; b < slots.size(); ++b)
      if (risky.test(b))
        work.push_back(b);
    settle();
    // A retain belongs to one copy.
    std::vector<Transfer> found;
    std::unordered_set<const Stmt *> claimed;
    for (size_t i = 0; i < copies.size(); ++i) {
      Transfer &t = copies[i];
      if (kept[i])
        continue;
      if (std::any_of(t.Retains.begin(), t.Retains.end(), [&](const Place &p) {
            return claimed.count(&p.stmt()) > 0;
          })) {
        keep(i);
        settle();
        continue;
      }
      for (const Place &p : t.Retains)
        claimed.insert(&p.stmt());
    }
    for (size_t i = 0; i < copies.size(); ++i)
      if (!kept[i])
        found.push_back(std::move(copies[i]));
    if (found.empty())
      return false;
    Edits e;
    Remover r(a, e);
    for (Transfer &t : found) {
      for (const Place &p : t.Retains)
        r.remove(p);
      for (int64_t k : t.Parts) {
        Moved.insert({t.S.L, k});
        Instr store;
        if (F.Locals[t.S.L].Ty == Type::Box) {
          store.Op = Opcode::Store;
          store.Local = t.S.L;
          store.Args = {Operand::null(Type::Box)};
          e.insert(*t.Before.B, t.Before.Index, std::move(store));
          continue;
        }
        Operand at = t.Base;
        if (k != 0) {
          Instr add;
          add.Op = Opcode::PtrAdd;
          add.Result = {F.NextValueId++, Type::Ptr, "moved.addr"};
          add.Args = {t.Base, Operand::i64(k)};
          at = Operand::value(add.Result);
          e.insert(*t.Before.B, t.Before.Index, std::move(add));
        }
        store.Op = Opcode::PtrStore;
        store.Args = {at, Operand::null(Type::Box)};
        e.insert(*t.Before.B, t.Before.Index, std::move(store));
      }
      ++Counts.Transfers;
      Counts.Retains += t.Retains.size();
    }
    e.apply(F.Body);
    return true;
  }

  /// The moved slots, as a flow's bits.
  Slots moved() const {
    Slots slots(F.Locals.size());
    for (const auto &[l, k] : Moved)
      slots.add({l, k});
    return slots;
  }

  /// The slot node @p n loads to release (a release load), if it is one.
  static std::optional<Slot> releaseSlot(const Analysis &a, uint32_t n) {
    const Instr *in = a.G.instr(n);
    if (!in || !a.releaseLoad(a.G.Nodes[n].At))
      return std::nullopt;
    if (in->Op == Opcode::Load)
      return Slot{in->Local, 0};
    const Address p = a.O.address(in->Args[0]);
    if (p.L == kNoLocal || !p.Offset)
      return std::nullopt;
    return Slot{p.L, *p.Offset};
  }

  /// The moved slot node @p n releases (a release load), if it is one.
  std::optional<Slot> releasedSlot(const Analysis &a, uint32_t n) const {
    std::optional<Slot> s = releaseSlot(a, n);
    if (!s || !Moved.count({s->L, s->Offset}))
      return std::nullopt;
    return s;
  }

  /// By node: for the `release` of a slot of @p slots that finds it null on
  /// every path once the copies at a last use (@p last) are moved, the
  /// slot's bit.  Such a release releases nothing, and the cleanup removes
  /// it (dropNullReleases).  One flow forward from the entry, null set at
  /// each move's null stores, as dropNullReleases has it after them.
  std::vector<std::optional<size_t>>
  idleReleases(const Analysis &a, const std::vector<Transfer> &copies,
               const std::vector<std::vector<size_t>> &bits, const Slots &slots,
               const std::vector<bool> &last) const {
    std::unordered_map<uint32_t, std::vector<size_t>> nulled;
    for (size_t i = 0; i < copies.size(); ++i)
      if (last[i])
        nulled[copies[i].Last].push_back(i);
    std::vector<std::optional<size_t>> idle(a.G.Nodes.size());
    Flow(a.G, slots.size(), /*must=*/true, [&](uint32_t n, Flow::State &s) {
      const Instr *in = a.G.instr(n);
      if (!in)
        return;
      // A node in a loop is stepped through again, the last time with its
      // final state.
      if (std::optional<Slot> r = releaseSlot(a, n))
        if (std::optional<size_t> bit = slots.find(*r)) {
          const Place at = a.G.Nodes[n].At;
          idle[a.G.of(at.B->Stmts[at.Index + 1])] =
              s.Reached && s.B.test(*bit) ? bit : std::nullopt;
        }
      a.touched(n, slots, [&](size_t i) {
        s.B.set(i, nullAfter(a, *in, slots[i], s.B.test(i)));
      });
      if (auto it = nulled.find(n); it != nulled.end())
        for (size_t i : it->second)
          for (size_t b : bits[i])
            s.B.set(b, true);
    }).forward(F.Body);
    return idle;
  }

  /// Whether @p o may be an address (a `ptr`, or an integer one was cast
  /// to), not a reference.
  static bool mayAddress(const Analysis &a, const Operand &o) {
    const Type t = a.V.type(o);
    return t != Type::Box && t != Type::Obj && t != Type::Weak;
  }

  /// Whether slot @p s is null after @p in, given whether it was before.
  bool nullAfter(const Analysis &a, const Instr &in, Slot s,
                 bool isNull) const {
    if (F.Locals[s.L].Ty == Type::Box)
      return in.Op == Opcode::Store && in.Local == s.L ? isNullBox(in.Args[0])
                                                       : isNull;
    for (size_t j = 0; j < in.Args.size(); ++j) {
      const Operand &o = in.Args[j];
      // What a reference loaded from the local refers to is not the local:
      // only an address writes it.
      if (!a.O.from(o, s.L) || !mayAddress(a, o))
        continue;
      int64_t at = 0;
      const bool known = a.O.offsetIn(o, s.L, at);
      if ((in.Op == Opcode::PtrAdd && j == 0) || in.Op == Opcode::PtrLoad)
        continue; // computes an address, or reads
      if (in.Op == Opcode::MemCopy && j == 1)
        continue; // the source, read
      if (in.Op == Opcode::PtrStore && j == 0 && known) {
        const int64_t size = storeSize(a.V.type(in.Args[1]));
        if (at == s.Offset && isNullBox(in.Args[1]))
          isNull = true;
        else if (overlaps(at, size, s.Offset))
          isNull = false;
        continue;
      }
      if (in.Op == Opcode::MemZero && j == 0 && known)
        if (std::optional<int64_t> len = constantI64(in.Args[1])) {
          if (covers(at, *len, s.Offset))
            isNull = true;
          else if (overlaps(at, *len, s.Offset))
            isNull = false;
          continue;
        }
      if (in.Op == Opcode::MemCopy && j == 0 && known)
        if (std::optional<int64_t> len = constantI64(in.Args[2])) {
          if (overlaps(at, *len, s.Offset))
            isNull = false;
          continue;
        }
      // Anything else may write through the address.
      isNull = false;
    }
    return isNull;
  }

  /// What became of a moved box local at the end of an arm.
  enum class Arm : uint8_t { Unreached, Null, Holding };

  /// An if or a switch, statement S of block B, that the release of a
  /// moved box local (the load at index J, then the release) follows.
  struct Sink {
    Stmt *S;
    Block *B;
    size_t J;
    size_t Bit;
    std::vector<Arm> Arms;
  };

  /// Remove the releases of moved slots that are null on every path to
  /// them, and move a release that follows an if or a switch into its one
  /// arm in which the slot may hold a reference.
  bool dropNullReleases() {
    Analysis a(F);
    const Slots slots = moved();
    const size_t nodes = a.G.Nodes.size();
    std::vector<std::optional<size_t>> released(nodes);
    for (uint32_t n = 0; n < nodes; ++n)
      if (std::optional<Slot> s = releasedSlot(a, n))
        released[n] = slots.find(*s);
    std::vector<Sink> sinks;
    sinkable(a, F.Body, slots, sinks);
    std::unordered_map<const Stmt *, size_t> sinkOf;
    for (size_t i = 0; i < sinks.size(); ++i)
      sinkOf[sinks[i].S] = i;
    // Whether each moved slot is null on every path to a point.
    std::vector<bool> null(nodes);
    Flow(
        a.G, slots.size(), /*must=*/true,
        [&](uint32_t n, Flow::State &s) {
          if (released[n])
            null[n] = s.Reached && s.B.test(*released[n]);
          if (const Instr *in = a.G.instr(n))
            a.touched(n, slots, [&](size_t i) {
              s.B.set(i, nullAfter(a, *in, slots[i], s.B.test(i)));
            });
        },
        [&](const Stmt &st, size_t k, const Flow::State &s) {
          auto it = sinkOf.find(&st);
          if (it == sinkOf.end())
            return;
          Sink &sink = sinks[it->second];
          sink.Arms[k] = !s.Reached           ? Arm::Unreached
                         : s.B.test(sink.Bit) ? Arm::Null
                                              : Arm::Holding;
        })
        .forward(F.Body);
    Edits e;
    Remover r(a, e);
    bool changed = false;
    for (uint32_t n = 0; n < nodes; ++n) {
      if (!null[n])
        continue;
      const Place at = a.G.Nodes[n].At;
      r.remove({at.B, at.Index + 1});
      ++Counts.Releases;
      changed = true;
    }
    for (const Sink &sink : sinks)
      changed |= sinkRelease(sink, e);
    e.apply(F.Body);
    return changed;
  }

  /// The ifs and switches of @p b and the blocks in it that the release of
  /// a moved box local follows: only loads of other locals, which no
  /// release changes, may come between them.
  void sinkable(const Analysis &a, Block &b, const Slots &slots,
                std::vector<Sink> &out) const {
    for (size_t i = 0; i < b.Stmts.size(); ++i) {
      Stmt &s = b.Stmts[i];
      size_t arms = 0;
      if (auto *x = std::get_if<If>(&s)) {
        sinkable(a, *x->Then, slots, out);
        if (x->Else)
          sinkable(a, *x->Else, slots, out);
        arms = 2;
      } else if (auto *x = std::get_if<While>(&s)) {
        sinkable(a, *x->CondBlock, slots, out);
        sinkable(a, *x->Body, slots, out);
        continue;
      } else if (auto *x = std::get_if<Switch>(&s)) {
        for (Switch::Case &c : x->Cases)
          sinkable(a, *c.Body, slots, out);
        if (x->Default)
          sinkable(a, *x->Default, slots, out);
        arms = x->Cases.size() + 1;
      } else {
        continue;
      }
      size_t j = i + 1;
      std::optional<Slot> slot;
      std::vector<LocalId> passed;
      for (; j < b.Stmts.size(); ++j) {
        const auto *in = std::get_if<Instr>(&b.Stmts[j]);
        if (!in || in->Op != Opcode::Load)
          break;
        if (a.releaseLoad({&b, j})) {
          slot = releasedSlot(a, a.G.of(b.Stmts[j]));
          break;
        }
        passed.push_back(in->Local);
      }
      if (!slot || F.Locals[slot->L].Ty != Type::Box ||
          std::find(passed.begin(), passed.end(), slot->L) != passed.end())
        continue;
      if (const std::optional<size_t> bit = slots.find(*slot))
        out.push_back(
            {&s, &b, j, *bit, std::vector<Arm>(arms, Arm::Unreached)});
    }
  }

  /// Move @p sink's release into its one arm reached that ends holding a
  /// reference, when it has others reached; true if it did.
  static bool sinkRelease(const Sink &sink, Edits &e) {
    Block &b = *sink.B;
    if (e.removed(b.Stmts[sink.J + 1]))
      return false;
    size_t through = 0;
    std::vector<size_t> holding;
    for (size_t k = 0; k < sink.Arms.size(); ++k) {
      if (sink.Arms[k] == Arm::Unreached)
        continue;
      ++through;
      if (sink.Arms[k] == Arm::Holding)
        holding.push_back(k);
    }
    if (through < 2 || holding.size() != 1)
      return false;
    Block &arm = armBlock(*sink.S, holding.front());
    e.insert(arm, arm.Stmts.size(), std::get<Instr>(b.Stmts[sink.J]));
    e.insert(arm, arm.Stmts.size(), std::get<Instr>(b.Stmts[sink.J + 1]));
    e.remove(b.Stmts[sink.J]);
    e.remove(b.Stmts[sink.J + 1]);
    return true;
  }

  /// Arm @p k of the if or switch @p s, made when it is an absent else or
  /// default.
  static Block &armBlock(Stmt &s, size_t k) {
    std::unique_ptr<Block> *arm = nullptr;
    if (auto *x = std::get_if<If>(&s)) {
      arm = k == 0 ? &x->Then : &x->Else;
    } else {
      auto &sw = std::get<Switch>(s);
      arm = k < sw.Cases.size() ? &sw.Cases[k].Body : &sw.Default;
    }
    if (!*arm)
      *arm = std::make_unique<Block>();
    return **arm;
  }

  /// Remove the null stores into moved slots that no path reads before the
  /// slot is written again or the function ends.
  bool dropDeadNullStores() {
    Analysis a(F);
    const Slots slots = moved();
    const size_t nodes = a.G.Nodes.size();
    std::vector<std::optional<size_t>> stored(nodes);
    bool any = false;
    for (uint32_t n = 0; n < nodes; ++n) {
      const Instr *in = a.G.instr(n);
      Slot s;
      if (in && in->Op == Opcode::Store && isNullBox(in->Args[0])) {
        s = {in->Local, 0};
      } else if (in && in->Op == Opcode::PtrStore && isNullBox(in->Args[1])) {
        const Address p = a.O.address(in->Args[0]);
        if (p.L == kNoLocal || !p.Offset)
          continue;
        s = {p.L, *p.Offset};
      } else {
        continue;
      }
      if (!Moved.count({s.L, s.Offset}))
        continue;
      stored[n] = slots.find(s);
      any = true;
    }
    if (!any)
      return false;
    // Whether some path from a point reads each moved slot before writing
    // all of it.
    std::vector<bool> read(nodes);
    Flow(a.G, slots.size(), /*must=*/false, [&](uint32_t n, Flow::State &s) {
      if (stored[n])
        read[n] = s.B.test(*stored[n]);
      reads(a, n, slots, s.B);
    }).backward(F.Body);
    Edits e;
    Remover r(a, e);
    bool changed = false;
    for (uint32_t n = 0; n < nodes; ++n)
      if (stored[n] && !read[n]) {
        r.remove(a.G.Nodes[n].At);
        changed = true;
      }
    e.apply(F.Body);
    return changed;
  }

  /// The step of the flow that finds the reads of moved slots (backward,
  /// may): given in @p read, for each of @p slots, whether some path from
  /// the point after node @p n reads the slot before writing all of it,
  /// make it the same for the point before.
  void reads(const Analysis &a, uint32_t n, const Slots &slots,
             Bits &read) const {
    switch (a.G.Nodes[n].K) {
    case Node::Test:
      return; // a test's value was read before: it reads no slot
    case Node::Return:
    case Node::Dead:
    case Node::Exit:
      read.fill(false); // nothing reads a slot after them
      return;
    case Node::Instruction:
      break;
    }
    const Instr &in = *a.G.instr(n);
    a.touched(n, slots, [&](size_t i) {
      switch (access(a, in, slots[i])) {
      case Access::None:
        break;
      case Access::Read:
        read.set(i, true);
        break;
      case Access::Written:
        read.set(i, false);
        break;
      }
    });
  }

  enum class Access : uint8_t { None, Read, Written };

  /// What @p in does to slot @p s: reads it, writes all of it, or neither.
  Access access(const Analysis &a, const Instr &in, Slot s) const {
    if (F.Locals[s.L].Ty == Type::Box) {
      if (in.Op == Opcode::Load && in.Local == s.L)
        return Access::Read;
      if (in.Op == Opcode::Store && in.Local == s.L)
        return Access::Written;
      return Access::None;
    }
    for (size_t j = 0; j < in.Args.size(); ++j) {
      const Operand &o = in.Args[j];
      // A reference loaded from the local was read when it was loaded.
      if (!a.O.from(o, s.L) || !mayAddress(a, o))
        continue;
      if (in.Op == Opcode::PtrAdd && j == 0)
        continue;
      int64_t at = 0;
      if (!a.O.offsetIn(o, s.L, at))
        return Access::Read;
      if (in.Op == Opcode::PtrLoad) {
        if (overlaps(at, storeSize(in.Result.Ty), s.Offset))
          return Access::Read;
        continue;
      }
      if (in.Op == Opcode::PtrStore && j == 0) {
        const int64_t size = storeSize(a.V.type(in.Args[1]));
        if (covers(at, size, s.Offset))
          return Access::Written;
        if (overlaps(at, size, s.Offset))
          return Access::Read;
        continue;
      }
      if ((in.Op == Opcode::MemCopy || in.Op == Opcode::MemZero) &&
          &o != &in.Args.back())
        if (std::optional<int64_t> len = constantI64(in.Args.back())) {
          const bool dst = j == 0;
          if (dst && covers(at, *len, s.Offset))
            return Access::Written;
          if (overlaps(at, *len, s.Offset))
            return Access::Read;
          continue;
        }
      return Access::Read;
    }
    return Access::None;
  }
};

class MovesAtLastUsePass : public Pass {
public:
  std::string_view name() const override { return "moves-at-last-use"; }
  Status run(Program &program, Statistics &stats) override {
    MoveCounts total;
    for (Module &m : program.Modules)
      for (Function &f : m.Functions) {
        if (f.IsExtern)
          continue;
        const MoveCounts c = moveAtLastUse(f);
        total.Transfers += c.Transfers;
        total.Retains += c.Retains;
        total.Releases += c.Releases;
      }
    stats.add(name(), "transfers", total.Transfers);
    stats.add(name(), "retains removed", total.Retains);
    stats.add(name(), "releases removed", total.Releases);
    return Status::ok();
  }
};

} // namespace

MoveCounts moveAtLastUse(Function &f) {
  if (f.IsExtern)
    return {};
  return Mover(f).run();
}

std::unique_ptr<Pass> createMovesAtLastUsePass() {
  return std::make_unique<MovesAtLastUsePass>();
}

} // namespace paykan::pir
