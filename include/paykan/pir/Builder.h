// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// pir::Builder — appends instructions and structured statements to a PIR
// function.  Used by the AST->PIR lowering and by tests that construct PIR by
// hand.  Header-only, standard C++ only.

#pragma once

#include "paykan/pir/PIR.h"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace paykan::pir {

/// A typed operand: what expression emission produces.  `Ty == Void` means
/// "no value" (a void call, or a failed emission).
struct Val {
  Operand Op = Operand::i64(0);
  Type Ty = Type::Void;

  Val() = default;
  Val(Operand op, Type ty) : Op(std::move(op)), Ty(ty) {}

  explicit operator bool() const { return Ty != Type::Void; }
  bool isValue() const { return Op.isValue(); }
  ValueId id() const { return Op.valueId(); }

  static Val i64(int64_t x) { return {Operand::i64(x), Type::I64}; }
  static Val f64(double x) { return {Operand::f64(x), Type::F64}; }
  static Val boolean(bool x) { return {Operand::boolean(x), Type::Bool}; }
  static Val chr(char x) { return {Operand::chr(x), Type::Char}; }
  static Val null(Type t) { return {Operand::null(t), t}; }
  static Val symbol(std::string name, Type t) {
    return {Operand::symbol(std::move(name)), t};
  }
};

class Builder {
  Function *Fn = nullptr;
  /// Innermost block last.  Statements are appended to the back.
  std::vector<Block *> Stack;
  /// Statements emitted after a terminator go here and are discarded, so the
  /// lowering never has to check reachability itself (mirrors emitting into
  /// a dead LLVM block).
  Block Dead;
  /// While nesting depth, for the verifier-visible break/continue rule.
  unsigned LoopDepth = 0;

  /// A statement after which nothing runs: break / continue / ret /
  /// unreachable, or an if whose two branches both end that way.
  static bool isTerminator(const Stmt &s) {
    if (auto *i = std::get_if<If>(&s))
      return i->Else && endsInTerminator(*i->Then) &&
             endsInTerminator(*i->Else);
    return std::holds_alternative<Break>(s) ||
           std::holds_alternative<Continue>(s) ||
           std::holds_alternative<Return>(s) ||
           std::holds_alternative<Unreachable>(s);
  }
  static bool endsInTerminator(const Block &b) {
    return !b.Stmts.empty() && isTerminator(b.Stmts.back());
  }

  Block &target() {
    assert(!Stack.empty() && "no insertion block");
    Block &b = *Stack.back();
    // Dead code accumulates until the next function; nested statements may
    // still point into it, so it is never cleared mid-function.
    if (endsInTerminator(b))
      return Dead;
    return b;
  }

public:
  Builder() = default;
  explicit Builder(Function *fn) { setFunction(fn); }

  void setFunction(Function *fn) {
    Fn = fn;
    Stack.clear();
    Dead.Stmts.clear();
    LoopDepth = 0;
    if (fn)
      Stack.push_back(&fn->Body);
  }
  Function *function() const { return Fn; }

  /// The builder's position (function, block stack, loop depth), saved and
  /// restored around the emission of another function's body.
  struct State {
    Function *Fn = nullptr;
    std::vector<Block *> Stack;
    unsigned LoopDepth = 0;
  };
  State saveState() const { return {Fn, Stack, LoopDepth}; }
  void restoreState(State st) {
    Fn = st.Fn;
    Stack = std::move(st.Stack);
    LoopDepth = st.LoopDepth;
  }

  /// True when the current block already ends in a terminator (anything
  /// emitted now is dead).
  bool isTerminated() const {
    return Stack.empty() || endsInTerminator(*Stack.back());
  }

  Value newValue(Type ty, std::string name = "") {
    assert(Fn);
    return Value{Fn->NextValueId++, ty, std::move(name)};
  }

  LocalId addLocal(std::string name, Type ty) {
    assert(Fn);
    Fn->Locals.push_back({std::move(name), ty});
    return static_cast<LocalId>(Fn->Locals.size() - 1);
  }
  Type localType(LocalId id) const { return Fn->Locals[id].Ty; }

  // -- Raw statement emission ------------------------------------------------

