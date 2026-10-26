// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// Binary PIR codec (docs/design/pkm.md §5) and symbol index (§6.1).  The
// writer walks the module's vectors in order and interns strings on first
// use, so one module has one encoding; the reader is one bounds-checked
// pass over the same layout, with reserved codes and bits, slack bytes and
// non-canonical encodings (an empty "has module" string, a zero value id)
// rejected so that encode(decode(b)) == b for every accepted blob.

#include "paykan/pir/Binary.h"

#include "paykan/pir/Codes.h"
#include "paykan/pir/Version.h"
#include "paykan/support/Bytes.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace paykan::pir::binary {

namespace {

using support::ByteReader;
using support::ByteWriter;

constexpr uint8_t kMagic[4] = {'P', 'I', 'R', 'B'};
constexpr uint8_t kTrailer[4] = {'B', 'R', 'I', 'P'};
constexpr uint8_t kIndexMagic[4] = {'P', 'K', 'S', 'Y'};
constexpr uint8_t kIndexTrailer[4] = {'Y', 'S', 'K', 'P'};

// The flag bits a codec 1.0 reader understands (§5.2, §5.7); every other
// bit is reserved for a later ticket and must be zero.
constexpr uint32_t kKnownModuleFlags = kFlagNamesStripped | kFlagDefinesMain;
constexpr uint32_t kFnExtern = 1u << 0, kFnHasModule = 1u << 1,
                   kFnHasSymbol = 1u << 2, kFnShared = 1u << 3,
                   kFnHasUses = 1u << 4;
constexpr uint32_t kKnownFnFlags =
    kFnExtern | kFnHasModule | kFnHasSymbol | kFnShared | kFnHasUses;
constexpr uint32_t kClassExtern = 1u << 0, kClassShared = 1u << 1;
constexpr uint32_t kKnownClassFlags = kClassExtern | kClassShared;

enum OperandKind : uint8_t {
  kOpValue,
  kOpI64,
  kOpF64,
  kOpBool,
  kOpChar,
  kOpNullBox,
  kOpNullObj,
  kOpSymbol
};
enum StmtTag : uint8_t {
  kStInstr,
  kStIf,
  kStWhile,
  kStBreak,
  kStContinue,
  kStRet,
  kStUnreachable,
  kStSwitch
};
// Item tables in §5.2 order, and the least bytes one record of each takes.
enum Table : size_t {
  kExternGlobals,
  kCStrs,
  kDatas,
  kBytes,
  kSlots,
  kClassObjs,
  kClasses,
  kFunctions
};
constexpr size_t kTableCount = 8;
constexpr size_t kMinRecordBytes[kTableCount] = {2, 2, 2, 2, 1, 1, 8, 8};
constexpr uint8_t kMaxSymbolKind = 9;
constexpr uint8_t kMaxLinkage = 2;
constexpr size_t kMinIndexEntryBytes = 5 + 32;

constexpr uint8_t predCode(CmpPred p) {
  switch (p) {
  case CmpPred::Eq:
    return 0;
  case CmpPred::Ne:
    return 1;
  case CmpPred::Lt:
    return 2;
  case CmpPred::Le:
    return 3;
  case CmpPred::Gt:
    return 4;
  case CmpPred::Ge:
    return 5;
  }
  return 0;
}
constexpr bool predFromCode(uint8_t c, CmpPred &out) {
  constexpr CmpPred kPreds[] = {CmpPred::Eq, CmpPred::Ne, CmpPred::Lt,
                                CmpPred::Le, CmpPred::Gt, CmpPred::Ge};
  if (c >= std::size(kPreds))
    return false;
  out = kPreds[c];
  return true;
}

bool definesMain(const Module &m) {
  for (const Function &f : m.Functions)
    if (m.defines(f) && f.Name == "main")
      return true;
  return false;
}

bool magicIs(ByteReader &r, const uint8_t (&magic)[4], const char *what) {
  std::span<const uint8_t> got;
  if (!r.bytes(4, got))
    return false;
  if (!std::equal(got.begin(), got.end(), magic))
    return r.fail(std::string("bad ") + what, r.offset() - 4);
  return true;
}

Status readerError(const ByteReader &r, size_t base = 0) {
  return Status::error("binary PIR: " + r.error() + " at byte " +
                       std::to_string(base + r.errorOffset()));
}

// -- Encoder

class Encoder {
public:
  explicit Encoder(bool strip) : Strip(strip) {}

