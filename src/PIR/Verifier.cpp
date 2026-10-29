// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// PIR verifier: the rules of docs/pir.md §9.

#include "paykan/pir/Verifier.h"

#include "paykan/pir/Printer.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace paykan::pir {

std::string VerifyError::str() const { return Where + ": " + Message; }

std::string formatErrors(const std::vector<VerifyError> &errors) {
  std::string out;
  for (const VerifyError &e : errors) {
    out += e.str();
    out += "\n";
  }
  return out;
}

namespace {

std::string operandStr(const Operand &op) {
  std::ostringstream os;
  print(op, os);
  return os.str();
}

std::string sigStr(const Signature &sig) {
  std::ostringstream os;
  print(sig, os);
  return os.str();
}

bool isNumeric(Type t) { return t == Type::I64 || t == Type::F64; }
/// The types an address (local.addr, field.addr) may point to: scalars,
/// inline optionals, and the box of a reference (an `inout` parameter of a
/// reference type).
bool isAddressable(Type t) {
  return isNumeric(t) || t == Type::Bool || t == Type::Char || t == Type::Box ||
         isOptional(t);
}
/// The slots local.addr and field.addr take the address of: the addressable
/// ones and `mem`.
bool hasAddress(Type t) { return isAddressable(t) || t == Type::Mem; }
bool isPointerLike(Type t) {
  return t == Type::Box || t == Type::Obj || t == Type::Ptr;
}
bool isFieldType(Type t) {
  return t == Type::I64 || t == Type::F64 || t == Type::Bool ||
         t == Type::Char || t == Type::Box || isOptional(t) || t == Type::Mem ||
         t == Type::Weak;
}

/// What is wrong with a `mem` slot's layout, or "" when it is valid: a
/// positive size, a power-of-two alignment up to 8, and a size that is a
/// multiple of the alignment (as C pads an aggregate).
std::string memLayoutError(int64_t size, int64_t align) {
  if (size <= 0)
    return "a mem slot's size must be positive";
  if (!isMemAlign(align))
    return "a mem slot's alignment must be 1, 2, 4 or 8";
  if (size % align != 0)
    return "a mem slot's size must be a multiple of its alignment";
  return "";
}

/// True when @p sig takes or returns a `mem`, which is never a value.
bool hasMem(const Signature &sig) {
  if (sig.Ret == Type::Mem)
    return true;
  for (Type t : sig.Params)
    if (t == Type::Mem)
      return true;
  return false;
}

/// True when @p sig takes or returns an inline optional.
bool hasOptional(const Signature &sig) {
  if (isOptional(sig.Ret))
    return true;
  for (Type t : sig.Params)
    if (isOptional(t))
      return true;
  return false;
}

/// The `cast` table of docs/pir.md §6.
bool castAllowed(Type from, Type to) {
  if (from == to)
    return false; // a no-op cast is a mistake
  switch (from) {
  case Type::F64:
    return to == Type::I64;
  case Type::I64:
    return to == Type::F64 || to == Type::Bool || to == Type::Char ||
           isPointerLike(to);
  case Type::Bool:
  case Type::Char:
    return to == Type::I64;
  case Type::Box:
  case Type::Obj:
  case Type::Ptr:
    return to == Type::I64 || isPointerLike(to);
  case Type::Void:
  case Type::OptI64:
  case Type::OptF64:
  case Type::OptBool:
  case Type::OptChar:
  case Type::Mem:
  case Type::Weak:
    return false;
  }
  return false;
}

/// What a module declares, for symbol resolution.
struct ModuleIndex {
  const Module &M;
  std::unordered_map<std::string, const Function *> Functions;
  std::unordered_map<std::string, const Class *> Classes;
  std::unordered_map<std::string, Type>
      Globals; // ptr for data, obj/ptr for externs

  explicit ModuleIndex(const Module &m) : M(m) {
    for (const Function &f : m.Functions)
      Functions.emplace(f.Name, &f);
    for (const Class &c : m.Classes)
      Classes.emplace(c.Name, &c);
    for (const CStrGlobal &g : m.CStrs)
      Globals.emplace(g.Name, Type::Ptr);
    for (const DataGlobal &g : m.Datas)
      Globals.emplace(g.Name, Type::Ptr);
    for (const BytesGlobal &g : m.Bytes)
      Globals.emplace(g.Name, Type::Ptr);
    for (const ExternGlobal &g : m.Externs)
      Globals.emplace(g.Name,
                      g.K == ExternGlobal::Object ? Type::Obj : Type::Ptr);
  }

  const Field *findField(const Class &c, const std::string &name) const {
    for (const Field &f : c.Fields)
      if (f.Name == name)
        return &f;
    return nullptr;
  }
};

class ModuleVerifier {
public:
  ModuleVerifier(const Module &m, std::vector<VerifyError> &errors)
      : Index(m), Errors(errors) {}

  void run() {
    checkModuleItems();
    for (const Function &f : Index.M.Functions)
      if (!f.IsExtern)
        FunctionVerifier(*this, f).run();
  }

  const ModuleIndex Index;
  std::vector<VerifyError> &Errors;

