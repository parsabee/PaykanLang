// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The moves-at-last-use pass (see MoveAtLastUse.h; docs/pir.md §12 has the
// correctness argument).
//
// A function's structured body is first made a control-flow graph (Graph):
// a node per instruction, per test (an if's or a switch's value, a while's
// condition), per `ret` and per `unreachable`, linked as the statements run.
// With it come, for every value, where it is defined and which nodes use it
// (Values), and what it may refer to without a reference of its own
// (Origins): a box loaded from a local, the object unboxed from that box, a
// field of the object, an address into a `mem` local and what is read
// through it are all aliases of that local.
//
// A copy is moved when every path from it reaches the release of the
// slot it copied (a box local, or a reference held in a `mem` local at a
// constant offset) before anything else touches the slot or uses an alias
// of it, or ends in `unreachable`.  Moving removes the copy's retains and
// stores null into the slot; the cleanup that follows removes what that made
// useless.

#include "paykan/pir/MoveAtLastUse.h"

#include "Names.h"

#include <algorithm>
#include <cstddef>
#include <map>
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
    Join,        // where paths meet: a loop's head, an arm's end, a jump
  };
  Kind K = Join;
  /// The statement (Instruction, Test, Return, Dead).
  Place At;
  /// The operand a Test or a Return uses (none for a `ret` of nothing).
  const Operand *Use = nullptr;
  std::vector<uint32_t> Succ;
};

/// A function body's control-flow graph.
class Graph {
public:
  std::vector<Node> Nodes;
  uint32_t Entry = 0;
  /// The node of each instruction, `ret` and `unreachable`, and of each if's
  /// and switch's test.
  std::unordered_map<const Stmt *, uint32_t> NodeOf;
  /// For each if and switch: the node ending each of its arms, from which
  /// the statement after it runs, in the order of the arms (then, else; the
  /// cases, then the default).  An absent else or default is an arm too.
  std::unordered_map<const Stmt *, std::vector<uint32_t>> ArmEnds;

  explicit Graph(Function &f) { Entry = block(f.Body, add(Node::Exit)); }

  const Instr *instr(uint32_t n) const {
    return Nodes[n].K == Node::Instruction ? Nodes[n].At.instr() : nullptr;
  }
  /// The node after instruction node @p n.
  uint32_t next(uint32_t n) const { return Nodes[n].Succ.front(); }

private:
  struct Loop {
    uint32_t Head;
    uint32_t Exit;
  };
  std::vector<Loop> Loops;

  uint32_t add(Node::Kind k, Place at = {}, const Operand *use = nullptr,
               std::vector<uint32_t> succ = {}) {
    Node n;
    n.K = k;
    n.At = at;
    n.Use = use;
    n.Succ = std::move(succ);
    Nodes.push_back(std::move(n));
    return static_cast<uint32_t>(Nodes.size() - 1);
  }

  /// The entry of @p b, whose end goes on to @p next.
  uint32_t block(Block &b, uint32_t next) {
    for (size_t i = b.Stmts.size(); i-- > 0;)
      next = stmt(b, i, next);
    return next;
  }