  std::vector<uint8_t> run(const Module &m) {
    str(m.Name);
    W.uleb(m.Externs.size());
    for (const ExternGlobal &g : m.Externs) {
      str(g.Name);
      W.u8(g.K == ExternGlobal::Object ? 0 : 1);
    }
    W.uleb(m.CStrs.size());
    for (const CStrGlobal &g : m.CStrs) {
      str(g.Name);
      W.str(g.Data);
    }
    W.uleb(m.Datas.size());
    for (const DataGlobal &g : m.Datas) {
      str(g.Name);
      W.uleb(g.Words.size());
      for (int64_t w : g.Words)
        W.sleb(w);
    }
    W.uleb(m.Bytes.size());
    for (const BytesGlobal &g : m.Bytes) {
      str(g.Name);
      W.uleb(g.Bytes.size());
      W.bytes(g.Bytes);
    }
    W.uleb(0); // SlotRec (#170)
    W.uleb(0); // ClassObjRec: reserved, always empty (no #160 erasure)
    W.uleb(m.Classes.size());
    for (const Class &c : m.Classes)
      klass(c);
    W.uleb(m.Functions.size());
    for (const Function &f : m.Functions)
      function(f);
    W.bytes(kTrailer, 4);
    std::vector<uint8_t> body = W.take();

    // The string table precedes the body, so the body is written first.
    ByteWriter out;
    out.bytes(kMagic, 4);
    out.u32(kCodecVersion);
    out.u32(kPIRVersion);
    out.u32((Strip ? kFlagNamesStripped : 0) |
            (definesMain(m) ? kFlagDefinesMain : 0));
    Strings.write(out);
    out.bytes(body);
    return out.take();
  }

private:
  ByteWriter W;
  support::StringTable Strings;
  bool Strip;

  void str(std::string_view s) { W.uleb(Strings.intern(s)); }
  /// A value or local name: dropped under --strip-names.
  void name(std::string_view s) { str(Strip ? std::string_view() : s); }
  void type(Type t) { W.u8(typeEntry(t).Code); }
  /// A local's or field's type; a `mem` slot adds its size and alignment.
  void slotType(Type t, int64_t size, int64_t align) {
    type(t);
    if (t == Type::Mem) {
      W.uleb(static_cast<uint64_t>(size));
      W.uleb(static_cast<uint64_t>(align));
    }
  }
  void sig(const Signature &s) {
    W.uleb(s.Params.size());
    for (Type t : s.Params)
      type(t);
    type(s.Ret);
  }

  void operand(const Operand &op) {
    struct Visitor {
      Encoder &E;
      void operator()(ValueId id) const {
        E.W.u8(kOpValue);
        E.W.uleb(id);
      }
      void operator()(int64_t v) const {
        E.W.u8(kOpI64);
        E.W.sleb(v);
      }
      void operator()(double v) const {
        E.W.u8(kOpF64);
        E.W.f64(v);
      }
      void operator()(bool v) const {
        E.W.u8(kOpBool);
        E.W.u8(v ? 1 : 0);
      }
      void operator()(char v) const {
        E.W.u8(kOpChar);
        E.W.u8(static_cast<uint8_t>(v));
      }
      void operator()(const Operand::Null &n) const {
        E.W.u8(n.Ty == Type::Box ? kOpNullBox : kOpNullObj);
      }
      void operator()(const SymbolRef &s) const {
        E.W.u8(kOpSymbol);
        E.str(s.Name);
      }
    };
    std::visit(Visitor{*this}, op.V);
  }

  void instr(const Instr &in) {
    // A stable load's opcode follows its mark, a `retain lend`'s its own,
    // and a call's with an argument that is not owned the ownership of its
    // arguments that are not (§5.5).
    if (in.Stable)
      W.u8(kStableLoadCode);
    if (in.Lend)
      W.u8(kLendCode);
    ownership(in);
    W.u8(opcodeEntry(in.Op).Code);
    W.uleb(in.Result.Id);
    if (in.Result.Id != kNoValue) {
      type(in.Result.Ty);
      name(in.Result.Name);
    }
    // Only the extras the opcode uses (§5.5): the rest stay at their
    // defaults on both sides.
    switch (in.Op) {
    case Opcode::Cmp:
    case Opcode::UCmp:
      W.u8(predCode(in.Pred));
      break;
    case Opcode::Wrap:
    case Opcode::Mask:
      W.uleb(in.Slot);
      break;
    case Opcode::Cast:
      type(in.CastTo);
      break;
    case Opcode::Call:
    case Opcode::FnAddr:
      str(in.Callee);
      break;
    case Opcode::VCall:
      str(in.ClassName);
      W.uleb(in.Slot);
      sig(in.Sig);
      break;
    case Opcode::ICall:
      sig(in.Sig);
      break;
    case Opcode::New:
    case Opcode::VTableAddr:
    case Opcode::VTableStore:
      str(in.ClassName);
      break;
    case Opcode::FieldLoad:
    case Opcode::FieldStore:
    case Opcode::FieldAddr:
      str(in.ClassName);
      str(in.Field);
      break;
    case Opcode::Load:
    case Opcode::Store:
    case Opcode::LocalAddr:
      W.uleb(in.Local);
      break;
    default:
      break;
    }
    W.uleb(in.Args.size());
    for (const Operand &a : in.Args)
      operand(a);
  }