  void error(const std::string &where, const std::string &msg) {
    Errors.push_back({where, msg});
  }

private:
  void checkModuleItems() {
    const Module &m = Index.M;
    const std::string where = m.Name;

    // One @-namespace for functions and globals.
    std::unordered_set<std::string> symbols;
    auto define = [&](const std::string &name, const char *what) {
      if (!symbols.insert(name).second)
        error(where, std::string(what) + " '@" + name + "' is defined twice");
    };
    for (const Function &f : m.Functions)
      define(f.Name, "symbol");
    for (const CStrGlobal &g : m.CStrs)
      define(g.Name, "symbol");
    for (const DataGlobal &g : m.Datas)
      define(g.Name, "symbol");
    for (const BytesGlobal &g : m.Bytes)
      define(g.Name, "symbol");
    for (const ExternGlobal &g : m.Externs)
      define(g.Name, "symbol");

    // Runtime externs, and only they, have `$rt.` names (PIR.h), so the
    // runtime's symbols and the program's never meet; native C externs (#198)
    // likewise have `$c.` names.
    auto programName = [&](const std::string &name, const char *what) {
      if (isRuntimeName(name))
        error(where, std::string(what) + " '@" + name +
                         "' has a runtime name (only a runtime extern may)");
      if (isNativeName(name))
        error(where, std::string(what) + " '@" + name +
                         "' has a native name (only a native extern may)");
    };
    auto runtimeExternName = [&](const std::string &name, const char *what) {
      if (!isRuntimeName(name))
        error(where, std::string(what) + " '@" + name + "' is not named '@" +
                         std::string(kRuntimePrefix) + "<symbol>'");
    };
    for (const Function &f : m.Functions) {
      if (f.IsExtern && f.Module.empty()) {
        if (!isNativeName(f.Name))
          runtimeExternName(f.Name, "runtime extern function");
      } else {
        programName(f.Name, "function");
        if (!f.Symbol.empty())
          programName(f.Symbol, "extern function symbol");
      }
    }
    for (const CStrGlobal &g : m.CStrs)
      programName(g.Name, "cstr global");
    for (const DataGlobal &g : m.Datas)
      programName(g.Name, "data global");
    for (const BytesGlobal &g : m.Bytes)
      programName(g.Name, "bytes global");
    for (const ExternGlobal &g : m.Externs)
      runtimeExternName(g.Name, "extern global");

    std::unordered_set<std::string> classes;
    for (const Class &c : m.Classes) {
      if (!classes.insert(c.Name).second)
        error(where, "class '" + c.Name + "' is defined twice");
      if (!c.Super.empty() && !Index.Classes.count(c.Super))
        error(where,
              "class '" + c.Name + "' extends unknown class '" + c.Super + "'");
      if (c.IsExtern && c.Module.empty())
        error(where, "extern class '" + c.Name + "' names no module");
      std::unordered_set<std::string> fields;
      for (const Field &f : c.Fields) {
        if (!fields.insert(f.Name).second)
          error(where,
                "class '" + c.Name + "' declares field '" + f.Name + "' twice");
        if (!isFieldType(f.Ty))
          error(where, "field '" + c.Name + "." + f.Name + "' has type " +
                           typeName(f.Ty) +
                           " (fields are i64, f64, bool, char, box, an "
                           "optional or mem)");
        else if (f.Ty == Type::Mem)
          if (std::string bad = memLayoutError(f.Size, f.Align); !bad.empty())
            error(where, "field '" + c.Name + "." + f.Name + "': " + bad);
      }
      if (!c.IsExtern && (c.VTable.empty() || c.VTable[0].Slot != "destroy"))
        error(where, "class '" + c.Name + "': vtable slot 0 must be 'destroy'");
      for (size_t i = 0; i < c.VTable.size(); ++i) {
        const VTableEntry &e = c.VTable[i];
        if (e.Target.empty())
          continue;
        auto it = Index.Functions.find(e.Target);
        if (it == Index.Functions.end()) {
          error(where, "class '" + c.Name + "' vtable slot " +
                           std::to_string(i) + " ('" + e.Slot +
                           "') names undeclared function '@" + e.Target + "'");
        } else if (const Signature slot =
                       machineSignature(e.Sig, lendsType(e.Sig));
                   !(machineSignature(*it->second) == slot)) {
          // A slot's and its target's machine functions are the same: a
          // runtime target spells out the ownership mask a slot's type
          // implies (`$rt.PaykanObject_equals(obj, box, i64)` for `(obj,
          // box)`, §3).
          error(where, "class '" + c.Name + "' vtable slot " +
                           std::to_string(i) + " ('" + e.Slot +
                           "') has signature " + sigStr(e.Sig) +
                           " (a machine function " + sigStr(slot) + ") but '@" +
                           e.Target + "' is declared " +
                           sigStr(it->second->Sig) + " (a machine function " +
                           sigStr(machineSignature(*it->second)) + ")");
        }
      }
    }

    for (const Function &f : m.Functions) {
      if (hasMem(f.Sig))
        error(where, "function '@" + f.Name +
                         "' takes or returns a mem (pass its address)");
      if (!f.Uses.empty() && !m.isCompiled(f))
        error(where, "function '@" + f.Name +
                         "' has uses but is not a compiled declaration (an "
                         "extern naming its own module)");
      for (const std::string &u : f.Uses)
        if (!Index.Functions.count(u) && !Index.Classes.count(u))
          error(where, "compiled function '@" + f.Name + "' uses '@" + u +
                           "', which the module does not declare");
      if (f.IsExtern && f.Shared && f.Module.empty())
        error(where, "runtime extern function '@" + f.Name + "' is shared");
      if (f.IsExtern) {
        if (!f.Params.empty() || !f.Locals.empty() || !f.Body.Stmts.empty())
          error(where, "extern function '@" + f.Name + "' has a body");
        if (!f.Symbol.empty() && f.Module.empty())
          error(where, "runtime extern function '@" + f.Name +
                           "' has a 'symbol' (only a module extern may)");
        // An inline optional crosses into C only as its parts: its by-value
        // passing is not the same on both backends' C ABIs.
        if (f.Module.empty() && hasOptional(f.Sig))
          error(where, "extern function '@" + f.Name +
                           "' takes or returns an optional (a runtime or "
                           "native function cannot)");
        continue;
      }
      if (f.Params.size() != f.Sig.Params.size()) {
        error(where, "function '@" + f.Name +
                         "': parameter list does not match its signature");
      } else {
        for (size_t i = 0; i < f.Params.size(); ++i)
          if (f.Params[i].Ty != f.Sig.Params[i])
            error(where,
                  "function '@" + f.Name + "': parameter " + std::to_string(i) +
                      " has type " + typeName(f.Params[i].Ty) +
                      " but the signature says " + typeName(f.Sig.Params[i]));
      }
      if (f.Name == "main") {
        bool ok = f.Sig.Ret == Type::I64 &&
                  (f.Sig.Params.empty() ||
                   (f.Sig.Params.size() == 2 && f.Sig.Params[0] == Type::Ptr &&
                    f.Sig.Params[1] == Type::I64));
        if (!ok)
          error(where, "'@main' must have signature () -> i64 or "
                       "(ptr, i64) -> i64, not " +
                           sigStr(f.Sig));
      }
    }
  }