  /// Append an instruction; returns its result (Void-typed Val when none).
  Val emit(Instr instr) {
    Type ty = instr.Result.Ty;
    ValueId id = instr.Result.Id;
    target().Stmts.push_back(std::move(instr));
    if (ty == Type::Void)
      return Val();
    return Val(Operand::value(id), ty);
  }

  void emitBreak() {
    assert(LoopDepth > 0 && "break outside loop");
    target().Stmts.push_back(Break{});
  }
  void emitContinue() {
    assert(LoopDepth > 0 && "continue outside loop");
    target().Stmts.push_back(Continue{});
  }
  void emitRet(const Val &v) { target().Stmts.push_back(Return{v.Op}); }
  void emitRetVoid() { target().Stmts.push_back(Return{std::nullopt}); }
  void emitUnreachable() { target().Stmts.push_back(Unreachable{}); }

  // -- Structured control flow ------------------------------------------------
  //
  //   If *s = openIf(cond, /*withElse=*/true);
  //   enter(*s->Then); ... leave();
  //   enter(*s->Else); ... leave();
  //
  // A block opened while the current block is terminated is attached to the
  // dead block and discarded, so the lowering may keep emitting structure.

  If *openIf(const Val &cond, bool withElse) {
    If s;
    s.Cond = cond.Op;
    s.Then = std::make_unique<Block>();
    if (withElse)
      s.Else = std::make_unique<Block>();
    Block &t = target();
    t.Stmts.push_back(std::move(s));
    return &std::get<If>(t.Stmts.back());
  }

  /// Open a while: the caller enters CondBlock, emits the condition, calls
  /// setWhileCond, leaves, then enters Body.
  While *openWhile() {
    While s;
    s.CondBlock = std::make_unique<Block>();
    s.Body = std::make_unique<Block>();
    s.Cond = Operand::boolean(false);
    Block &t = target();
    t.Stmts.push_back(std::move(s));
    return &std::get<While>(t.Stmts.back());
  }
  void setWhileCond(While *w, const Val &cond) { w->Cond = cond.Op; }

  void enter(Block &b) { Stack.push_back(&b); }
  void leave() {
    assert(Stack.size() > 1 && "cannot leave the function body");
    Stack.pop_back();
  }
  void enterLoopBody(Block &b) {
    enter(b);
    ++LoopDepth;
  }
  void leaveLoopBody() {
    leave();
    --LoopDepth;
  }

  // -- Instruction helpers
  // ------------------------------------------------------

  Val binary(Opcode op, const Val &a, const Val &b, std::string name = "") {
    Instr i;
    i.Op = op;
    i.Result = newValue(a.Ty, std::move(name));
    i.Args = {a.Op, b.Op};
    return emit(std::move(i));
  }
  Val unary(Opcode op, const Val &a, std::string name = "") {
    Instr i;
    i.Op = op;
    i.Result = newValue(a.Ty, std::move(name));
    i.Args = {a.Op};
    return emit(std::move(i));
  }
  Val cmp(CmpPred pred, const Val &a, const Val &b, std::string name = "") {
    Instr i;
    i.Op = Opcode::Cmp;
    i.Pred = pred;
    i.Result = newValue(Type::Bool, std::move(name));
    i.Args = {a.Op, b.Op};
    return emit(std::move(i));
  }
  Val select(const Val &c, const Val &a, const Val &b, std::string name = "") {
    Instr i;
    i.Op = Opcode::Select;
    i.Result = newValue(a.Ty, std::move(name));
    i.Args = {c.Op, a.Op, b.Op};
    return emit(std::move(i));
  }
  Val itof(const Val &a, std::string name = "") {
    Instr i;
    i.Op = Opcode::IToF;
    i.Result = newValue(Type::F64, std::move(name));
    i.Args = {a.Op};
    return emit(std::move(i));
  }
  Val cast(const Val &a, Type to, std::string name = "") {
    if (a.Ty == to)
      return a;
    Instr i;
    i.Op = Opcode::Cast;
    i.CastTo = to;
    i.Result = newValue(to, std::move(name));
    i.Args = {a.Op};
    return emit(std::move(i));
  }