  /// `u8 55 ; uleb n ; n × (uleb argIndex ; u8 kind ; [kind 2] Operand)`:
  /// the arguments that are lent (kind 1) or passed (kind 2, its bit
  /// after it) in order, when there is one.
  void ownership(const Instr &in) {
    size_t n = 0;
    for (const ArgOwnership &o : in.Own)
      if (o.K != ArgOwnership::Owned)
        ++n;
    if (n == 0)
      return;
    W.u8(kOwnershipCode);
    W.uleb(n);
    for (size_t i = 0; i < in.Own.size(); ++i) {
      const ArgOwnership &o = in.Own[i];
      if (o.K == ArgOwnership::Owned)
        continue;
      W.uleb(i);
      W.u8(o.K == ArgOwnership::Lent ? 1 : 2);
      if (o.K == ArgOwnership::Passed)
        operand(o.Bit);
    }
  }

  void block(const Block *b) {
    if (!b) {
      W.uleb(0);
      return;
    }
    W.uleb(b->Stmts.size());
    for (const Stmt &s : b->Stmts)
      stmt(s);
  }

  void stmt(const Stmt &s) {
    struct Visitor {
      Encoder &E;
      void operator()(const Instr &in) const {
        E.W.u8(kStInstr);
        E.instr(in);
      }
      void operator()(const If &i) const {
        E.W.u8(kStIf);
        E.operand(i.Cond);
        E.block(i.Then.get());
        E.W.u8(i.Else ? 1 : 0);
        if (i.Else)
          E.block(i.Else.get());
      }
      void operator()(const While &w) const {
        E.W.u8(kStWhile);
        E.block(w.CondBlock.get());
        E.operand(w.Cond);
        E.block(w.Body.get());
      }
      void operator()(const Break &) const { E.W.u8(kStBreak); }
      void operator()(const Continue &) const { E.W.u8(kStContinue); }
      void operator()(const Return &r) const {
        E.W.u8(kStRet);
        E.W.u8(r.Value ? 1 : 0);
        if (r.Value)
          E.operand(*r.Value);
      }
      void operator()(const Unreachable &) const { E.W.u8(kStUnreachable); }
      void operator()(const Switch &sw) const {
        E.W.u8(kStSwitch);
        E.operand(sw.Value);
        E.W.uleb(sw.Cases.size());
        for (const Switch::Case &c : sw.Cases) {
          E.operand(c.Value);
          E.block(c.Body.get());
        }
        E.W.u8(sw.Default ? 1 : 0);
        if (sw.Default)
          E.block(sw.Default.get());
      }
    };
    std::visit(Visitor{*this}, s);
  }

  /// A length-prefixed FunctionRec (§5.7): the record is written to a
  /// scratch writer first because its length comes before it.
  void function(const Function &f) {
    ByteWriter outer = std::move(W);
    W = ByteWriter();
    str(f.Name);
    W.u32((f.IsExtern ? kFnExtern : 0) | (f.Module.empty() ? 0 : kFnHasModule) |
          (f.Symbol.empty() ? 0 : kFnHasSymbol) | (f.Shared ? kFnShared : 0) |
          (f.Uses.empty() ? 0 : kFnHasUses));
    sig(f.Sig);
    if (!f.Module.empty())
      str(f.Module);
    if (!f.Symbol.empty())
      str(f.Symbol);
    if (!f.Uses.empty())
      uses(f.Uses);
    if (!f.IsExtern) {
      W.uleb(0); // nConventions (#98)
      W.uleb(f.Params.size());
      for (const Value &p : f.Params) {
        W.uleb(p.Id);
        type(p.Ty);
        name(p.Name);
      }
      W.uleb(f.Locals.size());
      for (const Local &l : f.Locals) {
        name(l.Name);
        if (l.View)
          W.u8(kViewSlotCode);
        slotType(l.Ty, l.Size, l.Align);
      }
      W.uleb(f.NextValueId);
      block(&f.Body);
    }
    std::vector<uint8_t> rec = W.take();
    W = std::move(outer);
    W.uleb(rec.size());
    W.bytes(rec);
  }

  /// `uleb n, n × str`.
  void uses(const std::vector<std::string> &names) {
    W.uleb(names.size());
    for (const std::string &n : names)
      str(n);
  }

  void klass(const Class &c) {
    str(c.Name);
    W.u32((c.IsExtern ? kClassExtern : 0) | (c.Shared ? kClassShared : 0));
    str(c.Super);
    if (c.IsExtern)
      str(c.Module);
    W.uleb(c.Fields.size());
    for (const Field &f : c.Fields) {
      str(f.Name);
      slotType(f.Ty, f.Size, f.Align);
    }
    W.uleb(c.VTable.size());
    for (const VTableEntry &e : c.VTable) {
      str(e.Slot);
      str(e.Target);
      sig(e.Sig);
    }
  }
};

// -- Decoder

/// Reads records from one ByteReader; every failure is recorded in the
/// reader, which the caller turns into a Status.
class Decoder {
public:
  Decoder(ByteReader &r, const std::vector<std::string> &strings,
          const DecodeOptions &opts, size_t &stmtBudget)
      : R(r), Strings(strings), Opts(opts), StmtBudget(stmtBudget) {}