  // -- Function bodies

  class FunctionVerifier {
  public:
    FunctionVerifier(ModuleVerifier &mv, const Function &f)
        : MV(mv), F(f), Where(mv.Index.M.Name + ":@" + f.Name) {}

    void run() {
      for (const Local &l : F.Locals) {
        if (l.Ty == Type::Void)
          error("local '%" + l.Name + "' has type void");
        else if (l.Ty == Type::Mem)
          if (std::string bad = memLayoutError(l.Size, l.Align); !bad.empty())
            error("local '%" + l.Name + "': " + bad);
      }
      for (const Value &p : F.Params)
        define(p);
      bool returns = verifyBlock(F.Body, /*inLoop=*/false);
      if (F.Sig.Ret != Type::Void && !returns)
        error("control can fall off the end of a non-void function");
    }

  private:
    ModuleVerifier &MV;
    const Function &F;
    std::string Where;
    /// Values visible at this point: id -> type.  Block scoping is done by
    /// removing a block's definitions when it closes.
    std::unordered_map<ValueId, Type> Visible;
    std::unordered_set<ValueId> EverDefined;

    void error(const std::string &msg) { MV.error(Where, msg); }

    void define(const Value &v) {
      if (v.Id == kNoValue) {
        error("a value has no id");
        return;
      }
      if (!EverDefined.insert(v.Id).second)
        error("value '%" + std::to_string(v.Id) + "' is defined twice");
      if (v.Ty == Type::Void || v.Ty == Type::Mem)
        error("value '%" + std::to_string(v.Id) + "' has type " +
              typeName(v.Ty));
      Visible[v.Id] = v.Ty;
    }

    /// Type of an operand, or Void after reporting a use of an unknown value.
    Type typeOf(const Operand &op) {
      if (op.isValue()) {
        auto it = Visible.find(op.valueId());
        if (it == Visible.end()) {
          error("use of value '%" + std::to_string(op.valueId()) +
                "' before its definition or outside its block");
          return Type::Void;
        }
        return it->second;
      }
      if (const auto *n = std::get_if<Operand::Null>(&op.V)) {
        if (n->Ty != Type::Box && n->Ty != Type::Obj)
          error("null must be box or obj");
        return n->Ty;
      }
      if (const auto *s = std::get_if<SymbolRef>(&op.V)) {
        auto it = MV.Index.Globals.find(s->Name);
        if (it == MV.Index.Globals.end()) {
          error("reference to undeclared global '@" + s->Name + "'");
          return Type::Void;
        }
        return it->second;
      }
      if (std::holds_alternative<int64_t>(op.V))
        return Type::I64;
      if (std::holds_alternative<double>(op.V))
        return Type::F64;
      if (std::holds_alternative<bool>(op.V))
        return Type::Bool;
      return Type::Char;
    }

    /// Checks that @p op has type @p want; reports otherwise.
    void expect(const Operand &op, Type want, const char *what) {
      Type got = typeOf(op);
      if (got != Type::Void && got != want)
        error(std::string(what) + " " + operandStr(op) + " has type " +
              typeName(got) + ", expected " + typeName(want));
    }

    void expectArgs(const std::vector<Operand> &args, size_t from,
                    const Signature &sig, const std::string &callee) {
      if (args.size() - from != sig.Params.size()) {
        error("call to " + callee + " passes " +
              std::to_string(args.size() - from) + " argument(s), expected " +
              std::to_string(sig.Params.size()));
        return;
      }
      for (size_t i = 0; i < sig.Params.size(); ++i)
        expect(args[from + i], sig.Params[i],
               ("argument " + std::to_string(i) + " of " + callee).c_str());
    }

