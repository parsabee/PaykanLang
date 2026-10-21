// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The shared lowering (docs/design/mlir-backend.md): one imported PIR module
// (the `pir` dialect) rewritten into func, scf and arith, keeping the pir
// ops that no upstream dialect lowers to both LLVM and EmitC.  What is
// PaykanLang's own is decided here, once, for both targets:
//
// * Symbols get their link names (linkName(), vtableName()); runtime and
//   native functions keep their C names.
// * Every class's layout is computed as C lays out the object (the 16-byte
//   header, then the fields at their alignment, docs/pir.md §5): a field is
//   the object's address plus its offset, read and written with
//   pir.ptr_load / pir.ptr_store; `new` allocates the size and zeroes it;
//   the vtable is a pir.vtable global, and a vcall loads the slot and calls
//   it with pir.icall.
// * ARC, allocation and weak references are runtime calls (func.call).
// * Locals are pir.alloca slots.
// * Structured control flow becomes scf.if / scf.while.  SCF has no break,
//   continue or early return: a function using them gets an exit flag (a
//   slot: 0 running, 1 break, 2 continue, 3 return) that those statements
//   set; what follows a statement that may have set it runs under scf.if on
//   the flag being 0, a loop's condition stops the loop once it is set, and
//   a returned value goes into a slot the function returns at its end.  A
//   function whose returns are all in tail position needs none of it.
// * A switch becomes scf.index_switch, which each target makes its own
//   switch.

#include "PIRToStandard.h"

#include "Dialect/PIRDialect.h"
#include "Names.h"
#include "paykan/pir/PIR.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/TypeSwitch.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/SCF/IR/SCF.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/IRMapping.h>
#include <mlir/IR/SymbolTable.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace paykan::mlir_backend {

using namespace ::mlir;
namespace P = ::paykan::pir_dialect;

// -- Link names

namespace {

void escape(llvm::StringRef s, std::string &out) {
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
      out += static_cast<char>(c);
    } else if (c == '_') {
      out += "__";
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "_%02X", c);
      out += buf;
    }
  }
}

std::string mangled(const char *prefix, llvm::StringRef module,
                    llvm::StringRef name) {
  std::string out = prefix;
  escape(module, out);
  out += "_S";
  escape(name, out);
  return out;
}

} // namespace

std::string linkName(llvm::StringRef module, llvm::StringRef name) {
  return mangled("pk_", module, name);
}

std::string vtableName(llvm::StringRef module, llvm::StringRef name) {
  return mangled("pkvt_", module, name);
}

namespace {

// -- Layouts

/// Bytes and alignment of a value of type @p t (or a `mem` slot) in memory.
std::pair<int64_t, int64_t> sizeAlign(Type t) {
  if (auto mem = dyn_cast<P::MemType>(t))
    return {mem.getSize(), mem.getAlign()};
  if (auto opt = dyn_cast<P::OptType>(t)) {
    // { bool present; <payload> value; } as C lays it out.
    auto [s, a] = sizeAlign(opt.getPayload());
    return {a + s, a};
  }
  if (t.isInteger(1) || t.isInteger(8))
    return {1, 1};
  return {8, 8}; // i64, f64 and every pointer (LP64, docs/pir.md)
}

int64_t alignTo(int64_t n, int64_t a) { return (n + a - 1) / a * a; }

/// The runtime header of every object (Runtime.h, PaykanObject): the
/// vtable pointer, then the 32-bit strong and weak counts.
constexpr int64_t kHeaderSize = 16;

struct ClassLayout {
  llvm::StringMap<std::pair<int64_t, Type>> Fields; // name -> (offset, type)
  int64_t Size = kHeaderSize;
  std::string VTable;
};

/// What a statement may leave by: the exits of an enclosing construct it
/// does not complete.
enum Exit : unsigned { kNone = 0, kBreak = 1, kContinue = 2, kReturn = 4 };

/// The exit flag's values.
enum Flag : int64_t {
  kRunning = 0,
  kBreaking = 1,
  kContinuing = 2,
  kReturning = 3
};

class ModuleLowering;

/// The lowering of one pir.func into a func.func.
class FunctionLowering {
public:
  FunctionLowering(ModuleLowering &m, P::FuncOp src, func::FuncOp dst);
  void run();

private:
  ModuleLowering &M;
  P::FuncOp Src;
  func::FuncOp Dst;
  OpBuilder B;
  Location Loc;
  IRMapping Map;
  /// pir.local value -> its slot's type.
  llvm::DenseMap<Value, Type> SlotTypes;
  llvm::DenseMap<Operation *, unsigned> ExitCache;
  Value ExitSlot; // the exit flag (null in a function without one)
  Value RetSlot;  // the returned value (null when not needed)
  Type RetType;   // the function's result type (null for void)

  // -- Analysis
  unsigned exits(Operation *op);
  unsigned exits(Region &r);
  bool tailOnly(Block &b);

  // -- Helpers
  Value lookup(Value v) { return Map.lookup(v); }
  Value i64(int64_t v) {
    return arith::ConstantIntOp::create(B, Loc, B.getI64Type(), v);
  }
  Value flagConst(int64_t v) {
    return arith::ConstantIntOp::create(B, Loc, B.getI8Type(), v);
  }
  void setFlag(int64_t v) {
    P::PtrStoreOp::create(B, Loc, ExitSlot, flagConst(v));
  }
  Value flagIs(int64_t v) {
    Value f = P::PtrLoadOp::create(B, Loc, B.getI8Type(), ExitSlot);
    return arith::CmpIOp::create(B, Loc, arith::CmpIPredicate::eq, f,
                                 flagConst(v));
  }
  /// scf.if (flag == v) { flag = 0 }
  void resetFlagIf(int64_t v);
  Value zeroOf(Type t);
  Value asPtr(Value v);