  bool str(std::string &out) {
    uint64_t i;
    if (!R.uleb(i))
      return false;
    if (i >= Strings.size())
      return R.fail("string index out of range", R.offset() - 1);
    out = Strings[static_cast<size_t>(i)];
    return true;
  }

  bool externGlobal(ExternGlobal &g) {
    uint8_t k;
    if (!str(g.Name) || !R.u8(k))
      return false;
    if (k > 1)
      return R.fail("reserved extern global kind", R.offset() - 1);
    g.K = k ? ExternGlobal::VTable : ExternGlobal::Object;
    return true;
  }
  bool cstr(CStrGlobal &g) { return str(g.Name) && R.str(g.Data); }
  bool data(DataGlobal &g) {
    uint64_t n;
    if (!str(g.Name) || !R.count(n))
      return false;
    g.Words.resize(static_cast<size_t>(n));
    for (int64_t &w : g.Words)
      if (!R.sleb(w))
        return false;
    return true;
  }
  bool bytes(BytesGlobal &g) {
    uint64_t n;
    std::span<const uint8_t> b;
    if (!str(g.Name) || !R.count(n) || !R.bytes(static_cast<size_t>(n), b))
      return false;
    g.Bytes.assign(b.begin(), b.end());
    return true;
  }

  /// `uleb n, n × str`, at least one.
  bool uses(std::vector<std::string> &names) {
    uint64_t n;
    if (!R.count(n))
      return false;
    if (n == 0)
      return R.fail("an empty uses list is written as none");
    names.resize(static_cast<size_t>(n));
    for (std::string &name : names)
      if (!nonEmpty(name, "use"))
        return false;
    return true;
  }

  bool klass(Class &c) {
    uint32_t flags;
    uint64_t n;
    if (!str(c.Name) || !R.u32(flags))
      return false;
    if (flags & ~kKnownClassFlags)
      return R.fail("reserved class flag", R.offset() - 4);
    c.IsExtern = (flags & kClassExtern) != 0;
    c.Shared = (flags & kClassShared) != 0;
    if (!str(c.Super) || (c.IsExtern && !str(c.Module)))
      return false;
    if (!R.count(n, 2))
      return false;
    c.Fields.resize(static_cast<size_t>(n));
    for (Field &f : c.Fields)
      if (!str(f.Name) || !slotType(f.Ty, f.Size, f.Align))
        return false;
    if (!R.count(n, 3))
      return false;
    c.VTable.resize(static_cast<size_t>(n));
    for (VTableEntry &e : c.VTable)
      if (!str(e.Slot) || !str(e.Target) || !sig(e.Sig))
        return false;
    return true;
  }

  /// The FunctionRec after its length prefix; with @p bodies false only the
  /// name and flags are read (what inspect() needs).
  bool function(Function &f, bool bodies) {
    uint32_t flags;
    if (!str(f.Name) || !R.u32(flags))
      return false;
    if (flags & ~kKnownFnFlags)
      return R.fail("reserved function flag", R.offset() - 4);
    f.IsExtern = (flags & kFnExtern) != 0;
    f.Shared = (flags & kFnShared) != 0;
    if (!bodies)
      return true;
    if (!sig(f.Sig))
      return false;
    if ((flags & kFnHasModule) && !nonEmpty(f.Module, "module"))
      return false;
    if ((flags & kFnHasSymbol) && !nonEmpty(f.Symbol, "symbol"))
      return false;
    if ((flags & kFnHasUses) && !uses(f.Uses))
      return false;
    if (f.IsExtern)
      return true;
    uint64_t n;
    if (!R.count(n))
      return false;
    if (n != 0)
      return R.fail("parameter conventions are not supported");
    if (!R.count(n, 3))
      return false;
    if (n != f.Sig.Params.size())
      return R.fail("parameter count disagrees with the signature");
    f.Params.resize(static_cast<size_t>(n));
    for (size_t i = 0; i < f.Params.size(); ++i) {
      Value &p = f.Params[i];
      if (!R.uleb32(p.Id) || !type(p.Ty) || !str(p.Name))
        return false;
      if (p.Ty != f.Sig.Params[i])
        return R.fail("parameter type disagrees with the signature");
    }
    if (!R.count(n, 2))
      return false;
    f.Locals.resize(static_cast<size_t>(n));
    for (Local &l : f.Locals) {
      uint8_t c;
      if (!str(l.Name) || !R.u8(c))
        return false;
      // A `view` local's slot type follows its mark (§5.4).
      l.View = c == kViewSlotCode;
      if ((l.View && !R.u8(c)) || !slotType(c, l.Ty, l.Size, l.Align))
        return false;
    }
    return R.uleb32(f.NextValueId) && block(f.Body);
  }

private:
  ByteReader &R;
  const std::vector<std::string> &Strings;
  const DecodeOptions &Opts;
  size_t &StmtBudget;
  uint32_t Depth = 0;