    /// The ownership of a call's arguments (§3, §6): none, or one for each
    /// argument, lent or passed only for a box parameter of a lending
    /// callee that has a bit in the ownership mask, a passed one's bit a
    /// `bool` value.
    void expectOwnership(const Instr &in, const Signature &sig, bool lend,
                         const std::string &callee) {
      if (in.Own.empty())
        return;
      if (in.Own.size() != in.Args.size()) {
        error("call to " + callee + " gives the ownership of " +
              std::to_string(in.Own.size()) + " argument(s), but passes " +
              std::to_string(in.Args.size()));
        return;
      }
      const size_t first = firstParamArg(in);
      for (size_t i = 0; i < in.Own.size(); ++i) {
        const ArgOwnership &o = in.Own[i];
        if (o.K == ArgOwnership::Owned)
          continue;
        const std::string what =
            i < first
                ? "the callee of " + callee
                : "argument " + std::to_string(i - first) + " of " + callee;
        if (!lend) {
          std::string msg = what;
          msg += " is lent or passed, but ";
          msg += callee;
          msg += " does not lend (a runtime or native function, or one "
                 "without a box parameter)";
          error(msg);
          continue;
        }
        if (i < first || i - first >= sig.Params.size() ||
            sig.Params[i - first] != Type::Box) {
          error(what + " is lent or passed, but is no box parameter");
          continue;
        }
        if (size_t k = boxIndex(sig, i - first); k >= kOwnershipBits)
          error(what + " is lent or passed, but is box parameter " +
                std::to_string(k) +
                ", which the ownership mask has no bit for (it is always "
                "owned)");
        if (o.K == ArgOwnership::Passed) {
          if (!o.Bit.isValue())
            error("the passed bit of " + what + " is not a value");
          else
            expect(o.Bit, Type::Bool, ("passed bit of " + what).c_str());
        }
      }
    }

    bool arity(const Instr &in, size_t n) {
      if (in.Args.size() != n) {
        error(std::string("'") + opName(in) + "' takes " + std::to_string(n) +
              " operand(s), got " + std::to_string(in.Args.size()));
        return false;
      }
      return true;
    }

    static std::string opName(const Instr &in) { return opcodeName(in.Op); }

    /// Checks the result type against @p want, when the instruction has one.
    void result(const Instr &in, Type want) {
      if (in.Result.Id == kNoValue) {
        if (want != Type::Void)
          error(std::string("'") + opName(in) +
                "' produces a value but has no result");
        return;
      }
      if (want == Type::Void) {
        error(std::string("'") + opName(in) +
              "' produces no value but has a result");
      } else if (in.Result.Ty != want) {
        error("result of '" + opName(in) + "' has type " +
              typeName(in.Result.Ty) + ", expected " + typeName(want));
      }
      define(in.Result);
    }