  uint32_t stmt(Block &b, size_t i, uint32_t next) {
    Stmt &s = b.Stmts[i];
    const Place at{&b, i};
    uint32_t n = 0;
    if (std::holds_alternative<Instr>(s)) {
      n = add(Node::Instruction, at, nullptr, {next});
    } else if (auto *x = std::get_if<If>(&s)) {
      const uint32_t thenEnd = add(Node::Join, {}, nullptr, {next});
      const uint32_t elseEnd = add(Node::Join, {}, nullptr, {next});
      const uint32_t thenEntry = block(*x->Then, thenEnd);
      const uint32_t elseEntry = x->Else ? block(*x->Else, elseEnd) : elseEnd;
      ArmEnds[&s] = {thenEnd, elseEnd};
      n = add(Node::Test, at, &x->Cond, {thenEntry, elseEntry});
    } else if (auto *x = std::get_if<Switch>(&s)) {
      std::vector<uint32_t> ends;
      std::vector<uint32_t> entries;
      for (Switch::Case &c : x->Cases) {
        ends.push_back(add(Node::Join, {}, nullptr, {next}));
        entries.push_back(block(*c.Body, ends.back()));
      }
      ends.push_back(add(Node::Join, {}, nullptr, {next}));
      entries.push_back(x->Default ? block(*x->Default, ends.back())
                                   : ends.back());
      ArmEnds[&s] = std::move(ends);
      n = add(Node::Test, at, &x->Value, std::move(entries));
    } else if (auto *x = std::get_if<While>(&s)) {
      const uint32_t head = add(Node::Join);
      Loops.push_back({head, next});
      const uint32_t body = block(*x->Body, head);
      Loops.pop_back();
      const uint32_t test = add(Node::Test, at, &x->Cond, {body, next});
      Nodes[head].Succ = {block(*x->CondBlock, test)};
      return head;
    } else if (std::holds_alternative<Break>(s)) {
      return add(Node::Join, {}, nullptr, {Loops.back().Exit});
    } else if (std::holds_alternative<Continue>(s)) {
      return add(Node::Join, {}, nullptr, {Loops.back().Head});
    } else if (auto *x = std::get_if<Return>(&s)) {
      n = add(Node::Return, at, x->Value ? &*x->Value : nullptr);
    } else {
      n = add(Node::Dead, at);
    }
    NodeOf[&s] = n;
    return n;
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
/// reference of their own, and which locals cannot be moved from.
class Origins {
public:
  Origins(const Function &f, const Graph &g, const Values &v)
      : F(f), G(g), V(v), From(v.size()), Addr(v.size()), Held(f.Locals.size()),
        AddressTaken(f.Locals.size()), Escaped(f.Locals.size()) {
    // What a local holds depends on stores that may come later (a loop):
    // until nothing changes.
    for (bool changed = true; changed;) {
      changed = false;
      walk(f.Body, changed);
    }
  }

  /// Whether @p o may refer to local @p l, or into its storage.
  bool from(const Operand &o, LocalId l) const {
    if (!o.isValue() || o.valueId() >= From.size())
      return false;
    const std::vector<LocalId> &ls = From[o.valueId()];
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
  /// stored away.
  bool movable(LocalId l) const {
    if (Escaped[l])
      return false;
    return F.Locals[l].Ty == Type::Mem ||
           (F.Locals[l].Ty == Type::Box && !AddressTaken[l]);
  }

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
  const std::vector<LocalId> &fromOf(const Operand &o) const {
    static const std::vector<LocalId> none;
    return o.isValue() && o.valueId() < From.size() ? From[o.valueId()] : none;
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
      merge(from, fromOf(in.Args[0]));
      merge(from, fromOf(in.Args[1]));
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
      if (F.Locals[in.Local].Ty != Type::Box)
        changed |= merge(Held[in.Local], fromOf(in.Args[0]));
      return;
    case Opcode::PtrStore:
    case Opcode::FieldStore:
      // An alias stored in memory: what it refers to is no longer followed.
      if (!retained(in.Args[1]))
        for (LocalId l : fromOf(in.Args[1]))
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
          merge(from, fromOf(o));
      break;
    case Opcode::Cast:
      // A reference cast to an integer is still one.
      merge(from, fromOf(in.Args[0]));
      break;
    default:
      if (refersTo(in.Result.Ty))
        for (const Operand &o : in.Args)
          merge(from, fromOf(o));
      break;
    }
    if (in.Result.Ty == Type::Void)
      return;
    std::vector<LocalId> &mine = From[in.Result.Id];
    changed |= merge(mine, from);
    Addr[in.Result.Id] = addr;
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

/// The copy of a box local being examined: the retain that makes it, the
/// box it retains and the statement that takes the box.
struct Copy {
  Place Retain;
  ValueId Moved = kNoValue;
  const Stmt *Consumer = nullptr;
};

/// A copy moved: its retains removed, null stored into the slot's
/// references before Before.
struct Transfer {
  Slot S;
  /// The references moved (offsets in the slot's local; {0} for a box).
  std::vector<int64_t> Parts;
  std::vector<Place> Retains;
  Place Before;
  /// A mem local's address the null stores go through (its copy's source).
  Operand Base;
};

/// One analysis of a function: its graph and what its values refer to.
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

  /// Whether @p o, used by node @p n, is a copy with a reference of its own
  /// made before @p before: a value whose other use is a retain ahead of
  /// @p before in its block (`f(a, a)`: the first argument).
  bool ownCopy(const Operand &o, uint32_t n, Place before) const {
    if (!o.isValue())
      return false;
    const std::vector<uint32_t> &users = V.users(o.valueId());
    if (users.size() != 2 || (users[0] == n) == (users[1] == n))
      return false;
    const uint32_t other = users[0] == n ? users[1] : users[0];
    const Instr *in = G.instr(other);
    const Place at = G.Nodes[other].At;
    return in && in->Op == Opcode::Retain && at.B == before.B &&
           at.Index < before.Index;
  }

  /// What node @p n does to slot @p s.  For a box local's copy @p c, its
  /// consumer may use the moved box, and earlier copies of the slot are
  /// references of their own.
  Event event(uint32_t n, Slot s, const Copy &c) const {
    const Node &node = G.Nodes[n];
    switch (node.K) {
    case Node::Join:
      return Event::None;
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
        if (O.from(o, s.L) &&
            !(&node.At.stmt() == c.Consumer && sameValue(o, c.Moved)) &&
            !ownCopy(o, n, c.Retain))
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

  /// Whether every path from node @p start ends in a release of slot @p s
  /// (or in `unreachable`) before any other event on it.
  bool releasedOnEveryPath(uint32_t start, Slot s, const Copy &c) const {
    std::vector<bool> seen(G.Nodes.size());
    std::vector<uint32_t> work{start};
    while (!work.empty()) {
      const uint32_t n = work.back();
      work.pop_back();
      if (seen[n])
        continue;
      seen[n] = true;
      switch (event(n, s, c)) {
      case Event::Bad:
        return false;
      case Event::Release:
        break;
      case Event::None:
        for (uint32_t succ : G.Nodes[n].Succ)
          work.push_back(succ);
        break;
      }
    }
    return true;
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
  /// and `%v` handed to its consumer, all in one block.
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
    if (!releasedOnEveryPath(G.next(r), {x, 0}, Copy{at, v, &use.stmt()}))
      return std::nullopt;
    Transfer t;
    t.S = {x, 0};
    t.Parts = {0};
    t.Retains = {at};
    t.Before = {at.B, at.Index + 1};
    return t;
  }

  /// Whether node @p c takes the reference %v as its own: a call's
  /// argument (the callee releases it), a store into a local, a field (not
  /// a closure's capture) or memory, or the value returned.
  bool consumes(uint32_t c, ValueId v) const {
    const Node &node = G.Nodes[c];
    if (node.K == Node::Return)
      return node.Use && sameValue(*node.Use, v);
    const Instr *in = G.instr(c);
    if (!in)
      return false;
    switch (in->Op) {
    case Opcode::Call:
    case Opcode::Store:
      return true;
    case Opcode::VCall:
    case Opcode::ICall:
      return !sameValue(in->Args[0], v);
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

  /// A whole mem local's copy at the `mem.copy` node @p n.
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
        const Instr *in = std::get_if<Instr>(&at.B->Stmts[i]);
        if (!in || (in->Result.Ty != Type::Void && group.count(in->Result.Id)))
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
    const uint32_t lastNode = G.NodeOf.at(&last.stmt());
    for (const auto &[k, p] : retains) {
      if (!releasedOnEveryPath(G.next(lastNode), {m, k}, Copy{}))
        return std::nullopt;
      t.Parts.push_back(k);
      t.Retains.push_back(p);
    }
    t.Before = {last.B, last.Index + 1};
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
    if (!transfer())
      return Counts;
    // Each round of the cleanup may make more of it possible (a release
    // moved into an arm leaves the null stores of the others unread).
    for (bool changed = true; changed;) {
      changed = dropNullReleases();
      changed = dropDeadNullStores() || changed;
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
    std::vector<Transfer> found;
    std::unordered_set<const Stmt *> claimed;
    for (uint32_t n = 0; n < a.G.Nodes.size(); ++n) {
      const Instr *in = a.G.instr(n);
      std::optional<Transfer> t;
      if (in && in->Op == Opcode::Retain)
        t = a.boxCopy(n);
      else if (in && in->Op == Opcode::MemCopy)
        t = a.memCopy(n);
      // A retain belongs to one copy.
      if (!t || std::any_of(t->Retains.begin(), t->Retains.end(),
                            [&](const Place &p) {
                              return claimed.count(&p.stmt()) > 0;
                            }))
        continue;
      for (const Place &p : t->Retains)
        claimed.insert(&p.stmt());
      found.push_back(std::move(*t));
    }
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

  /// The moved slot node @p n releases (a release load), if it is one.
  std::optional<std::pair<LocalId, int64_t>> releasedSlot(const Analysis &a,
                                                          uint32_t n) const {
    const Instr *in = a.G.instr(n);
    if (!in || !a.releaseLoad(a.G.Nodes[n].At))
      return std::nullopt;
    std::pair<LocalId, int64_t> s;
    if (in->Op == Opcode::Load) {
      s = {in->Local, 0};
    } else {
      const Address p = a.O.address(in->Args[0]);
      if (p.L == kNoLocal || !p.Offset)
        return std::nullopt;
      s = {p.L, *p.Offset};
    }
    if (!Moved.count(s))
      return std::nullopt;
    return s;
  }

  /// The state of every moved slot before each node: whether it is null on
  /// every path there.  Unreached nodes have no state.
  struct NullStates {
    std::vector<std::vector<char>> In;
    std::vector<bool> Reached;
  };

  NullStates
  nullStates(const Analysis &a,
             const std::vector<std::pair<LocalId, int64_t>> &slots) const {
    NullStates st;
    const size_t nodes = a.G.Nodes.size();
    st.In.assign(nodes, {});
    st.Reached.assign(nodes, false);
    st.In[a.G.Entry].assign(slots.size(), 0);
    st.Reached[a.G.Entry] = true;
    std::vector<uint32_t> work{a.G.Entry};
    while (!work.empty()) {
      const uint32_t n = work.back();
      work.pop_back();
      std::vector<char> out = st.In[n];
      step(a, n, slots, out);
      for (uint32_t s : a.G.Nodes[n].Succ) {
        if (!st.Reached[s]) {
          st.Reached[s] = true;
          st.In[s] = out;
          work.push_back(s);
          continue;
        }
        bool changed = false;
        for (size_t i = 0; i < out.size(); ++i)
          if (st.In[s][i] && !out[i]) {
            st.In[s][i] = 0;
            changed = true;
          }
        if (changed)
          work.push_back(s);
      }
    }
    return st;
  }

  /// Whether @p o may be an address (a `ptr`, or an integer one was cast
  /// to), not a reference.
  static bool mayAddress(const Analysis &a, const Operand &o) {
    const Type t = a.V.type(o);
    return t != Type::Box && t != Type::Obj && t != Type::Weak;
  }

  /// What instruction node @p n does to whether each moved slot is null.
  void step(const Analysis &a, uint32_t n,
            const std::vector<std::pair<LocalId, int64_t>> &slots,
            std::vector<char> &state) const {
    const Instr *in = a.G.instr(n);
    if (!in)
      return;
    for (size_t i = 0; i < slots.size(); ++i) {
      const auto &[l, k] = slots[i];
      if (F.Locals[l].Ty == Type::Box) {
        if (in->Op == Opcode::Store && in->Local == l)
          state[i] = isNullBox(in->Args[0]);
        continue;
      }
      for (size_t j = 0; j < in->Args.size(); ++j) {
        const Operand &o = in->Args[j];
        // What a reference loaded from the local refers to is not the
        // local: only an address writes it.
        if (!a.O.from(o, l) || !mayAddress(a, o))
          continue;
        int64_t at = 0;
        const bool known = a.O.offsetIn(o, l, at);
        if ((in->Op == Opcode::PtrAdd && j == 0) || in->Op == Opcode::PtrLoad)
          continue; // computes an address, or reads
        if (in->Op == Opcode::MemCopy && j == 1)
          continue; // the source, read
        if (in->Op == Opcode::PtrStore && j == 0 && known) {
          const int64_t size = storeSize(a.V.type(in->Args[1]));
          if (at == k && isNullBox(in->Args[1]))
            state[i] = 1;
          else if (overlaps(at, size, k))
            state[i] = 0;
          continue;
        }
        if (in->Op == Opcode::MemZero && j == 0 && known)
          if (std::optional<int64_t> len = constantI64(in->Args[1])) {
            if (covers(at, *len, k))
              state[i] = 1;
            else if (overlaps(at, *len, k))
              state[i] = 0;
            continue;
          }
        if (in->Op == Opcode::MemCopy && j == 0 && known)
          if (std::optional<int64_t> len = constantI64(in->Args[2])) {
            if (overlaps(at, *len, k))
              state[i] = 0;
            continue;
          }
        // Anything else may write through the address.
        state[i] = 0;
      }
    }
  }

  /// Remove the releases of moved slots that are null on every path to
  /// them, and move a release that follows an if or a switch into its one
  /// arm in which the slot may hold a reference.
  bool dropNullReleases() {
    Analysis a(F);
    std::vector<std::pair<LocalId, int64_t>> slots(Moved.begin(), Moved.end());
    std::map<std::pair<LocalId, int64_t>, size_t> index;
    for (size_t i = 0; i < slots.size(); ++i)
      index[slots[i]] = i;
    const NullStates st = nullStates(a, slots);
    Edits e;
    Remover r(a, e);
    bool changed = false;
    for (uint32_t n = 0; n < a.G.Nodes.size(); ++n) {
      const auto s = releasedSlot(a, n);
      if (!s || !st.Reached[n] || !st.In[n][index.at(*s)])
        continue;
      const Place at = a.G.Nodes[n].At;
      r.remove({at.B, at.Index + 1});
      ++Counts.Releases;
      changed = true;
    }
    changed |= sinkReleases(a, F.Body, st, index, e);
    e.apply(F.Body);
    return changed;
  }

  /// The balancing releases of @p b and the blocks in it (see
  /// dropNullReleases).
  bool sinkReleases(const Analysis &a, Block &b, const NullStates &st,
                    const std::map<std::pair<LocalId, int64_t>, size_t> &index,
                    Edits &e) {
    bool changed = false;
    for (size_t i = 0; i < b.Stmts.size(); ++i) {
      Stmt &s = b.Stmts[i];
      if (auto *x = std::get_if<If>(&s)) {
        changed |= sinkReleases(a, *x->Then, st, index, e);
        if (x->Else)
          changed |= sinkReleases(a, *x->Else, st, index, e);
      } else if (auto *x = std::get_if<While>(&s)) {
        changed |= sinkReleases(a, *x->CondBlock, st, index, e);
        changed |= sinkReleases(a, *x->Body, st, index, e);
        continue;
      } else if (auto *x = std::get_if<Switch>(&s)) {
        for (Switch::Case &c : x->Cases)
          changed |= sinkReleases(a, *c.Body, st, index, e);
        if (x->Default)
          changed |= sinkReleases(a, *x->Default, st, index, e);
      } else {
        continue;
      }
      // The release right after it: only loads of other slots, which no
      // release changes, may come first.
      size_t j = i + 1;
      std::optional<std::pair<LocalId, int64_t>> slot;
      std::vector<LocalId> passed;
      for (; j < b.Stmts.size(); ++j) {
        const auto *in = std::get_if<Instr>(&b.Stmts[j]);
        if (!in || in->Op != Opcode::Load)
          break;
        if (a.releaseLoad({&b, j})) {
          slot = releasedSlot(a, a.G.NodeOf.at(&b.Stmts[j]));
          break;
        }
        passed.push_back(in->Local);
      }
      if (!slot || F.Locals[slot->first].Ty != Type::Box ||
          e.removed(b.Stmts[j + 1]) ||
          std::find(passed.begin(), passed.end(), slot->first) != passed.end())
        continue;
      const size_t bit = index.at(*slot);
      const std::vector<uint32_t> &ends = a.G.ArmEnds.at(&s);
      std::vector<size_t> holding;
      size_t through = 0;
      for (size_t k = 0; k < ends.size(); ++k) {
        if (!st.Reached[ends[k]])
          continue;
        ++through;
        if (!st.In[ends[k]][bit])
          holding.push_back(k);
      }
      if (through < 2 || holding.size() != 1)
        continue;
      Block &arm = armBlock(s, holding.front());
      e.insert(arm, arm.Stmts.size(), std::get<Instr>(b.Stmts[j]));
      e.insert(arm, arm.Stmts.size(), std::get<Instr>(b.Stmts[j + 1]));
      e.remove(b.Stmts[j]);
      e.remove(b.Stmts[j + 1]);
      changed = true;
    }
    return changed;
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
    Edits e;
    Remover r(a, e);
    bool changed = false;
    for (uint32_t n = 0; n < a.G.Nodes.size(); ++n) {
      const Instr *in = a.G.instr(n);
      std::pair<LocalId, int64_t> s;
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
      if (!Moved.count(s) || read(a, a.G.next(n), {s.first, s.second}))
        continue;
      r.remove(a.G.Nodes[n].At);
      changed = true;
    }
    e.apply(F.Body);
    return changed;
  }

  /// Whether some path from node @p start reads slot @p s before writing
  /// all of it.
  bool read(const Analysis &a, uint32_t start, Slot s) const {
    std::vector<bool> seen(a.G.Nodes.size());
    std::vector<uint32_t> work{start};
    while (!work.empty()) {
      const uint32_t n = work.back();
      work.pop_back();
      if (seen[n])
        continue;
      seen[n] = true;
      switch (access(a, n, s)) {
      case Access::Read:
        return true;
      case Access::Written:
        break;
      case Access::None:
        for (uint32_t succ : a.G.Nodes[n].Succ)
          work.push_back(succ);
        break;
      }
    }
    return false;
  }

  enum class Access : uint8_t { None, Read, Written };

  Access access(const Analysis &a, uint32_t n, Slot s) const {
    // A test's or a `ret`'s value was read before: neither reads the slot.
    const Node &node = a.G.Nodes[n];
    if (node.K != Node::Instruction)
      return node.K == Node::Join || node.K == Node::Test ? Access::None
                                                          : Access::Written;
    const Instr &in = *node.At.instr();
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