  /// A string the writer only emits when non-empty.
  bool nonEmpty(std::string &out, const char *what) {
    if (!str(out))
      return false;
    if (out.empty())
      return R.fail(std::string("empty ") + what + " name");
    return true;
  }
  bool type(Type &t) {
    uint8_t c;
    if (!R.u8(c))
      return false;
    if (!typeFromCode(c, t))
      return R.fail("reserved type code", R.offset() - 1);
    if (t == Type::Mem)
      return R.fail("mem is only the type of a local or a field",
                    R.offset() - 1);
    return true;
  }
  /// A local's or field's type: a type, or `mem` and its size and alignment.
  bool slotType(Type &t, int64_t &size, int64_t &align) {
    uint8_t c;
    return R.u8(c) && slotType(c, t, size, align);
  }
  /// The same, its code @p c read.
  bool slotType(uint8_t c, Type &t, int64_t &size, int64_t &align) {
    if (!typeFromCode(c, t))
      return R.fail("reserved type code", R.offset() - 1);
    if (t != Type::Mem)
      return true;
    uint64_t sz, al;
    if (!R.uleb(sz, INT64_MAX) || !R.uleb(al, INT64_MAX))
      return false;
    size = static_cast<int64_t>(sz);
    align = static_cast<int64_t>(al);
    return true;
  }
  bool sig(Signature &s) {
    uint64_t n;
    if (!R.count(n))
      return false;
    s.Params.resize(static_cast<size_t>(n));
    for (Type &t : s.Params)
      if (!type(t))
        return false;
    return type(s.Ret);
  }

  bool operand(Operand &op) {
    uint8_t kind;
    if (!R.u8(kind))
      return false;
    switch (kind) {
    case kOpValue: {
      uint32_t id;
      if (!R.uleb32(id))
        return false;
      if (id == kNoValue)
        return R.fail("operand names value 0");
      op = Operand::value(id);
      return true;
    }
    case kOpI64: {
      int64_t v;
      if (!R.sleb(v))
        return false;
      op = Operand::i64(v);
      return true;
    }
    case kOpF64: {
      double v;
      if (!R.f64(v))
        return false;
      op = Operand::f64(v);
      return true;
    }
    case kOpBool: {
      uint8_t v;
      if (!flag(v))
        return false;
      op = Operand::boolean(v != 0);
      return true;
    }
    case kOpChar: {
      uint8_t v;
      if (!R.u8(v))
        return false;
      op = Operand::chr(static_cast<char>(v));
      return true;
    }
    case kOpNullBox:
      op = Operand::null(Type::Box);
      return true;
    case kOpNullObj:
      op = Operand::null(Type::Obj);
      return true;
    case kOpSymbol: {
      std::string name;
      if (!str(name))
        return false;
      op = Operand::symbol(std::move(name));
      return true;
    }
    default:
      return R.fail("reserved operand kind", R.offset() - 1);
    }
  }
  /// A u8 that is 0 or 1.
  bool flag(uint8_t &v) {
    if (!R.u8(v))
      return false;
    if (v > 1)
      return R.fail("flag byte is not 0 or 1", R.offset() - 1);
    return true;
  }

  bool instr(Instr &in) {
    uint8_t code;
    if (!R.u8(code))
      return false;
    // A stable load's opcode follows its mark, a `retain lend`'s its own,
    // and a call's the ownership of its arguments that are not owned
    // (§5.5).
    in.Stable = code == kStableLoadCode;
    in.Lend = code == kLendCode;
    std::vector<std::pair<size_t, ArgOwnership>> own;
    const bool owns = code == kOwnershipCode;
    if (owns && !ownership(own))
      return false;
    if ((in.Stable || in.Lend || owns) && !R.u8(code))
      return false;
    if (!opcodeFromCode(code, in.Op))
      return R.fail("reserved opcode", R.offset() - 1);
    if (in.Stable && in.Op != Opcode::FieldLoad && in.Op != Opcode::PtrLoad)
      return R.fail("a stable mark before an instruction that is no load",
                    R.offset() - 1);
    if (in.Lend && in.Op != Opcode::Retain)
      return R.fail("a lend mark before an instruction that is no retain",
                    R.offset() - 1);
    if (owns && in.Op != Opcode::Call && in.Op != Opcode::VCall &&
        in.Op != Opcode::ICall)
      return R.fail("an ownership mark before an instruction that is no call",
                    R.offset() - 1);
    if (!R.uleb32(in.Result.Id))
      return false;
    if (in.Result.Id != kNoValue &&
        (!type(in.Result.Ty) || !str(in.Result.Name)))
      return false;
    switch (in.Op) {
    case Opcode::Cmp:
    case Opcode::UCmp: {
      uint8_t p;
      if (!R.u8(p))
        return false;
      if (!predFromCode(p, in.Pred))
        return R.fail("reserved cmp predicate", R.offset() - 1);
      break;
    }
    case Opcode::Wrap:
    case Opcode::Mask:
      if (!R.uleb32(in.Slot))
        return false;
      break;
    case Opcode::Cast:
      if (!type(in.CastTo))
        return false;
      break;
    case Opcode::Call:
    case Opcode::FnAddr:
      if (!str(in.Callee))
        return false;
      break;
    case Opcode::VCall:
      if (!str(in.ClassName) || !R.uleb32(in.Slot) || !sig(in.Sig))
        return false;
      break;
    case Opcode::ICall:
      if (!sig(in.Sig))
        return false;
      break;
    case Opcode::New:
    case Opcode::VTableAddr:
    case Opcode::VTableStore:
      if (!str(in.ClassName))
        return false;
      break;
    case Opcode::FieldLoad:
    case Opcode::FieldStore:
    case Opcode::FieldAddr:
      if (!str(in.ClassName) || !str(in.Field))
        return false;
      break;
    case Opcode::Load:
    case Opcode::Store:
    case Opcode::LocalAddr:
      if (!R.uleb32(in.Local))
        return false;
      break;
    default:
      break;
    }
    uint64_t n;
    if (!R.count(n))
      return false;
    in.Args.resize(static_cast<size_t>(n));
    for (Operand &a : in.Args)
      if (!operand(a))
        return false;
    if (own.empty())
      return true;
    if (own.back().first >= in.Args.size())
      return R.fail("an argument's ownership names no argument");
    in.Own.resize(in.Args.size());
    for (auto &[i, o] : own)
      in.Own[i] = std::move(o);
    return true;
  }