    void verifyInstr(const Instr &in) {
      // Only a box read from a slot is stable (§6).
      if (in.Stable &&
          ((in.Op != Opcode::FieldLoad && in.Op != Opcode::PtrLoad) ||
           in.Result.Ty != Type::Box))
        error(std::string("a stable '") + opName(in) +
              "' (only a field.load or a ptr.load of a box is stable)");
      // Only a call's arguments are lent or passed, and only a retain
      // lends (§6).
      if (!in.Own.empty() && in.Op != Opcode::Call && in.Op != Opcode::VCall &&
          in.Op != Opcode::ICall)
        error(std::string("'") + opName(in) +
              "' gives its operands an ownership (only a call's arguments "
              "have one)");
      if (in.Lend && in.Op != Opcode::Retain)
        error(std::string("a lend '") + opName(in) + "' (only a retain lends)");
      switch (in.Op) {
      case Opcode::Add:
      case Opcode::Sub:
      case Opcode::Mul:
      case Opcode::Div:
      case Opcode::Rem: {
        if (!arity(in, 2))
          break;
        Type a = typeOf(in.Args[0]), b = typeOf(in.Args[1]);
        if (a != Type::Void && !isNumeric(a))
          error("arithmetic on " + std::string(typeName(a)));
        else if (a != Type::Void && b != Type::Void && a != b)
          error("arithmetic operands have different types: " +
                std::string(typeName(a)) + " and " + typeName(b));
        result(in, a == Type::Void ? in.Result.Ty : a);
        break;
      }
      case Opcode::Neg: {
        if (!arity(in, 1))
          break;
        Type a = typeOf(in.Args[0]);
        if (a != Type::Void && !isNumeric(a))
          error("neg on " + std::string(typeName(a)));
        result(in, a == Type::Void ? in.Result.Ty : a);
        break;
      }
      case Opcode::Not:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Bool, "operand of not");
        result(in, Type::Bool);
        break;
      case Opcode::Cmp: {
        if (!arity(in, 2))
          break;
        Type a = typeOf(in.Args[0]), b = typeOf(in.Args[1]);
        if (a != Type::Void && b != Type::Void && a != b)
          error("cmp operands have different types: " +
                std::string(typeName(a)) + " and " + typeName(b));
        if (isPointerLike(a) && in.Pred != CmpPred::Eq &&
            in.Pred != CmpPred::Ne)
          error("only eq/ne compare " + std::string(typeName(a)) + " values");
        if (isOptional(a))
          error("cmp on " + std::string(typeName(a)) +
                " (compare is_some and the unwrapped payloads)");
        result(in, Type::Bool);
        break;
      }
      case Opcode::Select: {
        if (!arity(in, 3))
          break;
        expect(in.Args[0], Type::Bool, "condition of select");
        Type a = typeOf(in.Args[1]), b = typeOf(in.Args[2]);
        if (a != Type::Void && b != Type::Void && a != b)
          error("select arms have different types: " +
                std::string(typeName(a)) + " and " + typeName(b));
        result(in, a == Type::Void ? in.Result.Ty : a);
        break;
      }
      case Opcode::IToF:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::I64, "operand of itof");
        result(in, Type::F64);
        break;
      case Opcode::FToI:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::F64, "operand of ftoi");
        result(in, Type::I64);
        break;
      case Opcode::Cast: {
        if (!arity(in, 1))
          break;
        Type from = typeOf(in.Args[0]);
        if (from != Type::Void && !castAllowed(from, in.CastTo))
          error("cannot cast " + std::string(typeName(from)) + " to " +
                typeName(in.CastTo));
        result(in, in.CastTo);
        break;
      }
      case Opcode::Wrap:
      case Opcode::Mask:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::I64, "operand of wrap / mask");
        if (in.Slot != 8 && in.Slot != 16 && in.Slot != 32)
          error(std::string(opcodeName(in.Op)) + " width " +
                std::to_string(in.Slot) + " is not 8, 16 or 32");
        result(in, Type::I64);
        break;
      case Opcode::UDiv:
      case Opcode::URem:
        if (!arity(in, 2))
          break;
        expect(in.Args[0], Type::I64, "operand of udiv / urem");
        expect(in.Args[1], Type::I64, "operand of udiv / urem");
        result(in, Type::I64);
        break;
      case Opcode::UCmp:
        if (!arity(in, 2))
          break;
        expect(in.Args[0], Type::I64, "operand of ucmp");
        expect(in.Args[1], Type::I64, "operand of ucmp");
        result(in, Type::Bool);
        break;
      case Opcode::UToF:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::I64, "operand of utof");
        result(in, Type::F64);
        break;
      case Opcode::FToU:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::F64, "operand of ftou");
        result(in, Type::I64);
        break;
      case Opcode::FRound32:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::F64, "operand of fround32");
        result(in, Type::F64);
        break;
      case Opcode::Call: {
        auto it = MV.Index.Functions.find(in.Callee);
        if (it == MV.Index.Functions.end()) {
          error("call to undeclared function '@" + in.Callee + "'");
          for (const Operand &a : in.Args)
            typeOf(a);
          if (in.Result.Id != kNoValue)
            define(in.Result);
          break;
        }
        expectArgs(in.Args, 0, it->second->Sig, "'@" + in.Callee + "'");
        expectOwnership(in, it->second->Sig, lends(*it->second),
                        "'@" + in.Callee + "'");
        result(in, it->second->Sig.Ret);
        break;
      }
      case Opcode::VCall: {
        if (in.Args.empty()) {
          error("vcall has no receiver");
          break;
        }
        expect(in.Args[0], Type::Obj, "receiver of vcall");
        Signature sig = in.Sig;
        bool known = true;
        if (!in.ClassName.empty()) {
          auto it = MV.Index.Classes.find(in.ClassName);
          if (it == MV.Index.Classes.end()) {
            error("vcall on unknown class '" + in.ClassName + "'");
            known = false;
          } else if (in.Slot >= it->second->VTable.size()) {
            error("vcall slot " + std::to_string(in.Slot) +
                  " is out of range for class '" + in.ClassName + "' (" +
                  std::to_string(it->second->VTable.size()) + " slots)");
            known = false;
          } else {
            sig = it->second->VTable[in.Slot].Sig;
          }
        }
        if (!known) {
          for (size_t i = 1; i < in.Args.size(); ++i)
            typeOf(in.Args[i]);
          if (in.Result.Id != kNoValue)
            define(in.Result);
          break;
        }
        // The slot's signature includes the receiver.
        const std::string slot =
            "vcall slot " + std::to_string(in.Slot) +
            (in.ClassName.empty() ? "" : " of '" + in.ClassName + "'");
        expectArgs(in.Args, 0, sig, slot);
        expectOwnership(in, sig, lendsType(sig), slot);
        result(in, sig.Ret);
        break;
      }
      case Opcode::FnAddr:
        if (!arity(in, 0))
          break;
        if (!MV.Index.Functions.count(in.Callee))
          error("address of undeclared function '@" + in.Callee + "'");
        result(in, Type::Ptr);
        break;
      case Opcode::ICall:
        if (in.Args.empty()) {
          error("icall has no callee");
          break;
        }
        expect(in.Args[0], Type::Ptr, "callee of icall");
        expectArgs(in.Args, 1, in.Sig, "icall");
        expectOwnership(in, in.Sig, lendsType(in.Sig), "icall");
        result(in, in.Sig.Ret);
        break;
      case Opcode::Retain:
      case Opcode::Release:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Box, "operand of retain/release");
        result(in, Type::Void);
        break;
      case Opcode::Box:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Obj, "operand of box");
        result(in, Type::Box);
        break;
      case Opcode::Unbox:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Box, "operand of unbox");
        result(in, Type::Obj);
        break;
      case Opcode::WeakNew:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Box, "operand of weak.new");
        result(in, Type::Weak);
        break;
      case Opcode::WeakLoad:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Weak, "operand of weak.load");
        result(in, Type::Box);
        break;
      case Opcode::WeakRelease:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Weak, "operand of weak.release");
        result(in, Type::Void);
        break;
      case Opcode::New:
        arity(in, 0);
        if (!MV.Index.Classes.count(in.ClassName))
          error("new of unknown class '" + in.ClassName + "'");
        result(in, Type::Obj);
        break;
      case Opcode::Free:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Obj, "operand of free");
        result(in, Type::Void);
        break;
      case Opcode::FieldLoad:
      case Opcode::FieldStore: {
        bool store = in.Op == Opcode::FieldStore;
        if (!arity(in, store ? 2 : 1))
          break;
        expect(in.Args[0], Type::Obj, "receiver of field access");
        const Field *fld = nullptr;
        auto it = MV.Index.Classes.find(in.ClassName);
        if (it == MV.Index.Classes.end()) {
          error("field access on unknown class '" + in.ClassName + "'");
        } else {
          fld = MV.Index.findField(*it->second, in.Field);
          if (!fld)
            error("class '" + in.ClassName + "' has no field '" + in.Field +
                  "'");
          else if (fld->Ty == Type::Mem)
            error(opName(in) + " of mem field '" + in.Field +
                  "' (a mem slot is read and written through its address)");
        }
        if (store) {
          if (fld)
            expect(in.Args[1], fld->Ty, "stored value");
          else
            typeOf(in.Args[1]);
          result(in, Type::Void);
        } else {
          result(in, fld ? fld->Ty : in.Result.Ty);
        }
        break;
      }
      case Opcode::VTableLoad:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Obj, "operand of vtable.load");
        result(in, Type::Ptr);
        break;
      case Opcode::VTableStore:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Obj, "operand of vtable.store");
        if (!MV.Index.Classes.count(in.ClassName))
          error("vtable.store of unknown class '" + in.ClassName + "'");
        result(in, Type::Void);
        break;
      case Opcode::VTableAddr:
        if (!in.ClassName.empty()) {
          arity(in, 0);
          if (!MV.Index.Classes.count(in.ClassName))
            error("vtable.addr of unknown class '" + in.ClassName + "'");
        } else if (arity(in, 1)) {
          const auto *s = std::get_if<SymbolRef>(&in.Args[0].V);
          if (!s)
            error("vtable.addr takes a class name or an '@vtable' symbol");
          else
            expect(in.Args[0], Type::Ptr, "operand of vtable.addr");
        }
        result(in, Type::Ptr);
        break;
      case Opcode::Load:
        arity(in, 0);
        if (in.Local >= F.Locals.size()) {
          error("load of undeclared local " + std::to_string(in.Local));
          result(in, in.Result.Ty);
        } else if (F.Locals[in.Local].Ty == Type::Mem) {
          error("load of a mem local (it is read through its address)");
          result(in, in.Result.Ty);
        } else {
          result(in, F.Locals[in.Local].Ty);
        }
        break;
      case Opcode::Store:
        if (!arity(in, 1))
          break;
        if (in.Local >= F.Locals.size())
          error("store to undeclared local " + std::to_string(in.Local));
        else if (F.Locals[in.Local].Ty == Type::Mem)
          error("store to a mem local (it is written through its address)");
        else
          expect(in.Args[0], F.Locals[in.Local].Ty, "stored value");
        result(in, Type::Void);
        break;
      case Opcode::LocalAddr:
        arity(in, 0);
        if (in.Local >= F.Locals.size())
          error("local.addr of undeclared local " + std::to_string(in.Local));
        else if (!hasAddress(F.Locals[in.Local].Ty))
          error("local.addr of a local of type " +
                std::string(typeName(F.Locals[in.Local].Ty)) +
                " (only i64, f64, bool, char, optional, box and mem slots "
                "have addresses)");
        result(in, Type::Ptr);
        break;
      case Opcode::FieldAddr: {
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Obj, "receiver of field.addr");
        auto it = MV.Index.Classes.find(in.ClassName);
        const Field *fld = it == MV.Index.Classes.end()
                               ? nullptr
                               : MV.Index.findField(*it->second, in.Field);
        if (it == MV.Index.Classes.end())
          error("field.addr on unknown class '" + in.ClassName + "'");
        else if (!fld)
          error("class '" + in.ClassName + "' has no field '" + in.Field + "'");
        else if (!hasAddress(fld->Ty))
          error("field.addr of a field of type " +
                std::string(typeName(fld->Ty)) +
                " (only i64, f64, bool, char, optional, box and mem slots "
                "have addresses)");
        result(in, Type::Ptr);
        break;
      }
      case Opcode::PtrLoad:
        if (!arity(in, 1))
          break;
        expect(in.Args[0], Type::Ptr, "address of ptr.load");
        if (!isAddressable(in.Result.Ty))
          error("ptr.load of " + std::string(typeName(in.Result.Ty)) +
                " (only i64, f64, bool, char, an optional and box)");
        result(in, in.Result.Ty);
        break;
      case Opcode::PtrStore: {
        if (!arity(in, 2))
          break;
        expect(in.Args[0], Type::Ptr, "address of ptr.store");
        Type t = typeOf(in.Args[1]);
        if (t != Type::Void && !isAddressable(t))
          error("ptr.store of " + std::string(typeName(t)) +
                " (only i64, f64, bool, char, an optional and box)");
        result(in, Type::Void);
        break;
      }
      case Opcode::PtrAdd:
        if (!arity(in, 2))
          break;
        expect(in.Args[0], Type::Ptr, "address of ptr.add");
        expect(in.Args[1], Type::I64, "offset of ptr.add");
        result(in, Type::Ptr);
        break;
      case Opcode::MemCopy:
        if (!arity(in, 3))
          break;
        expect(in.Args[0], Type::Ptr, "destination of mem.copy");
        expect(in.Args[1], Type::Ptr, "source of mem.copy");
        expect(in.Args[2], Type::I64, "byte count of mem.copy");
        result(in, Type::Void);
        break;
      case Opcode::MemZero:
        if (!arity(in, 2))
          break;
        expect(in.Args[0], Type::Ptr, "address of mem.zero");
        expect(in.Args[1], Type::I64, "byte count of mem.zero");
        result(in, Type::Void);
        break;
      case Opcode::Some: {
        if (!arity(in, 1))
          break;
        Type a = typeOf(in.Args[0]);
        if (a != Type::Void && optionalOf(a) == Type::Void)
          error("some of " + std::string(typeName(a)) +
                " (only i64, f64, bool and char have an optional)");
        result(in, optionalOf(a) == Type::Void ? in.Result.Ty : optionalOf(a));
        break;
      }
      case Opcode::None:
        arity(in, 0);
        if (in.Result.Id != kNoValue && !isOptional(in.Result.Ty))
          error("none of " + std::string(typeName(in.Result.Ty)) +
                ", which is not an optional");
        result(in, in.Result.Ty);
        break;
      case Opcode::IsSome:
      case Opcode::Unwrap: {
        if (!arity(in, 1))
          break;
        Type a = typeOf(in.Args[0]);
        if (a != Type::Void && !isOptional(a))
          error("operand of " + opName(in) + " has type " + typeName(a) +
                ", expected an optional");
        if (in.Op == Opcode::IsSome)
          result(in, Type::Bool);
        else
          result(in, isOptional(a) ? optionalPayload(a) : in.Result.Ty);
        break;
      }
      case Opcode::Owned:
        if (!arity(in, 1))
          break;
        verifyOwned(in.Args[0]);
        result(in, Type::Bool);
        break;
      }
    }

    /// `owned %p` (§3): %p a box parameter of the function (which then
    /// lends) that has a bit in the ownership mask.
    void verifyOwned(const Operand &p) {
      size_t at = F.Params.size();
      for (size_t i = 0; i < F.Params.size(); ++i)
        if (p.isValue() && F.Params[i].Id == p.valueId())
          at = i;
      if (at == F.Params.size() || F.Params[at].Ty != Type::Box) {
        error("'owned' of " + operandStr(p) +
              ", which is no box parameter of the function");
        return;
      }
      if (size_t k = boxIndex(F.Sig, at); k >= kOwnershipBits)
        error("'owned' of box parameter " + std::to_string(k) +
              ", which the ownership mask has no bit for (it is always "
              "owned)");
    }

    /// Verifies a switch; returns true when control cannot fall out of it
    /// (it has a default, and every branch ends in a terminator).
    bool verifySwitch(const Switch &sw, bool inLoop) {
      Type t = typeOf(sw.Value);
      if (t != Type::Void && t != Type::I64 && t != Type::Char &&
          t != Type::Bool)
        error("switch on " + operandStr(sw.Value) + " of type " + typeName(t) +
              ", expected i64, char or bool");
      bool terminated = sw.Default != nullptr;
      for (size_t i = 0; i < sw.Cases.size(); ++i) {
        const Switch::Case &c = sw.Cases[i];
        if (c.Value.isValue()) {
          error("switch case " + operandStr(c.Value) + " is not a constant");
        } else {
          if (t != Type::Void)
            expect(c.Value, t, "switch case");
          for (size_t j = 0; j < i; ++j)
            if (sameConstant(sw.Cases[j].Value, c.Value))
              error("switch case " + operandStr(c.Value) + " listed twice");
        }
        if (!c.Body) {
          error("switch case without a block");
          terminated = false;
          continue;
        }
        terminated = verifyBlock(*c.Body, inLoop) && terminated;
      }
      if (sw.Default)
        terminated = verifyBlock(*sw.Default, inLoop) && terminated;
      return terminated;
    }

    /// Verifies a block; returns true when control cannot fall out of its
    /// end (it ends in ret / unreachable, or an if or a switch whose every
    /// branch does).
    bool verifyBlock(const Block &b, bool inLoop) {
      std::vector<ValueId> defined;
      auto before = Visible;
      bool terminated = false;
      for (const Stmt &s : b.Stmts) {
        if (terminated) {
          error("statement follows a terminator in the same block");
          terminated = false; // report once per block
        }
        if (const auto *in = std::get_if<Instr>(&s)) {
          verifyInstr(*in);
        } else if (const auto *i = std::get_if<If>(&s)) {
          expect(i->Cond, Type::Bool, "if condition");
          bool t = i->Then ? verifyBlock(*i->Then, inLoop) : false;
          bool e = i->Else ? verifyBlock(*i->Else, inLoop) : false;
          if (!i->Then)
            error("if without a then block");
          terminated = i->Else && t && e;
        } else if (const auto *sw = std::get_if<Switch>(&s)) {
          terminated = verifySwitch(*sw, inLoop);
        } else if (const auto *w = std::get_if<While>(&s)) {
          if (!w->CondBlock || !w->Body) {
            error("while without a condition region or body");
            continue;
          }
          auto beforeCond = Visible;
          // The condition region's values are visible to the body: structure
          // is dominance, and the body runs only after the region.
          verifyBlockKeepingValues(*w->CondBlock, inLoop);
          expect(w->Cond, Type::Bool, "while condition");
          verifyBlock(*w->Body, /*inLoop=*/true);
          Visible = std::move(beforeCond);
        } else if (std::holds_alternative<Break>(s) ||
                   std::holds_alternative<Continue>(s)) {
          if (!inLoop)
            error("break/continue outside a while");
          terminated = true;
        } else if (const auto *r = std::get_if<Return>(&s)) {
          if (F.Sig.Ret == Type::Void) {
            if (r->Value)
              error("ret with a value in a void function");
          } else if (!r->Value) {
            error("ret without a value in a non-void function");
          } else {
            expect(*r->Value, F.Sig.Ret, "returned value");
          }
          terminated = true;
        } else {
          terminated = true; // unreachable
        }
      }
      Visible = std::move(before);
      return terminated;
    }

    /// Like verifyBlock, but the block's definitions stay visible (for a
    /// while's condition region, whose values the body may use).
    void verifyBlockKeepingValues(const Block &b, bool inLoop) {
      bool terminated = false;
      for (const Stmt &s : b.Stmts) {
        if (terminated)
          error("statement follows a terminator in the same block");
        if (const auto *in = std::get_if<Instr>(&s)) {
          verifyInstr(*in);
        } else if (std::holds_alternative<Break>(s) ||
                   std::holds_alternative<Continue>(s)) {
          error("break/continue inside a while condition region");
        } else if (std::holds_alternative<Return>(s) ||
                   std::holds_alternative<Unreachable>(s)) {
          error("a while condition region cannot end the function");
          terminated = true;
        } else if (const auto *i = std::get_if<If>(&s)) {
          expect(i->Cond, Type::Bool, "if condition");
          if (i->Then)
            verifyBlock(*i->Then, inLoop);
          if (i->Else)
            verifyBlock(*i->Else, inLoop);
        } else if (const auto *sw = std::get_if<Switch>(&s)) {
          verifySwitch(*sw, inLoop);
        } else if (const auto *w = std::get_if<While>(&s)) {
          if (!w->CondBlock || !w->Body)
            continue;
          auto beforeCond = Visible;
          verifyBlockKeepingValues(*w->CondBlock, inLoop);
          expect(w->Cond, Type::Bool, "while condition");
          verifyBlock(*w->Body, true);
          Visible = std::move(beforeCond);
        }
      }
    }
  };
};

