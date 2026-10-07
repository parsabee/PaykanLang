// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
bool isPointerLike(Type t) {
  return t == Type::Box || t == Type::Obj || t == Type::Ptr;
}
bool isFieldType(Type t) {
  return t == Type::I64 || t == Type::F64 || t == Type::Bool ||
         t == Type::Char || t == Type::Box;
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
                           " (fields are i64, f64, bool, char or box)");
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
        } else if (!(it->second->Sig == e.Sig)) {
          error(where, "class '" + c.Name + "' vtable slot " +
                           std::to_string(i) + " ('" + e.Slot +
                           "') has signature " + sigStr(e.Sig) + " but '@" +
                           e.Target + "' is declared " +
                           sigStr(it->second->Sig));
        }
      }
    }

    for (const Function &f : m.Functions) {
      if (f.IsExtern) {
        if (!f.Params.empty() || !f.Locals.empty() || !f.Body.Stmts.empty())
          error(where, "extern function '@" + f.Name + "' has a body");
        if (!f.Symbol.empty() && f.Module.empty())
          error(where, "runtime extern function '@" + f.Name +
                           "' has a 'symbol' (only a module extern may)");
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
                   (f.Sig.Params.size() == 1 && f.Sig.Params[0] == Type::Box));
        if (!ok)
          error(where, "'@main' must have signature () -> i64 or "
                       "(box) -> i64, not " +
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
      if (v.Ty == Type::Void)
        error("value '%" + std::to_string(v.Id) + "' has type void");
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
        expectArgs(
            in.Args, 0, sig,
            "vcall slot " + std::to_string(in.Slot) +
                (in.ClassName.empty() ? "" : " of '" + in.ClassName + "'"));
        result(in, sig.Ret);
        break;
      }
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
        } else {
          result(in, F.Locals[in.Local].Ty);
        }
        break;
      case Opcode::Store:
        if (!arity(in, 1))
          break;
        if (in.Local >= F.Locals.size())
          error("store to undeclared local " + std::to_string(in.Local));
        else
          expect(in.Args[0], F.Locals[in.Local].Ty, "stored value");
        result(in, Type::Void);
        break;
      }
    }

    /// Verifies a block; returns true when control cannot fall out of its
    /// end (it ends in ret / unreachable, or an if whose both branches do).
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

/// Cross-module checks: every extern that names a module must match a
/// definition in that module.
void verifyProgramLinks(const Program &p, std::vector<VerifyError> &errors) {
  std::unordered_map<std::string, const Module *> modules;
  for (const Module &m : p.Modules) {
    if (!modules.emplace(m.Name, &m).second)
      errors.push_back({m.Name, "module appears twice in the program"});
  }
  for (const Module &m : p.Modules) {
    for (const Function &f : m.Functions) {
      if (!f.IsExtern || f.Module.empty())
        continue;
      auto it = modules.find(f.Module);
      if (it == modules.end()) {
        errors.push_back({m.Name, "extern fn '@" + f.Name + "' names module '" +
                                      f.Module +
                                      "', which is not in the program"});
        continue;
      }
      const Function *def = it->second->findFunction(f.linkName());
      if (!def || def->IsExtern)
        errors.push_back({m.Name, "extern fn '@" + f.linkName() +
                                      "' is not defined in module '" +
                                      f.Module + "'"});
      else if (!(def->Sig == f.Sig))
        errors.push_back({m.Name, "extern fn '@" + f.Name + "' is declared " +
                                      sigStr(f.Sig) + " but module '" +
                                      f.Module + "' defines it " +
                                      sigStr(def->Sig)});
    }
    for (const Class &c : m.Classes) {
      if (!c.IsExtern)
        continue;
      auto it = modules.find(c.Module);
      if (it == modules.end()) {
        errors.push_back({m.Name, "extern class '" + c.Name +
                                      "' names module '" + c.Module +
                                      "', which is not in the program"});
        continue;
      }
      const Class *def = it->second->findClass(c.Name);
      if (!def || def->IsExtern) {
        errors.push_back({m.Name, "extern class '" + c.Name +
                                      "' is not defined in module '" +
                                      c.Module + "'"});
        continue;
      }
      bool same = def->Fields.size() == c.Fields.size();
      for (size_t i = 0; same && i < c.Fields.size(); ++i)
        same = def->Fields[i].Name == c.Fields[i].Name &&
               def->Fields[i].Ty == c.Fields[i].Ty;
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