  /// The arguments' ownership after its mark (§5.5): at least one, in
  /// increasing order, each lent or passed.
  bool ownership(std::vector<std::pair<size_t, ArgOwnership>> &out) {
    uint64_t n;
    if (!R.count(n, 2))
      return false;
    if (n == 0)
      return R.fail("a call whose arguments are all owned has no ownership "
                    "mark");
    out.resize(static_cast<size_t>(n));
    for (size_t j = 0; j < out.size(); ++j) {
      auto &[i, o] = out[j];
      uint64_t at;
      uint8_t kind;
      if (!R.uleb(at, UINT32_MAX) || !R.u8(kind))
        return false;
      if (j > 0 && at <= out[j - 1].first)
        return R.fail("arguments' ownership out of order");
      i = static_cast<size_t>(at);
      if (kind == 1) {
        o.K = ArgOwnership::Lent;
      } else if (kind == 2) {
        o.K = ArgOwnership::Passed;
        if (!operand(o.Bit))
          return false;
      } else {
        return R.fail("reserved argument ownership", R.offset() - 1);
      }
    }
    return true;
  }

  bool block(Block &b) {
    if (++Depth > Opts.MaxNesting)
      return R.fail("blocks nested too deeply");
    uint64_t n;
    if (!R.count(n))
      return false;
    if (n > StmtBudget)
      return R.fail("too many statements");
    StmtBudget -= static_cast<size_t>(n);
    b.Stmts.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
      Stmt s;
      if (!stmt(s))
        return false;
      b.Stmts.push_back(std::move(s));
    }
    --Depth;
    return true;
  }
  bool subBlock(std::unique_ptr<Block> &out) {
    out = std::make_unique<Block>();
    return block(*out);
  }

  bool stmt(Stmt &s) {
    uint8_t tag;
    if (!R.u8(tag))
      return false;
    switch (tag) {
    case kStInstr: {
      Instr in;
      if (!instr(in))
        return false;
      s = std::move(in);
      return true;
    }
    case kStIf: {
      If i;
      uint8_t hasElse;
      if (!operand(i.Cond) || !subBlock(i.Then) || !flag(hasElse))
        return false;
      if (hasElse && !subBlock(i.Else))
        return false;
      s = std::move(i);
      return true;
    }
    case kStWhile: {
      While w;
      if (!subBlock(w.CondBlock) || !operand(w.Cond) || !subBlock(w.Body))
        return false;
      s = std::move(w);
      return true;
    }
    case kStBreak:
      s = Break{};
      return true;
    case kStContinue:
      s = Continue{};
      return true;
    case kStRet: {
      Return r;
      uint8_t hasValue;
      if (!flag(hasValue))
        return false;
      if (hasValue) {
        Operand v;
        if (!operand(v))
          return false;
        r.Value = std::move(v);
      }
      s = std::move(r);
      return true;
    }
    case kStUnreachable:
      s = Unreachable{};
      return true;
    case kStSwitch: {
      Switch sw;
      uint64_t n;
      if (!operand(sw.Value) || !R.count(n))
        return false;
      // Every case takes at least two bytes: its constant and its block.
      if (n > R.remaining() / 2)
        return R.fail("too many switch cases");
      sw.Cases.resize(static_cast<size_t>(n));
      for (Switch::Case &c : sw.Cases)
        if (!operand(c.Value) || !subBlock(c.Body))
          return false;
      uint8_t hasDefault;
      if (!flag(hasDefault) || (hasDefault && !subBlock(sw.Default)))
        return false;
      s = std::move(sw);
      return true;
    }
    default:
      return R.fail("reserved statement tag", R.offset() - 1);
    }
  }
};