  Val call(const std::string &callee, const Signature &sig,
           const std::vector<Val> &args, std::string name = "") {
    Instr i;
    i.Op = Opcode::Call;
    i.Callee = callee;
    if (sig.Ret != Type::Void)
      i.Result = newValue(sig.Ret, std::move(name));
    for (auto &a : args)
      i.Args.push_back(a.Op);
    return emit(std::move(i));
  }
  Val vcall(const Val &recv, const std::string &className, uint32_t slot,
            const Signature &sig, const std::vector<Val> &args,
            std::string name = "") {
    Instr i;
    i.Op = Opcode::VCall;
    i.ClassName = className;
    i.Slot = slot;
    i.Sig = sig;
    if (sig.Ret != Type::Void)
      i.Result = newValue(sig.Ret, std::move(name));
    i.Args.push_back(recv.Op);
    for (auto &a : args)
      i.Args.push_back(a.Op);
    return emit(std::move(i));
  }

  void retain(const Val &box) {
    Instr i;
    i.Op = Opcode::Retain;
    i.Args = {box.Op};
    emit(std::move(i));
  }
  void release(const Val &box) {
    Instr i;
    i.Op = Opcode::Release;
    i.Args = {box.Op};
    emit(std::move(i));
  }
  Val box(const Val &obj, std::string name = "") {
    Instr i;
    i.Op = Opcode::Box;
    i.Result = newValue(Type::Box, std::move(name));
    i.Args = {obj.Op};
    return emit(std::move(i));
  }
  Val unbox(const Val &b, std::string name = "") {
    Instr i;
    i.Op = Opcode::Unbox;
    i.Result = newValue(Type::Obj, std::move(name));
    i.Args = {b.Op};
    return emit(std::move(i));
  }
  Val newObject(const std::string &className, std::string name = "") {
    Instr i;
    i.Op = Opcode::New;
    i.ClassName = className;
    i.Result = newValue(Type::Obj, std::move(name));
    return emit(std::move(i));
  }
  void freeObject(const Val &obj) {
    Instr i;
    i.Op = Opcode::Free;
    i.Args = {obj.Op};
    emit(std::move(i));
  }
  Val fieldLoad(const Val &obj, const std::string &className,
                const std::string &field, Type fieldTy, std::string name = "") {
    Instr i;
    i.Op = Opcode::FieldLoad;
    i.ClassName = className;
    i.Field = field;
    i.Result = newValue(fieldTy, std::move(name));
    i.Args = {obj.Op};
    return emit(std::move(i));
  }
  void fieldStore(const Val &obj, const std::string &className,
                  const std::string &field, const Val &v) {
    Instr i;
    i.Op = Opcode::FieldStore;
    i.ClassName = className;
    i.Field = field;
    i.Args = {obj.Op, v.Op};
    emit(std::move(i));
  }
  Val vtableLoad(const Val &obj, std::string name = "") {
    Instr i;
    i.Op = Opcode::VTableLoad;
    i.Result = newValue(Type::Ptr, std::move(name));
    i.Args = {obj.Op};
    return emit(std::move(i));
  }
  /// `vtable.addr Class`: the address of a class's vtable.
  Val vtableAddr(const std::string &className, std::string name = "") {
    Instr i;
    i.Op = Opcode::VTableAddr;
    i.ClassName = className;
    i.Result = newValue(Type::Ptr, std::move(name));
    return emit(std::move(i));
  }
  /// `vtable.addr @sym`: the address of an extern (runtime) vtable global,
  /// given as the symbol operand.
  Val vtableAddrExtern(const Val &symbol, std::string name = "") {
    Instr i;
    i.Op = Opcode::VTableAddr;
    i.Args = {symbol.Op};
    i.Result = newValue(Type::Ptr, std::move(name));
    return emit(std::move(i));
  }

  Val load(LocalId local, std::string name = "") {
    Instr i;
    i.Op = Opcode::Load;
    i.Local = local;
    i.Result = newValue(localType(local), std::move(name));
    return emit(std::move(i));
  }
  void store(LocalId local, const Val &v) {
    Instr i;
    i.Op = Opcode::Store;
    i.Local = local;
    i.Args = {v.Op};
    emit(std::move(i));
  }
};

} // namespace paykan::pir