  // -- Statements
  void lowerBlock(Block &b);
  void lowerOp(Operation *op);
  Value lowerTail(Block &b);
  void lowerIf(P::IfOp op);
  void lowerSwitch(P::SwitchOp op) {
    lowerSwitchWith(op, Type(), [&](Region *r) {
      if (r && !r->empty())
        lowerBlock(r->front());
      return Value();
    });
  }
  /// The dispatch of @p op, each region (null: an absent default) lowered
  /// by @p fill, which returns the value the region yields when @p result
  /// is a type: the switch's value of that type, else null.
  Value lowerSwitchWith(P::SwitchOp op, Type result,
                        llvm::function_ref<Value(Region *)> fill);
  /// Fill @p to, a block of an scf op, with @p fill(@p from) and the yield
  /// of what it returns (a result-less scf.if's blocks have theirs).
  void fillCase(Block *to, Region *from, Type result,
                llvm::function_ref<Value(Region *)> fill);
  void lowerWhile(P::WhileOp op);
  void lowerInstr(Operation *op);
  void define(Operation *src, Value v) {
    if (v)
      Map.map(src->getResult(0), v);
  }
  /// The value of a call-like op, or null for a void one.
  static Value resultOf(Operation *op) {
    return op->getNumResults() ? op->getResult(0) : Value();
  }
  std::vector<Value> mapped(OperandRange operands);
  Value fieldAddr(Value obj, StringRef className, StringRef field, Type &ty);
};

/// The lowering of one module.
class ModuleLowering {
public:
  explicit ModuleLowering(ModuleOp module)
      : Mod(module), Ctx(module.getContext()), B(Ctx), Loc(module.getLoc()) {}

  LogicalResult run();

  // -- What FunctionLowering looks up
  std::string functionLinkName(StringRef pirName) const {
    auto it = FunctionNames.find(pirName);
    return it == FunctionNames.end() ? pirName.str() : it->second;
  }
  std::string symbolLinkName(StringRef pirName) const {
    auto it = SymbolNames.find(pirName);
    return it == SymbolNames.end() ? pirName.str() : it->second;
  }
  const ClassLayout *layout(StringRef className) const {
    auto it = Layouts.find(className);
    return it == Layouts.end() ? nullptr : &it->second;
  }
  /// A runtime (or C library) function, declared in the module on first use
  /// (with kRuntimeAttr unless @p isRuntime is false: an extern that is not
  /// the runtime's, a native or another module's function).
  func::FuncOp runtime(StringRef name, FunctionType type,
                       bool isRuntime = true);
  void fail(const Twine &message) {
    if (Error.empty())
      Error = message.str();
  }

private:
  ModuleOp Mod;
  MLIRContext *Ctx;
  OpBuilder B;
  Location Loc;
  std::string Name;
  bool IsMain = false;
  llvm::StringMap<std::string> FunctionNames; // pir name -> link name
  llvm::StringMap<std::string> SymbolNames;   // globals: pir name -> link name
  llvm::StringMap<ClassLayout> Layouts;
  llvm::StringMap<func::FuncOp> Declared; // link name -> declaration
  std::string Error;