/// One pass over a blob: the header, string table and every record, with
/// function bodies decoded only when @p out is given (inspect() skips them
/// by their length prefix).
Status walk(std::span<const uint8_t> blob, const DecodeOptions &opts,
            BlobInfo &info, Module *out) {
  ByteReader r(blob);
  if (!magicIs(r, kMagic, "magic") || !r.u32(info.CodecVersion) ||
      !r.u32(info.PIRVersion) || !r.u32(info.Flags))
    return readerError(r);
  if (info.CodecVersion != kCodecVersion)
    return Status::error("binary PIR: unsupported codec version " +
                         versionString(info.CodecVersion));
  if (info.PIRVersion != kPIRVersion)
    return Status::error("binary PIR: unsupported PIR version " +
                         versionString(info.PIRVersion));
  if (info.Flags & ~kKnownModuleFlags)
    return Status::error("binary PIR: reserved flag bits set");
  if (!support::StringTable::read(r, info.Strings))
    return readerError(r);
  size_t stmtBudget = opts.MaxStatements;
  Decoder d(r, info.Strings, opts, stmtBudget);
  if (!d.str(info.ModuleName))
    return readerError(r);
  if (out)
    out->Name = info.ModuleName;

  for (size_t t = 0; t < kTableCount; ++t) {
    info.TableOffsets[t] = r.offset();
    uint64_t n;
    if (!r.count(n, kMinRecordBytes[t]))
      return readerError(r);
    if ((t == kSlots || t == kClassObjs) && n != 0)
      return Status::error("binary PIR: reserved record kind at byte " +
                           std::to_string(info.TableOffsets[t]));
    if (t == kFunctions && n > opts.MaxFunctions)
      return Status::error("binary PIR: too many functions");
    for (uint64_t i = 0; i < n; ++i) {
      ItemRef item;
      item.Offset = r.offset();
      bool ok = true;
      switch (t) {
      case kExternGlobals: {
        ExternGlobal g;
        ok = d.externGlobal(g);
        item.Kind = SymbolKind::ExternGlobal;
        item.Name = g.Name;
        if (ok && out)
          out->Externs.push_back(std::move(g));
        break;
      }
      case kCStrs: {
        CStrGlobal g;
        ok = d.cstr(g);
        item.Kind = SymbolKind::CStr;
        item.Name = g.Name;
        if (ok && out)
          out->CStrs.push_back(std::move(g));
        break;
      }
      case kDatas: {
        DataGlobal g;
        ok = d.data(g);
        item.Kind = SymbolKind::Data;
        item.Name = g.Name;
        if (ok && out)
          out->Datas.push_back(std::move(g));
        break;
      }
      case kBytes: {
        BytesGlobal g;
        ok = d.bytes(g);
        item.Kind = SymbolKind::Bytes;
        item.Name = g.Name;
        if (ok && out)
          out->Bytes.push_back(std::move(g));
        break;
      }
      case kClasses: {
        Class c;
        ok = d.klass(c);
        item.Kind = c.IsExtern ? SymbolKind::ExternClass : SymbolKind::Class;
        item.Name = c.Name;
        if (ok && out)
          out->Classes.push_back(std::move(c));
        break;
      }
      case kFunctions: {
        uint64_t len;
        if (!r.uleb(len))
          return readerError(r);
        size_t base = r.offset();
        ByteReader sub = r.sub(static_cast<size_t>(len));
        if (!r.ok())
          return readerError(r);
        Decoder fd(sub, info.Strings, opts, stmtBudget);
        Function f;
        if (!fd.function(f, out != nullptr))
          return readerError(sub, base);
        if (out && !sub.atEnd())
          return Status::error("binary PIR: slack bytes in function record "
                               "at byte " +
                               std::to_string(base + sub.offset()));
        item.Kind =
            f.IsExtern ? SymbolKind::ExternFunction : SymbolKind::Function;
        item.Name = f.Name;
        if (out)
          out->Functions.push_back(std::move(f));
        break;
      }
      default:
        break;
      }
      if (!ok)
        return readerError(r);
      item.Length = r.offset() - item.Offset;
      info.Items.push_back(std::move(item));
    }
  }
  info.TrailerOffset = r.offset();
  if (!magicIs(r, kTrailer, "trailer"))
    return readerError(r);
  if (!r.atEnd())
    return Status::error("binary PIR: bytes after the trailer");
  return Status::ok();
}

bool symbolLess(const SymbolEntry &a, const SymbolEntry &b) {
  return std::tie(a.Kind, a.Name, a.Offset) <
         std::tie(b.Kind, b.Name, b.Offset);
}

} // namespace