/// Cross-module checks: every extern that names another module must match
/// a definition in that module (in its PIR or its compiled code), shared
/// when it is.  A compiled declaration, which names its own module, is
/// checked against the module's compiled code, which PIR does not see.
void verifyProgramLinks(const Program &p, std::vector<VerifyError> &errors) {
  std::unordered_map<std::string, const Module *> modules;
  for (const Module &m : p.Modules) {
    if (!modules.emplace(m.Name, &m).second)
      errors.push_back({m.Name, "module appears twice in the program"});
  }
  for (const Module &m : p.Modules) {
    for (const Function &f : m.Functions) {
      if (!f.IsExtern || f.Module.empty() || m.isCompiled(f))
        continue;
      auto it = modules.find(f.Module);
      if (it == modules.end()) {
        errors.push_back({m.Name, "extern fn '@" + f.Name + "' names module '" +
                                      f.Module +
                                      "', which is not in the program"});
        continue;
      }
      const Function *def = it->second->findFunction(f.linkName());
      if (!def || !it->second->defines(*def))
        errors.push_back({m.Name, "extern fn '@" + f.linkName() +
                                      "' is not defined in module '" +
                                      f.Module + "'"});
      else if (def->Shared != f.Shared)
        errors.push_back({m.Name, "extern fn '@" + f.Name + "' is " +
                                      (f.Shared ? "" : "not ") +
                                      "shared but its definition in module '" +
                                      f.Module + "' is" +
                                      (def->Shared ? "" : " not")});
      else if (!(def->Sig == f.Sig))
        errors.push_back({m.Name, "extern fn '@" + f.Name + "' is declared " +
                                      sigStr(f.Sig) + " but module '" +
                                      f.Module + "' defines it " +
                                      sigStr(def->Sig)});
    }
    for (const Class &c : m.Classes) {
      if (!c.IsExtern || m.isCompiled(c))
        continue;
      auto it = modules.find(c.Module);
      if (it == modules.end()) {
        errors.push_back({m.Name, "extern class '" + c.Name +
                                      "' names module '" + c.Module +
                                      "', which is not in the program"});
        continue;
      }
      const Class *def = it->second->findClass(c.Name);
      if (!def || !it->second->defines(*def)) {
        errors.push_back({m.Name, "extern class '" + c.Name +
                                      "' is not defined in module '" +
                                      c.Module + "'"});
        continue;
      }
      bool same = def->Fields.size() == c.Fields.size();
      for (size_t i = 0; same && i < c.Fields.size(); ++i)
        same = def->Fields[i].Name == c.Fields[i].Name &&
               def->Fields[i].Ty == c.Fields[i].Ty &&
               def->Fields[i].Size == c.Fields[i].Size &&
               def->Fields[i].Align == c.Fields[i].Align;
      if (!same)
        errors.push_back({m.Name, "extern class '" + c.Name +
                                      "' has a different layout than its "
                                      "definition in module '" +
                                      c.Module + "'"});
    }
  }
  if (!p.Modules.empty() && !p.Modules.front().findFunction("main"))
    errors.push_back(
        {p.Modules.front().Name, "main module defines no '@main'"});
}

} // namespace

std::vector<VerifyError> verify(const Module &m) {
  std::vector<VerifyError> errors;
  ModuleVerifier(m, errors).run();
  return errors;
}

std::vector<VerifyError> verify(const Program &p) {
  std::vector<VerifyError> errors;
  for (const Module &m : p.Modules)
    ModuleVerifier(m, errors).run();
  verifyProgramLinks(p, errors);
  return errors;
}

} // namespace paykan::pir