  std::string definedName(StringRef name) const {
    if (IsMain && name == "main")
      return "main"; // the program's entry point
    return linkName(Name, name);
  }
  void layoutClass(P::ClassOp c);
};

// -- ModuleLowering

func::FuncOp ModuleLowering::runtime(StringRef name, FunctionType type,
                                     bool isRuntime) {
  if (auto it = Declared.find(name); it != Declared.end())
    return it->second;
  OpBuilder::InsertionGuard guard(B);
  B.setInsertionPointToStart(Mod.getBody());
  auto fn = func::FuncOp::create(B, Loc, name, type);
  fn.setPrivate();
  if (isRuntime)
    fn->setAttr(kRuntimeAttr, UnitAttr::get(Ctx));
  Declared[name] = fn;
  return fn;
}

void ModuleLowering::layoutClass(P::ClassOp c) {
  ClassLayout &l = Layouts[c.getName()];
  int64_t at = kHeaderSize;
  int64_t maxAlign = 8; // the header's pointer
  for (auto f : c.getBody().front().getOps<P::FieldOp>()) {
    auto [size, align] = sizeAlign(f.getType());
    at = alignTo(at, align);
    l.Fields[f.getName()] = {at, f.getType()};
    at += size;
    maxAlign = std::max(maxAlign, align);
  }
  l.Size = alignTo(at, maxAlign);
  std::optional<StringRef> module = c.getModule();
  l.VTable = c->hasAttr(kSharedAttr)
                 ? vtableName("", c.getName())
                 : vtableName(module ? *module : StringRef(Name), c.getName());
}

LogicalResult ModuleLowering::run() {
  std::optional<StringRef> name = Mod.getSymName();
  Name = name ? name->str() : std::string();
  IsMain = Mod->hasAttr(kMainModuleAttr);
  Block *body = Mod.getBody();

  // Names first: a body may reference any symbol of the module.
  for (auto c : body->getOps<P::ClassOp>())
    layoutClass(c);
  for (auto f : body->getOps<P::FuncOp>()) {
    std::string link;
    std::optional<StringRef> module = f.getModule();
    std::optional<StringRef> symbol = f.getLinkSymbol();
    if (f->hasAttr(kSharedAttr))
      link = linkName("", symbol ? *symbol : f.getSymName());
    else if (!f.isExternal())
      link = definedName(f.getSymName());
    else if (!module)
      link = ::paykan::pir::externCSymbol(f.getSymName());
    else
      link = linkName(*module, symbol ? *symbol : f.getSymName());
    FunctionNames[f.getSymName()] = link;
  }
  for (Operation &op : *body) {
    StringRef sym;
    std::string link;
    if (auto g = dyn_cast<P::CStrOp>(op))
      sym = g.getSymName();
    else if (auto g = dyn_cast<P::DataOp>(op))
      sym = g.getSymName();
    else if (auto g = dyn_cast<P::BytesOp>(op))
      sym = g.getSymName();
    else if (auto g = dyn_cast<P::ExternObjectOp>(op))
      link = ::paykan::pir::externCSymbol(g.getSymName());
    else if (auto g = dyn_cast<P::ExternVTableOp>(op))
      link = ::paykan::pir::externCSymbol(g.getSymName());
    else
      continue;
    StringRef pirName = SymbolTable::getSymbolName(&op);
    // A module-less extern global is the runtime's (Runtime.h declares it).
    if (!link.empty() && !::paykan::pir::isNativeName(pirName))
      op.setAttr(kRuntimeAttr, UnitAttr::get(Ctx));
    SymbolNames[pirName] = link.empty() ? linkName(Name, sym) : link;
    SymbolTable::setSymbolName(&op, SymbolNames[pirName]);
  }

  std::vector<Operation *> erase;
  B.setInsertionPointToEnd(body);
  for (auto c : body->getOps<P::ClassOp>()) {
    const ClassLayout &l = Layouts[c.getName()];
    if (c.getIsExtern()) {
      P::ExternVTableOp::create(B, Loc, l.VTable);
    } else {
      std::vector<Attribute> entries;
      for (auto s : c.getBody().front().getOps<P::SlotOp>()) {
        if (auto t = s.getTarget())
          entries.push_back(FlatSymbolRefAttr::get(Ctx, functionLinkName(*t)));
        else
          entries.push_back(UnitAttr::get(Ctx));
      }
      P::VTableOp::create(B, Loc, l.VTable, B.getArrayAttr(entries));
    }
    erase.push_back(c);
  }

  // Declarations, then definitions (bodies may call anything declared).
  std::vector<std::pair<P::FuncOp, func::FuncOp>> bodies;
  for (auto f : body->getOps<P::FuncOp>()) {
    std::string link = FunctionNames[f.getSymName()];
    erase.push_back(f);
    if (f.isExternal()) {
      runtime(link, f.getFunctionType(),
              !f.getModule() && !::paykan::pir::isNativeName(f.getSymName()));
      continue;
    }
    auto fn = func::FuncOp::create(B, Loc, link, f.getFunctionType());
    bodies.emplace_back(f, fn);
  }
  for (auto &[src, dst] : bodies)
    FunctionLowering(*this, src, dst).run();
  for (Operation *op : erase)
    op->erase();
  Mod->removeAttr(kMainModuleAttr);
  if (!Error.empty())
    return Mod.emitError("the shared lowering of module '")
           << Name << "' failed: " << Error;
  return success();
}

// -- FunctionLowering

FunctionLowering::FunctionLowering(ModuleLowering &m, P::FuncOp src,
                                   func::FuncOp dst)
    : M(m), Src(src), Dst(dst), B(dst.getContext()), Loc(src.getLoc()) {
  auto results = src.getFunctionType().getResults();
  if (!results.empty())
    RetType = results.front();
}

unsigned FunctionLowering::exits(Region &r) {
  unsigned e = kNone;
  for (Block &b : r)
    for (Operation &op : b)
      e |= exits(&op);
  return e;
}

unsigned FunctionLowering::exits(Operation *op) {
  if (auto it = ExitCache.find(op); it != ExitCache.end())
    return it->second;
  unsigned e = kNone;
  if (isa<P::BreakOp>(op))
    e = kBreak;
  else if (isa<P::ContinueOp>(op))
    e = kContinue;
  else if (isa<P::ReturnOp, P::UnreachableOp>(op))
    e = kReturn;
  else if (auto i = dyn_cast<P::IfOp>(op))
    e = exits(i.getThenRegion()) | exits(i.getElseRegion());
  else if (auto s = dyn_cast<P::SwitchOp>(op)) {
    e = exits(s.getDefaultRegion());
    for (Region &r : s.getCaseRegions())
      e |= exits(r);
  } else if (auto w = dyn_cast<P::WhileOp>(op))
    e = exits(w.getBodyRegion()) & kReturn; // the loop's own break/continue
  ExitCache[op] = e;
  return e;
}

/// Whether every exit of @p b is a return in tail position: the block's
/// last statement, or the last of a tail `if`'s or `switch`'s branch.
bool FunctionLowering::tailOnly(Block &b) {
  for (Operation &op : b) {
    bool last = &op == &b.back();
    if (!last || isa<P::WhileOp>(op)) {
      if (exits(&op) != kNone)
        return false;
      continue;
    }
    if (auto i = dyn_cast<P::IfOp>(op)) {
      for (Region *r : {&i.getThenRegion(), &i.getElseRegion()})
        if (!r->empty() && !tailOnly(r->front()))
          return false;
    } else if (isa<P::SwitchOp>(op)) {
      for (Region &r : op.getRegions())
        if (!r.empty() && !tailOnly(r.front()))
          return false;
    } else if (isa<P::BreakOp, P::ContinueOp>(op)) {
      return false;
    }
  }
  return true;
}

void FunctionLowering::run() {
  Block *entry = Dst.addEntryBlock();
  B.setInsertionPointToEnd(entry);
  Block &body = Src.getBody().front();
  for (auto [from, to] : llvm::zip(body.getArguments(), entry->getArguments()))
    Map.map(from, to);

  // The locals are slots at the top of the function, as PIR declares them.
  for (auto l : body.getOps<P::LocalOp>()) {
    Type slot = l.getSlot().getType().getSlot();
    auto [size, align] = sizeAlign(slot);
    Map.map(l.getSlot(),
            P::AllocaOp::create(B, Loc, size, align, l.getName()).getResult());
    SlotTypes[l.getSlot()] = slot;
  }

  // Without break, continue or an early return, no exit flag is needed.
  bool loopExits = false;
  Src.walk(
      [&](Operation *o) { loopExits |= isa<P::BreakOp, P::ContinueOp>(o); });
  if (!loopExits && tailOnly(body)) {
    Value r = lowerTail(body);
    func::ReturnOp::create(B, Loc, r ? ValueRange(r) : ValueRange());
    return;
  }
  ExitSlot = P::AllocaOp::create(B, Loc, 1, 1, "exit");
  setFlag(kRunning);
  if (RetType) {
    auto [size, align] = sizeAlign(RetType);
    RetSlot = P::AllocaOp::create(B, Loc, size, align, "ret");
  }
  lowerBlock(body);
  if (RetType)
    func::ReturnOp::create(
        B, Loc, ValueRange(P::PtrLoadOp::create(B, Loc, RetType, RetSlot)));
  else
    func::ReturnOp::create(B, Loc);
}

Value FunctionLowering::zeroOf(Type t) {
  if (isa<P::BoxType, P::ObjType, P::PtrType, P::WeakType>(t))
    return P::NullOp::create(B, Loc, t);
  if (isa<P::OptType>(t))
    return P::NoneOp::create(B, Loc, t);
  if (auto f = dyn_cast<FloatType>(t))
    return arith::ConstantFloatOp::create(B, Loc, f, llvm::APFloat(0.0));
  return arith::ConstantIntOp::create(B, Loc, t, 0);
}

Value FunctionLowering::asPtr(Value v) {
  if (isa<P::PtrType>(v.getType()))
    return v;
  return P::CastOp::create(B, Loc, P::PtrType::get(B.getContext()), v);
}

void FunctionLowering::resetFlagIf(int64_t v) {
  auto i = scf::IfOp::create(B, Loc, flagIs(v), /*withElseRegion=*/false);
  OpBuilder::InsertionGuard guard(B);
  B.setInsertionPointToStart(i.thenBlock());
  setFlag(kRunning);
}

/// The statements of a function whose exits are all tail returns: their
/// value, or null for a void function.
Value FunctionLowering::lowerTail(Block &b) {
  for (Operation &op : b) {
    if (&op != &b.back()) {
      lowerOp(&op);
      continue;
    }
    if (auto r = dyn_cast<P::ReturnOp>(op))
      return r.getValue() ? lookup(r.getValue()) : Value();
    if (isa<P::UnreachableOp>(op))
      return RetType ? zeroOf(RetType) : Value(); // never reached
    if (auto s = dyn_cast<P::SwitchOp>(op); s && exits(s) != kNone) {
      // A branch that falls through is a void function's (as an if's).
      return lowerSwitchWith(s, RetType, [&](Region *r) {
        Value v = r && !r->empty() ? lowerTail(r->front()) : Value();
        return RetType && !v ? zeroOf(RetType) : v;
      });
    }
    auto i = dyn_cast<P::IfOp>(op);
    if (!i || exits(i) == kNone) {
      lowerOp(&op);
      return Value(); // a void function falls off its end
    }
    auto branches =
        scf::IfOp::create(B, Loc, RetType ? TypeRange(RetType) : TypeRange(),
                          lookup(i.getCond()), /*withElseRegion=*/true);
    for (auto [from, to] :
         {std::pair(&i.getThenRegion(), branches.thenBlock()),
          std::pair(&i.getElseRegion(), branches.elseBlock())}) {
      OpBuilder::InsertionGuard guard(B);
      // A result-less scf.if comes with its yields; one with results not.
      if (to->empty())
        B.setInsertionPointToEnd(to);
      else
        B.setInsertionPoint(to->getTerminator());
      Value r = from->empty() ? Value() : lowerTail(from->front());
      if (RetType) {
        // A branch that falls through cannot happen in a non-void function
        // (PIR's verifier): only a void one has an empty branch.
        scf::YieldOp::create(B, Loc, ValueRange(r ? r : zeroOf(RetType)));
      }
    }
    return RetType ? branches.getResult(0) : Value();
  }
  return Value();
}

/// The statements of @p b, in a function with an exit flag: what follows a
/// statement that may set the flag runs only while it is clear.
void FunctionLowering::lowerBlock(Block &b) {
  OpBuilder::InsertionGuard guard(B);
  for (Operation &op : b) {
    lowerOp(&op);
    if (&op == &b.back() || exits(&op) == kNone)
      continue;
    if (isa<P::BreakOp, P::ContinueOp, P::ReturnOp, P::UnreachableOp>(op))
      break; // nothing follows these in PIR
    auto rest = scf::IfOp::create(B, Loc, flagIs(kRunning),
                                  /*withElseRegion=*/false);
    B.setInsertionPoint(rest.thenBlock()->getTerminator());
  }
}

void FunctionLowering::lowerIf(P::IfOp op) {
  bool hasElse = !op.getElseRegion().empty();
  auto i = scf::IfOp::create(B, Loc, lookup(op.getCond()), hasElse);
  OpBuilder::InsertionGuard guard(B);
  if (!op.getThenRegion().empty()) {
    B.setInsertionPoint(i.thenBlock()->getTerminator());
    lowerBlock(op.getThenRegion().front());
  }
  if (hasElse) {
    B.setInsertionPoint(i.elseBlock()->getTerminator());
    lowerBlock(op.getElseRegion().front());
  }
}

void FunctionLowering::fillCase(Block *to, Region *from, Type result,
                                llvm::function_ref<Value(Region *)> fill) {
  OpBuilder::InsertionGuard guard(B);
  bool yields = !to->empty();
  if (yields)
    B.setInsertionPoint(to->getTerminator());
  else
    B.setInsertionPointToEnd(to);
  Value v = fill(from);
  if (!yields)
    scf::YieldOp::create(B, Loc, result ? ValueRange(v) : ValueRange());
}

Value FunctionLowering::lowerSwitchWith(
    P::SwitchOp op, Type result, llvm::function_ref<Value(Region *)> fill) {
  Value v = lookup(op.getValue());
  // An i64 is its own case value; a char or a bool (i8, i1) is zero-extended,
  // as its cases are.
  SmallVector<int64_t> cases;
  for (Attribute c : op.getCases()) {
    const APInt &value = cast<IntegerAttr>(c).getValue();
    cases.push_back(value.getBitWidth() < 64
                        ? static_cast<int64_t>(value.getZExtValue())
                        : value.getSExtValue());
  }
  Value index =
      v.getType().getIntOrFloatBitWidth() < 64
          ? arith::IndexCastUIOp::create(B, Loc, B.getIndexType(), v)
                .getResult()
          : arith::IndexCastOp::create(B, Loc, B.getIndexType(), v).getResult();
  auto sw = scf::IndexSwitchOp::create(B, Loc,
                                       result ? TypeRange(result) : TypeRange(),
                                       index, cases, cases.size());
  for (auto [to, from] : llvm::zip(sw.getCaseRegions(), op.getCaseRegions()))
    fillCase(&to.emplaceBlock(), &from, result, fill);
  Region &rest = op.getDefaultRegion();
  fillCase(&sw.getDefaultRegion().emplaceBlock(),
           rest.empty() ? nullptr : &rest, result, fill);
  return result ? sw.getResult(0) : Value();
}

void FunctionLowering::lowerWhile(P::WhileOp op) {
  unsigned bodyExits = exits(op.getBodyRegion());
  auto w = scf::WhileOp::create(B, Loc, TypeRange(), ValueRange());
  {
    OpBuilder::InsertionGuard guard(B);
    Block *before = B.createBlock(&w.getBefore());
    Block &cond = op.getCondRegion().front();
    auto lowerCond = [&] {
      Value c;
      for (Operation &o : cond) {
        if (auto co = dyn_cast<P::CondOp>(o))
          c = lookup(co.getCond());
        else
          lowerOp(&o);
      }
      return c;
    };
    B.setInsertionPointToEnd(before);
    Value c;
    if (bodyExits == kNone) {
      c = lowerCond();
    } else {
      // Once the body has set the flag, the condition is not evaluated.
      auto guarded = scf::IfOp::create(B, Loc, TypeRange(B.getI1Type()),
                                       flagIs(kRunning), true);
      {
        OpBuilder::InsertionGuard inner(B);
        B.setInsertionPointToEnd(guarded.thenBlock());
        scf::YieldOp::create(B, Loc, ValueRange(lowerCond()));
        B.setInsertionPointToEnd(guarded.elseBlock());
        scf::YieldOp::create(
            B, Loc,
            ValueRange(arith::ConstantIntOp::create(B, Loc, B.getI1Type(), 0)));
      }
      c = guarded.getResult(0);
    }
    scf::ConditionOp::create(B, Loc, c, ValueRange());

    Block *after = B.createBlock(&w.getAfter());
    B.setInsertionPointToEnd(after);
    if (!op.getBodyRegion().empty())
      lowerBlock(op.getBodyRegion().front());
    if (bodyExits & kContinue)
      resetFlagIf(kContinuing); // the next iteration runs
    scf::YieldOp::create(B, Loc);
  }
  if (bodyExits & kBreak)
    resetFlagIf(kBreaking); // the statements after the loop run
}

void FunctionLowering::lowerOp(Operation *op) {
  if (isa<P::LocalOp>(op))
    return; // made at the top of the function
  if (auto i = dyn_cast<P::IfOp>(op))
    return lowerIf(i);
  if (auto s = dyn_cast<P::SwitchOp>(op))
    return lowerSwitch(s);
  if (auto w = dyn_cast<P::WhileOp>(op))
    return lowerWhile(w);
  if (isa<P::BreakOp>(op))
    return setFlag(kBreaking);
  if (isa<P::ContinueOp>(op))
    return setFlag(kContinuing);
  if (auto r = dyn_cast<P::ReturnOp>(op)) {
    if (r.getValue())
      P::PtrStoreOp::create(B, Loc, RetSlot, lookup(r.getValue()));
    return setFlag(kReturning);
  }
  if (isa<P::UnreachableOp>(op))
    return setFlag(kReturning);
  lowerInstr(op);
}

std::vector<Value> FunctionLowering::mapped(OperandRange operands) {
  std::vector<Value> vs;
  vs.reserve(operands.size());
  for (Value v : operands)
    vs.push_back(lookup(v));
  return vs;
}

Value FunctionLowering::fieldAddr(Value obj, StringRef className,
                                  StringRef field, Type &ty) {
  if (const ClassLayout *l = M.layout(className)) {
    if (auto it = l->Fields.find(field); it != l->Fields.end()) {
      ty = it->second.second;
      return P::PtrAddOp::create(B, Loc, asPtr(lookup(obj)),
                                 i64(it->second.first));
    }
  }
  M.fail("no field '" + field + "' in class '" + className + "'");
  ty = B.getI64Type();
  return asPtr(lookup(obj));
}

void FunctionLowering::lowerInstr(Operation *op) {
  MLIRContext *ctx = B.getContext();
  Type box = P::BoxType::get(ctx), obj = P::ObjType::get(ctx),
       ptr = P::PtrType::get(ctx), weak = P::WeakType::get(ctx),
       f64 = B.getF64Type();
  auto fnType = [&](TypeRange in, TypeRange out) {
    return B.getFunctionType(in, out);
  };
  auto callRuntime = [&](const char *name, TypeRange in, TypeRange out,
                         ValueRange args) {
    func::FuncOp fn = M.runtime(name, fnType(in, out));
    return func::CallOp::create(B, Loc, fn, args);
  };
  auto operand = [&](unsigned i) { return lookup(op->getOperand(i)); };
  auto isFloat = [](Value v) { return isa<FloatType>(v.getType()); };

  llvm::TypeSwitch<Operation *>(op)
      // Constants: as imported.
      .Case<arith::ConstantOp>([&](auto) { B.clone(*op, Map); })
      .Case<P::NullOp>([&](auto) { B.clone(*op, Map); })
      .Case([&](P::AddressOfOp o) {
        define(op, P::AddressOfOp::create(B, Loc, o.getType(),
                                          M.symbolLinkName(o.getSymbol())));
      })
      // Arithmetic.
      .Case([&](P::AddOp) {
        define(
            op,
            isFloat(operand(0))
                ? Value(arith::AddFOp::create(B, Loc, operand(0), operand(1)))
                : Value(arith::AddIOp::create(B, Loc, operand(0), operand(1))));
      })
      .Case([&](P::SubOp) {
        define(
            op,
            isFloat(operand(0))
                ? Value(arith::SubFOp::create(B, Loc, operand(0), operand(1)))
                : Value(arith::SubIOp::create(B, Loc, operand(0), operand(1))));
      })
      .Case([&](P::MulOp) {
        define(
            op,
            isFloat(operand(0))
                ? Value(arith::MulFOp::create(B, Loc, operand(0), operand(1)))
                : Value(arith::MulIOp::create(B, Loc, operand(0), operand(1))));
      })
      .Case([&](P::DivOp) {
        define(op, isFloat(operand(0)) ? Value(arith::DivFOp::create(
                                             B, Loc, operand(0), operand(1)))
                                       : Value(arith::DivSIOp::create(
                                             B, Loc, operand(0), operand(1))));
      })
      .Case([&](P::RemOp) {
        // A float remainder is C's fmod (LLVM's frem lowers to it too).
        define(op, isFloat(operand(0))
                       ? resultOf(callRuntime("fmod", {f64, f64}, {f64},
                                              {operand(0), operand(1)}))
                       : Value(arith::RemSIOp::create(B, Loc, operand(0),
                                                      operand(1))));
      })
      .Case([&](P::UDivOp) {
        define(op, arith::DivUIOp::create(B, Loc, operand(0), operand(1)));
      })
      .Case([&](P::URemOp) {
        define(op, arith::RemUIOp::create(B, Loc, operand(0), operand(1)));
      })
      .Case([&](P::NegOp) {
        Value a = operand(0);
        define(op, isFloat(a)
                       ? Value(arith::NegFOp::create(B, Loc, a))
                       : Value(arith::SubIOp::create(B, Loc, i64(0), a)));
      })
      .Case([&](P::NotOp) {
        define(op, arith::XOrIOp::create(
                       B, Loc, operand(0),
                       arith::ConstantIntOp::create(B, Loc, B.getI1Type(), 1)));
      })
      .Case([&](P::CmpOp o) {
        Value a = operand(0), b = operand(1);
        if (isFloat(a)) {
          // IEEE 754: `ne` is unordered, the rest ordered (docs/pir.md).
          arith::CmpFPredicate p = arith::CmpFPredicate::OEQ;
          switch (o.getPredicate()) {
          case P::CmpPredicate::eq:
            p = arith::CmpFPredicate::OEQ;
            break;
          case P::CmpPredicate::ne:
            p = arith::CmpFPredicate::UNE;
            break;
          case P::CmpPredicate::lt:
            p = arith::CmpFPredicate::OLT;
            break;
          case P::CmpPredicate::le:
            p = arith::CmpFPredicate::OLE;
            break;
          case P::CmpPredicate::gt:
            p = arith::CmpFPredicate::OGT;
            break;
          case P::CmpPredicate::ge:
            p = arith::CmpFPredicate::OGE;
            break;
          }
          return define(op, arith::CmpFOp::create(B, Loc, p, a, b));
        }
        if (!a.getType().isIntOrIndex()) {
          // Pointers compare as their addresses.
          a = P::CastOp::create(B, Loc, B.getI64Type(), a);
          b = P::CastOp::create(B, Loc, B.getI64Type(), b);
        }
        arith::CmpIPredicate p = arith::CmpIPredicate::eq;
        switch (o.getPredicate()) {
        case P::CmpPredicate::eq:
          p = arith::CmpIPredicate::eq;
          break;
        case P::CmpPredicate::ne:
          p = arith::CmpIPredicate::ne;
          break;
        case P::CmpPredicate::lt:
          p = arith::CmpIPredicate::slt;
          break;
        case P::CmpPredicate::le:
          p = arith::CmpIPredicate::sle;
          break;
        case P::CmpPredicate::gt:
          p = arith::CmpIPredicate::sgt;
          break;
        case P::CmpPredicate::ge:
          p = arith::CmpIPredicate::sge;
          break;
        }
        define(op, arith::CmpIOp::create(B, Loc, p, a, b));
      })
      .Case([&](P::UCmpOp o) {
        arith::CmpIPredicate p = arith::CmpIPredicate::eq;
        switch (o.getPredicate()) {
        case P::CmpPredicate::eq:
          p = arith::CmpIPredicate::eq;
          break;
        case P::CmpPredicate::ne:
          p = arith::CmpIPredicate::ne;
          break;
        case P::CmpPredicate::lt:
          p = arith::CmpIPredicate::ult;
          break;
        case P::CmpPredicate::le:
          p = arith::CmpIPredicate::ule;
          break;
        case P::CmpPredicate::gt:
          p = arith::CmpIPredicate::ugt;
          break;
        case P::CmpPredicate::ge:
          p = arith::CmpIPredicate::uge;
          break;
        }
        define(op, arith::CmpIOp::create(B, Loc, p, operand(0), operand(1)));
      })
      .Case([&](P::SelectOp) {
        define(op, arith::SelectOp::create(B, Loc, operand(0), operand(1),
                                           operand(2)));
      })
      .Case([&](P::IToFOp) {
        define(op, arith::SIToFPOp::create(B, Loc, f64, operand(0)));
      })
      .Case([&](P::FToIOp) {
        define(op, arith::FPToSIOp::create(B, Loc, B.getI64Type(), operand(0)));
      })
      .Case([&](P::UToFOp) {
        define(op, arith::UIToFPOp::create(B, Loc, f64, operand(0)));
      })
      .Case([&](P::FToUOp) {
        define(op, arith::FPToUIOp::create(B, Loc, B.getI64Type(), operand(0)));
      })
      .Case([&](P::FRound32Op) {
        Value narrow =
            arith::TruncFOp::create(B, Loc, B.getF32Type(), operand(0));
        define(op, arith::ExtFOp::create(B, Loc, f64, narrow));
      })
      .Case([&](P::WrapOp o) {
        Value low = arith::TruncIOp::create(
            B, Loc, B.getIntegerType(static_cast<unsigned>(o.getBits())),
            operand(0));
        define(op, arith::ExtSIOp::create(B, Loc, B.getI64Type(), low));
      })
      .Case([&](P::MaskOp o) {
        Value low = arith::TruncIOp::create(
            B, Loc, B.getIntegerType(static_cast<unsigned>(o.getBits())),
            operand(0));
        define(op, arith::ExtUIOp::create(B, Loc, B.getI64Type(), low));
      })
      .Case([&](P::CastOp o) {
        // docs/pir.md's table: bits, integer resizes, and the pointer casts
        // (which stay pir.cast).
        Value v = operand(0);
        Type from = v.getType(), to = o.getType();
        if (from == to)
          return define(op, v);
        if (isa<FloatType>(from) || isa<FloatType>(to))
          return define(op, arith::BitcastOp::create(B, Loc, to, v));
        if (from.isInteger() && to.isInteger()) {
          if (from.getIntOrFloatBitWidth() < to.getIntOrFloatBitWidth())
            return define(op, arith::ExtUIOp::create(B, Loc, to, v));
          return define(op, arith::TruncIOp::create(B, Loc, to, v));
        }
        define(op, P::CastOp::create(B, Loc, to, v));
      })
      // Calls.
      .Case([&](P::CallOp o) {
        define(op, resultOf(func::CallOp::create(
                       B, Loc, M.functionLinkName(o.getCallee()),
                       o->getResultTypes(), mapped(o.getArgs()))));
      })
      .Case([&](P::VCallOp o) {
        // The vtable is the object's first word (set by `new`, never
        // changed); slot k its k-th pointer.
        auto args = mapped(o.getArgs());
        Value vt = P::PtrLoadOp::create(B, Loc, ptr, asPtr(args.front()),
                                        /*vtable=*/true, /*method=*/false);
        Value slot = P::PtrAddOp::create(B, Loc, vt, i64(8 * o.getSlot()));
        Value fn = P::PtrLoadOp::create(B, Loc, ptr, slot, /*vtable=*/false,
                                        /*method=*/true);
        define(op, resultOf(P::ICallOp::create(B, Loc, o->getResultTypes(), fn,
                                               args)));
      })
      .Case([&](P::ICallOp o) {
        define(op, resultOf(P::ICallOp::create(B, Loc, o->getResultTypes(),
                                               lookup(o.getCallee()),
                                               mapped(o.getArgs()))));
      })
      .Case([&](P::FnAddrOp o) {
        define(op,
               P::FnAddrOp::create(B, Loc, M.functionLinkName(o.getCallee())));
      })
      // ARC and objects.
      .Case([&](P::RetainOp) {
        callRuntime(names::kPaykanRetain, {box}, {}, {operand(0)});
      })
      .Case([&](P::ReleaseOp) {
        callRuntime(names::kPaykanRelease, {box}, {}, {operand(0)});
      })
      .Case([&](P::BoxOp) {
        define(op, resultOf(callRuntime(names::kPaykanSharedNew, {obj}, {box},
                                        {operand(0)})));
      })
      .Case([&](P::UnboxOp) {
        // A box is its object (Runtime.h, "Shared").
        define(op, P::CastOp::create(B, Loc, obj, operand(0)));
      })
      .Case([&](P::NewOp o) {
        const ClassLayout *l = M.layout(o.getClassName());
        if (!l)
          return M.fail("no class '" + o.getClassName() + "'");
        Value size = i64(l->Size);
        Value p = resultOf(
            callRuntime(names::kPaykanMalloc, {B.getI64Type()}, {ptr}, {size}));
        // The header (vtable, zero counts) and every field, zeroed.
        P::MemZeroOp::create(B, Loc, p, size);
        P::PtrStoreOp::create(B, Loc, p,
                              P::AddressOfOp::create(B, Loc, ptr, l->VTable),
                              /*vtable=*/true);
        define(op, P::CastOp::create(B, Loc, obj, p));
      })
      .Case([&](P::FreeOp) {
        callRuntime(names::kPaykanFree, {ptr}, {}, {asPtr(operand(0))});
      })
      .Case([&](P::FieldLoadOp o) {
        Type ty;
        Value p = fieldAddr(o.getObj(), o.getClassName(), o.getField(), ty);
        define(op, P::PtrLoadOp::create(B, Loc, o.getType(), p));
      })
      .Case([&](P::FieldStoreOp o) {
        Type ty;
        Value p = fieldAddr(o.getObj(), o.getClassName(), o.getField(), ty);
        P::PtrStoreOp::create(B, Loc, p, lookup(o.getValue()));
      })
      .Case([&](P::FieldAddrOp o) {
        Type ty;
        define(op, fieldAddr(o.getObj(), o.getClassName(), o.getField(), ty));
      })
      .Case([&](P::VTableLoadOp) {
        define(op, P::PtrLoadOp::create(B, Loc, ptr, asPtr(operand(0)),
                                        /*vtable=*/true, /*method=*/false));
      })
      .Case([&](P::VTableAddrOp o) {
        const ClassLayout *l = M.layout(o.getClassName());
        if (!l)
          return M.fail("no class '" + o.getClassName() + "'");
        define(op, P::AddressOfOp::create(B, Loc, ptr, l->VTable));
      })
      // Weak references.
      .Case([&](P::WeakNewOp) {
        define(op, resultOf(callRuntime(names::kPaykanWeakNew, {box}, {weak},
                                        {operand(0)})));
      })
      .Case([&](P::WeakLoadOp) {
        define(op, resultOf(callRuntime(names::kPaykanWeakLoad, {weak}, {box},
                                        {operand(0)})));
      })
      .Case([&](P::WeakReleaseOp) {
        callRuntime(names::kPaykanWeakRelease, {weak}, {}, {operand(0)});
      })
      // Locals: their slots.
      .Case([&](P::LoadOp o) {
        define(op,
               P::PtrLoadOp::create(B, Loc, o.getType(), lookup(o.getSlot())));
      })
      .Case([&](P::StoreOp o) {
        P::PtrStoreOp::create(B, Loc, lookup(o.getSlot()),
                              lookup(o.getValue()));
      })
      .Case([&](P::LocalAddrOp o) { define(op, lookup(o.getSlot())); })
      // Memory and optionals: as they are.
      .Case<P::PtrLoadOp, P::PtrStoreOp, P::PtrAddOp, P::MemCopyOp,
            P::MemZeroOp, P::SomeOp, P::NoneOp, P::IsSomeOp, P::UnwrapOp>(
          [&](auto) { B.clone(*op, Map); })
      .Default([&](Operation *o) {
        M.fail("unexpected op '" + o->getName().getStringRef() +
               "' in function '@" + Src.getSymName() + "'");
      });
}

struct PIRToStandardPass
    : public PassWrapper<PIRToStandardPass, OperationPass<ModuleOp>> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(PIRToStandardPass)