std::vector<uint8_t> encode(const Module &m, const EncodeOptions &opts) {
  return Encoder(opts.StripNames).run(m);
}

StatusOr<Module> decode(std::span<const uint8_t> blob,
                        const DecodeOptions &opts) {
  BlobInfo info;
  Module m;
  if (Status s = walk(blob, opts, info, &m); !s)
    return s;
  return m;
}

StatusOr<BlobInfo> inspect(std::span<const uint8_t> blob) {
  BlobInfo info;
  if (Status s = walk(blob, DecodeOptions{}, info, nullptr); !s)
    return s;
  return info;
}

StatusOr<Function> decodeFunction(std::span<const uint8_t> blob,
                                  const BlobInfo &info, size_t offset,
                                  size_t length, const DecodeOptions &opts) {
  if (offset > blob.size() || length > blob.size() - offset)
    return Status::error("binary PIR: function record outside the blob");
  ByteReader r(blob.subspan(offset, length));
  uint64_t len;
  if (!r.uleb(len))
    return readerError(r, offset);
  if (len != r.remaining())
    return Status::error("binary PIR: function record length mismatch at "
                         "byte " +
                         std::to_string(offset));
  size_t stmtBudget = opts.MaxStatements;
  Decoder d(r, info.Strings, opts, stmtBudget);
  Function f;
  if (!d.function(f, true))
    return readerError(r, offset);
  if (!r.atEnd())
    return Status::error("binary PIR: slack bytes in function record at "
                         "byte " +
                         std::to_string(offset + r.offset()));
  return f;
}

std::vector<SymbolEntry> index(std::span<const uint8_t> blob) {
  std::vector<SymbolEntry> entries;
  auto info = inspect(blob);
  if (!info)
    return entries;
  for (const ItemRef &item : info.value().Items) {
    SymbolEntry e;
    e.Name = item.Name;
    e.Kind = static_cast<uint8_t>(item.Kind);
    e.Offset = item.Offset;
    e.Length = item.Length;
    e.Hash = support::sha256(blob.subspan(item.Offset, item.Length));
    entries.push_back(std::move(e));
  }
  std::sort(entries.begin(), entries.end(), symbolLess);
  return entries;
}

std::vector<uint8_t> encodeSymbolIndex(std::span<const SymbolEntry> entries) {
  std::vector<SymbolEntry> sorted(entries.begin(), entries.end());
  std::sort(sorted.begin(), sorted.end(), symbolLess);
  support::StringTable strings;
  ByteWriter body;
  body.uleb(sorted.size());
  for (const SymbolEntry &e : sorted) {
    body.uleb(strings.intern(e.Name));
    body.u8(e.Kind);
    body.u8(e.Linkage);
    body.uleb(e.Offset);
    body.uleb(e.Length);
    body.bytes(e.Hash);
  }
  body.bytes(kIndexTrailer, 4);
  ByteWriter out;
  out.bytes(kIndexMagic, 4);
  out.u32(kCodecVersion);
  strings.write(out);
  out.bytes(body.data());
  return out.take();
}

StatusOr<std::vector<SymbolEntry>>
decodeSymbolIndex(std::span<const uint8_t> section) {
  ByteReader r(section);
  uint32_t version;
  std::vector<std::string> strings;
  uint64_t n;
  if (!magicIs(r, kIndexMagic, "magic") || !r.u32(version))
    return readerError(r);
  if (version != kCodecVersion)
    return Status::error("symbol index: unsupported version " +
                         versionString(version));
  if (!support::StringTable::read(r, strings) ||
      !r.count(n, kMinIndexEntryBytes))
    return readerError(r);
  std::vector<SymbolEntry> entries(static_cast<size_t>(n));
  for (size_t i = 0; i < entries.size(); ++i) {
    SymbolEntry &e = entries[i];
    uint64_t name;
    std::span<const uint8_t> hash;
    if (!r.uleb(name, strings.empty() ? 0 : strings.size() - 1) ||
        !r.u8(e.Kind) || !r.u8(e.Linkage) || !r.uleb(e.Offset) ||
        !r.uleb(e.Length) || !r.bytes(32, hash))
      return readerError(r);
    e.Name = strings[static_cast<size_t>(name)];
    if (e.Kind > kMaxSymbolKind)
      return Status::error("symbol index: reserved symbol kind");
    if (e.Linkage > kMaxLinkage)
      return Status::error("symbol index: reserved linkage");
    std::copy(hash.begin(), hash.end(), e.Hash.begin());
    if (i > 0 && std::tie(e.Kind, e.Name) <
                     std::tie(entries[i - 1].Kind, entries[i - 1].Name))
      return Status::error("symbol index: entries are not sorted");
  }
  if (!magicIs(r, kIndexTrailer, "trailer"))
    return readerError(r);
  if (!r.atEnd())
    return Status::error("symbol index: bytes after the trailer");
  return entries;
}

} // namespace paykan::pir::binary