  StringRef getArgument() const override { return "pir-to-standard"; }
  StringRef getDescription() const override {
    return "Lower the pir dialect to func, scf, arith and the pir memory ops";
  }
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<func::FuncDialect, scf::SCFDialect, arith::ArithDialect>();
  }

  void runOnOperation() override {
    ModuleOp top = getOperation();
    std::vector<ModuleOp> modules;
    if (!top.getOps<P::FuncOp>().empty() || !top.getOps<P::ClassOp>().empty())
      modules.push_back(top);
    for (auto nested : top.getOps<ModuleOp>())
      modules.push_back(nested);
    for (ModuleOp m : modules)
      if (failed(lowerToStandard(m)))
        return signalPassFailure();
  }
};

} // namespace

LogicalResult lowerToStandard(ModuleOp module) {
  return ModuleLowering(module).run();
}

std::unique_ptr<Pass> createPIRToStandardPass() {
  return std::make_unique<PIRToStandardPass>();
}

void loadStandardDialects(MLIRContext &ctx) {
  ctx.getOrLoadDialect<func::FuncDialect>();
  ctx.getOrLoadDialect<scf::SCFDialect>();
  ctx.getOrLoadDialect<arith::ArithDialect>();
  ctx.getOrLoadDialect<P::PIRDialect>();
}

} // namespace paykan::mlir_backend
