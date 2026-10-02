// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// C backend: PIR -> C11 source.  One translation unit for the whole program;
// every module-defined symbol is mangled with its module so modules never
// collide, and runtime symbols are used by their C name with the casts
// Runtime.h's prototypes need.
//
// Every piece of C syntax written here (keywords, types, operators,
// punctuation, escapes, mangling affixes) is a constant from CNames.h.

#include "CNames.h"
#include "Names.h"
#include "paykan/backends/c/CBackend.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>

namespace paykan::backend_c {

namespace {

using pir::Type;
using namespace cnames;

// -- C syntax helpers ---------------------------------------------------------

/// `(x)`
std::string paren(const std::string &x) { return kLParen + x + kRParen; }

/// The cast prefix `(ty)`.
std::string cast(const std::string &ty) { return paren(ty); }

/// `x;`
std::string stmt(const std::string &x) { return x + kSemi; }

/// `a <op> b`, with a space on each side of the operator.
std::string binop(const std::string &a, const char *op, const std::string &b) {
  return a + kSpace + op + kSpace + b;
}

/// `a, b, ...`
std::string list(const std::vector<std::string> &items) {
  std::string s;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i)
      s += kListSep;
    s += items[i];
  }
  return s;
}

/// `fn(args...)`
std::string call(const std::string &fn, const std::vector<std::string> &args) {
  return fn + paren(list(args));
}

/// `/* text */`
std::string comment(const std::string &text) {
  return kCommentOpen + text + kCommentClose;
}

/// `"text"` (@p text must not need escaping).
std::string quoted(const std::string &text) { return kQuote + text + kQuote; }

/// `base->field`
std::string member(const std::string &base, const std::string &field) {
  return base + kOpArrow + field;
}

/// `a[i]`
std::string subscript(const std::string &a, const std::string &i) {
  return a + kLBracket + i + kRBracket;
}

/// A pointer type's spelling: `T *`.
std::string pointerTo(const std::string &ty) { return ty + kSpace + kOpDeref; }

/// `&x`
std::string addressOf(const std::string &x) { return kOpAddrOf + x; }

/// A declaration `T name` (`T *name` for a pointer type `T *`).
std::string declare(const std::string &ty, const std::string &name) {
  return ty + (ty.back() == kPointerStar ? "" : kSpace) + name;
}

// -- Runtime prototypes (Runtime.h) -------------------------------------------
//
// The C types of every runtime function the lowering may call, so arguments
// and results are cast between the PIR types (obj = PaykanObject*, box =
// PaykanShared*, ...) and the declared ones.

struct RuntimeProto {
  const char *Name;
  const char *Ret;
  std::vector<const char *> Params;
};

const RuntimeProto kRuntimeProtos[] = {
    {names::kPaykanRetain, kVoid, {kRtBoxPtr}},
    {names::kPaykanRelease, kVoid, {kRtBoxPtr}},
    {names::kPaykanSharedNew, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanSharedGet, kRtObjPtr, {kRtBoxPtr}},
    {names::kPaykanMalloc, kVoidPtr, {kSize}},
    {names::kPaykanFree, kVoid, {kVoidPtr}},
    {names::kPaykanPanicDivByZero, kVoid, {}},
    {names::kPaykanPanicDivOverflow, kVoid, {}},
    {names::kPaykanStringNew, kRtStrPtr, {kConstCharPtr, kInt64}},
    {names::kPaykanStringDestroy, kVoid, {kRtObjPtr}},
    {names::kPaykanStringConcat, kRtObjPtr, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanStringCharAt, kInt8, {kRtObjPtr, kInt64}},
    {names::kPaykanStringFromInt, kRtStrPtr, {kInt64}},
    {names::kPaykanStringFromFloat, kRtStrPtr, {kDouble}},
    {names::kPaykanStringFromBool, kRtStrPtr, {kInt64}},
    {names::kPaykanStringFromChar, kRtStrPtr, {kInt8}},
    {names::kPaykanStringEquals, kInt64, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanStringToString, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanStringLength, kInt64, {kRtObjPtr}},
    {names::kPaykanArrayNew, kRtArrPtr, {kUnsignedLong}},
    {names::kPaykanArrayNewObj, kRtArrPtr, {kUnsignedLong}},
    {names::kPaykanArrayNewFromData, kRtArrPtr, {kUnsignedLong, kConstVoidPtr}},
    {names::kPaykanArrayGet, kVoidPtr, {kRtArrPtr, kUnsignedLong}},
    {names::kPaykanArraySet, kVoid, {kRtArrPtr, kUnsignedLong, kVoidPtr}},
    {names::kPaykanArraySetObj, kVoid, {kRtArrPtr, kUnsignedLong, kRtBoxPtr}},
    {names::kPaykanArrayPush, kVoid, {kRtArrPtr, kVoidPtr}},
    {names::kPaykanArrayPushObj, kVoid, {kRtArrPtr, kRtBoxPtr}},
    {names::kPaykanArrayPop, kVoidPtr, {kRtArrPtr}},
    {names::kPaykanArrayPopObj, kRtBoxPtr, {kRtArrPtr}},
    {names::kPaykanTupleNew, kRtTupPtr, {kInt64, kConstUint8Ptr}},
    {names::kPaykanTupleGet, kInt64, {kRtTupPtr, kInt64}},
    {names::kPaykanTupleSet, kVoid, {kRtTupPtr, kInt64, kInt64}},
    {names::kPaykanTupleSetObj, kVoid, {kRtTupPtr, kInt64, kRtBoxPtr}},
    {names::kPaykanFileOpen, kRtBoxPtr, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanIntFromStr, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanFloatFromStr, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanPrint, kVoid, {kRtObjPtr}},
    {names::kPaykanPrintln, kVoid, {kRtObjPtr}},
    {names::kPaykanErrPrint, kVoid, {kRtObjPtr}},
    {names::kPaykanErrPrintln, kVoid, {kRtObjPtr}},
    {names::kPaykanObjectDestroy, kVoid, {kRtObjPtr}},
    {names::kPaykanObjectToString, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanObjectEquals, kInt64, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanFileDestroy, kVoid, {kRtObjPtr}},
    {names::kPaykanFileToString, kRtBoxPtr, {kRtObjPtr}},
    {names::kPaykanFileEquals, kInt64, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanFileWrite, kVoid, {kRtObjPtr, kRtObjPtr}},
    {names::kPaykanFileReadln, kRtBoxPtr, {kRtObjPtr}},
};

const RuntimeProto *findProto(const std::string &name) {
  for (const auto &p : kRuntimeProtos)
    if (name == p.Name)
      return &p;
  return nullptr;
}

const char *cType(Type t) {
  switch (t) {
  case Type::Void:
    return kVoid;
  case Type::I64:
    return kInt64;
  case Type::F64:
    return kDouble;
  case Type::Bool:
    return kBool;
  case Type::Char:
    return kInt8;
  case Type::Box:
    return kRtBoxPtr;
  case Type::Obj:
    return kRtObjPtr;
  case Type::Ptr:
    return kVoidPtr;
  }
  return kVoid;
}

/// The C operator of a comparison (integer, pointer and ordered float
/// compares; a float `ne` is emitted separately).
const char *cmpOperator(pir::CmpPred pred) {
  switch (pred) {
  case pir::CmpPred::Eq:
    return kOpEq;
  case pir::CmpPred::Ne:
    return kOpNe;
  case pir::CmpPred::Lt:
    return kOpLt;
  case pir::CmpPred::Le:
    return kOpLe;
  case pir::CmpPred::Gt:
    return kOpGt;
  case pir::CmpPred::Ge:
    return kOpGe;
  }
  return kOpEq;
}

bool isPointerCType(const std::string &c) {
  return c.find(kPointerStar) != std::string::npos;
}
bool isPointerType(Type t) {
  return t == Type::Box || t == Type::Obj || t == Type::Ptr;
}

/// Keep [A-Za-z0-9_]; escape anything else as _XX (hex).
std::string sanitize(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == kIdentUnderscore) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, kIdentHexEscapeFormat, c);
      out += buf;
    }
  }
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
    out = kIdentLeadPrefix + out;
  return out;
}

std::string escapeCString(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
    case '\\':
      out += kEscBackslash;
      break;
    case '"':
      out += kEscQuote;
      break;
    case '?':
      out += kEscQuestion;
      break;
    case '\n':
      out += kEscNewline;
      break;
    case '\t':
      out += kEscTab;
      break;
    case '\r':
      out += kEscReturn;
      break;
    default:
      if (c >= kPrintableFirst && c < kPrintableEnd) {
        out += static_cast<char>(c);
      } else {
        char buf[6];
        std::snprintf(buf, sizeof buf, kEscOctalFormat, c);
        out += buf;
      }
    }
  }
  return out;
}

std::string fmtI64(int64_t v) {
  if (v == INT64_MIN)
    return kInt64Min;
  return call(kInt64C, {std::to_string(v)});
}

std::string fmtF64(double v) {
  if (std::isnan(v))
    return kNan;
  if (std::isinf(v))
    return v < 0 ? paren(kOpNeg + std::string(kInfinity)) : kInfinity;
  char buf[64];
  std::snprintf(buf, sizeof buf, kF64Format, v);
  std::string s = buf;
  if (s.find_first_of(kF64Marks) == std::string::npos)
    s += kF64Suffix;
  return s;
}

// -- The emitter --------------------------------------------------------------

class Emitter {
  const pir::Program &P;
  std::ostream &O;
  std::ostream &E;

  std::unordered_map<std::string, size_t> ModuleIndex; // path -> index
  /// Mangled C symbol of every module-defined item: key "<mod>\n<name>".
  std::unordered_map<std::string, std::string> Symbols;
  std::set<std::string> UsedSymbols;
  /// Class name -> (defining module index, the class item).
  struct ClassDef {
    size_t Module;
    const pir::Class *Cls;
  };
  std::unordered_map<std::string, ClassDef> ClassDefs;

  // Per-function state.
  std::unordered_map<pir::ValueId, std::string> ValueNames;
  std::unordered_map<pir::ValueId, Type> ValueTypes;
  std::vector<std::string> LocalNames;
  const pir::Module *CurMod = nullptr;
  size_t CurModIdx = 0;
  int Indent = 0;
  bool Failed = false;

  std::string ind() const {
    std::string s;
    for (int i = 0; i < Indent; ++i)
      s += kIndentUnit;
    return s;
  }

  void fail(const std::string &msg) {
    if (!Failed)
      E << "C backend: " << msg << "\n";
    Failed = true;
  }

  std::string moduleStem(const pir::Module &m) {
    std::string stem = m.Name;
    size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos)
      stem = stem.substr(slash + 1);
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0)
      stem = stem.substr(0, dot);
    return sanitize(stem);
  }

  static std::string symKey(const std::string &mod, const std::string &name) {
    return mod + "\n" + name;
  }

  /// Reserve a unique C symbol for (module, name).  Every base starts with
  /// kSymbolPrefix (`pk_`), which keeps these apart from C keywords, the C
  /// library and the runtime (`Paykan*`); the names derived from a class
  /// symbol use their own prefixes (`pkvt_`, `pknew_`) and the emitter's
  /// helpers `pkrt_` (CNames.h).
  void defineSymbol(const pir::Module &m, const std::string &name,
                    const std::string &base) {
    std::string key = symKey(m.Name, name);
    if (Symbols.count(key))
      return;
    std::string sym = base;
    unsigned n = 1;
    while (UsedSymbols.count(sym))
      sym = base + kMangleSep + std::to_string(n++);
    UsedSymbols.insert(sym);
    Symbols[key] = sym;
  }

  const std::string &symbolOf(const std::string &mod, const std::string &name) {
    static const std::string missing = kMissing;
    auto it = Symbols.find(symKey(mod, name));
    if (it == Symbols.end()) {
      fail("no definition of '" + name + "' in module '" + mod + "'");
      return missing;
    }
    return it->second;
  }

  /// C symbol for a function referenced from module @p m by PIR name.
  std::string funcSymbol(const pir::Module &m, const std::string &name) {
    const pir::Function *fn = m.findFunction(name);
    if (!fn) {
      fail("function '" + name + "' is not declared in module '" + m.Name +
           "'");
      return kUndeclared;
    }
    if (fn->IsExtern) {
      if (fn->Module.empty())
        return fn->Name; // runtime symbol
      return symbolOf(fn->Module, fn->Name);
    }
    return symbolOf(m.Name, fn->Name);
  }

  const ClassDef *classDef(const std::string &name) {
    auto it = ClassDefs.find(name);
    if (it == ClassDefs.end()) {
      fail("class '" + name + "' is not defined by any module");
      return nullptr;
    }
    return &it->second;
  }

  /// The C symbol of a class: its struct tag (`struct <sym>`), and the base
  /// of its vtable / allocator names.  The constructor function uses the
  /// same symbol in the ordinary identifier namespace, which is why the
  /// struct is always spelled with its `struct` keyword and never typedef'd.
  std::string classSymbol(const std::string &name) {
    const ClassDef *d = classDef(name);
    return d ? symbolOf(P.Modules[d->Module].Name, name) : kNoClass;
  }
  std::string classStruct(const std::string &name) {
    return kStruct + std::string(kSpace) + classSymbol(name);
  }
  std::string classVTable(const std::string &name) {
    return kVTablePrefix + classSymbol(name);
  }
  std::string classNew(const std::string &name) {
    return kNewPrefix + classSymbol(name);
  }

  /// A PIR value (parameter or instruction result): `v<id>`, plus the
  /// value's name for readability.  The id is unique in the function and
  /// ends at the first '_', so these never collide with each other or with
  /// the `l_` / `l<k>_` locals (see emitFunction).
  static std::string valueName(const pir::Value &v) {
    std::string id = kValuePrefix + std::to_string(v.Id);
    if (v.Name.empty())
      return id;
    std::string nm = v.Name;
    for (char &c : nm)
      if (c == kValueNameDot)
        c = kValueDotReplacement;
    return id + kMangleSep + sanitize(nm);
  }

  /// A class field: `f_<name>`, so a field named after a C macro (`errno`,
  /// `stdout`, ...) is never expanded.
  static std::string fieldName(const std::string &name) {
    return kFieldPrefix + sanitize(name);
  }

  std::string operand(const pir::Operand &op, Type &ty) {
    if (auto *id = std::get_if<pir::ValueId>(&op.V)) {
      auto it = ValueNames.find(*id);
      if (it == ValueNames.end()) {
        fail("use of undefined value %" + std::to_string(*id));
        ty = Type::Void;
        return kUndef;
      }
      ty = ValueTypes[*id];
      return it->second;
    }
    if (auto *i = std::get_if<int64_t>(&op.V)) {
      ty = Type::I64;
      return fmtI64(*i);
    }
    if (auto *d = std::get_if<double>(&op.V)) {
      ty = Type::F64;
      return fmtF64(*d);
    }
    if (auto *b = std::get_if<bool>(&op.V)) {
      ty = Type::Bool;
      return *b ? kTrue : kFalse;
    }
    if (auto *c = std::get_if<char>(&op.V)) {
      ty = Type::Char;
      return cast(kInt8) + std::to_string(static_cast<int>(*c));
    }
    if (auto *n = std::get_if<pir::Operand::Null>(&op.V)) {
      ty = n->Ty;
      return kNull;
    }
    if (auto *s = std::get_if<pir::SymbolRef>(&op.V))
      return symbolOperand(s->Name, ty);
    ty = Type::Void;
    return kUnknownOperand;
  }

  std::string operand(const pir::Operand &op) {
    Type t;
    return operand(op, t);
  }

  std::string symbolOperand(const std::string &name, Type &ty) {
    for (const auto &g : CurMod->CStrs)
      if (g.Name == name) {
        ty = Type::Ptr;
        return cast(kVoidPtr) + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Datas)
      if (g.Name == name) {
        ty = Type::Ptr;
        return cast(kVoidPtr) + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Bytes)
      if (g.Name == name) {
        ty = Type::Ptr;
        return cast(kVoidPtr) + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Externs)
      if (g.Name == name) {
        if (g.K == pir::ExternGlobal::Object) {
          ty = Type::Obj;
          return cast(kRtObjPtr) + addressOf(name);
        }
        ty = Type::Ptr;
        return cast(kVoidPtr) + addressOf(name);
      }
    fail("unknown symbol '@" + name + "'");
    ty = Type::Void;
    return kUnknownSymbol;
  }

  /// Cast a PIR-typed C expression to a runtime parameter's C type (through
  /// intptr_t between integers and pointers).
  static std::string castTo(const std::string &expr, Type from,
                            const std::string &toC) {
    bool toPtr = isPointerCType(toC);
    if ((toPtr && from == Type::I64) || (!toPtr && isPointerType(from)))
      return cast(toC) + cast(kIntptr) + expr;
    return cast(toC) + expr;
  }

  static std::string castResult(const std::string &call,
                                const std::string &fromC, Type to) {
    bool fromPtr = isPointerCType(fromC);
    if (fromPtr && to == Type::I64)
      return cast(kInt64) + cast(kIntptr) + call;
    return cast(cType(to)) + call;
  }

  void line(const std::string &s) { O << ind() << s << kNewline; }

  // -- Program-level passes ---------------------------------------------------

  void collectSymbols() {
    for (size_t i = 0; i < P.Modules.size(); ++i) {
      const pir::Module &m = P.Modules[i];
      ModuleIndex[m.Name] = i;
      std::string base = kSymbolPrefix + moduleStem(m);
      for (const auto &c : m.Classes)
        if (!c.IsExtern) {
          if (ClassDefs.count(c.Name)) {
            fail("class '" + c.Name + "' is defined by two modules");
            continue;
          }
          ClassDefs[c.Name] = {i, &c};
          defineSymbol(m, c.Name, base + kMangleSep + sanitize(c.Name));
        }
      for (const auto &f : m.Functions)
        if (!f.IsExtern)
          defineSymbol(m, f.Name, base + kMangleSep + sanitize(f.Name));
      for (const auto &g : m.CStrs)
        defineSymbol(m, g.Name,
                     base + kCStrSuffix + std::to_string(&g - &m.CStrs[0]));
      for (const auto &g : m.Datas)
        defineSymbol(m, g.Name,
                     base + kDataSuffix + std::to_string(&g - &m.Datas[0]));
      for (const auto &g : m.Bytes)
        defineSymbol(m, g.Name,
                     base + kBytesSuffix + std::to_string(&g - &m.Bytes[0]));
    }
  }

  std::string signatureC(const pir::Signature &sig, bool withNames,
                         const std::vector<pir::Value> *params) {
    std::vector<std::string> ps;
    if (sig.Params.empty())
      ps.emplace_back(kVoid);
    for (size_t i = 0; i < sig.Params.size(); ++i) {
      if (withNames && params && i < params->size())
        ps.push_back(declare(cType(sig.Params[i]), valueName((*params)[i])));
      else
        ps.emplace_back(cType(sig.Params[i]));
    }
    return paren(list(ps));
  }

  /// `static inline <ret> <name>(<paramTy> <param>) { <ret> <local>;
  /// memcpy(&<local>, &<param>, sizeof <local>); return <local>; }`: a
  /// bit-cast helper of the prelude.
  void emitBitCastHelper(const char *ret, const char *name, const char *paramTy,
                         const char *param, const char *local) {
    O << kStatic << kSpace << kInline << kSpace << ret << kSpace << name
      << paren(declare(paramTy, param)) << kSpace << kLBrace << kNewline
      << kIndentUnit << stmt(declare(ret, local)) << kSpace
      << stmt(call(kMemcpy, {addressOf(local), addressOf(param),
                             kSizeof + std::string(kSpace) + local}))
      << kSpace << stmt(kReturn + std::string(kSpace) + local) << kNewline
      << kRBrace << kNewline;
  }

  void emitPrelude() {
    O << comment(kBanner) << kNewline;
    for (const char *h : {kMathH, kStdboolH, kStdintH, kStdlibH, kStringH})
      O << kInclude << kSysHeaderOpen << h << kSysHeaderClose << kNewline;
    O << kInclude << quoted(kRuntimeH) << kNewline << kNewline;
    // typedef void (*pkrt_fn)(void);
    O << stmt(kTypedef + std::string(kSpace) + kVoid + kSpace +
              paren(kOpDeref + std::string(kHelperFnType)) + paren(kVoid))
      << kNewline << kNewline;
    emitBitCastHelper(kInt64, kHelperF64Bits, kDouble, kHelperDouble,
                      kHelperInt);
    emitBitCastHelper(kDouble, kHelperBitsF64, kInt64, kHelperInt,
                      kHelperDouble);
    O << kNewline;
  }

  // -- Translation units ------------------------------------------------------
  //
  // A translation unit holds a set of modules: every module for `--emit-c`
  // (one readable file), or one module for `build` / `run` (cached per
  // module).  Each PIR module is self-describing -- it carries the classes
  // and functions it references from other modules as extern items -- so a
  // unit declares exactly what its modules define or import.

  /// The modules of the unit being emitted.
  std::vector<size_t> Unit;

  bool inUnit(size_t mi) const {
    for (size_t u : Unit)
      if (u == mi)
        return true;
    return false;
  }

  void emitClassLayouts() {
    // Every class a unit module defines or references, once; an extern item
    // carries the same (flattened) field list as the definition.
    std::vector<const pir::Class *> classes;
    std::set<std::string> seen;
    for (size_t mi : Unit)
      for (const auto &c : P.Modules[mi].Classes)
        if (seen.insert(c.Name).second)
          classes.push_back(&c);
    for (const pir::Class *c : classes)
      O << stmt(classStruct(c->Name)) << kNewline;
    if (!classes.empty())
      O << kNewline;
    for (const pir::Class *c : classes) {
      std::string note = c->Name;
      if (!c->Super.empty())
        note += kSpace + std::string(kSuperNote) + kSpace + c->Super;
      O << classStruct(c->Name) << kSpace << kLBrace << kSpace << comment(note)
        << kNewline;
      O << kIndentUnit << stmt(declare(kRtVTablePtr, kFieldVTable)) << kNewline
        << kIndentUnit << stmt(declare(kRtBoxPtr, kFieldShared)) << kNewline;
      for (const auto &f : c->Fields)
        O << kIndentUnit << stmt(declare(cType(f.Ty), fieldName(f.Name)))
          << kNewline;
      O << stmt(kRBrace) << kNewline << kNewline;
    }
  }

  void emitPrototypes() {
    // Functions the unit defines, and those it imports from other modules.
    std::set<std::string> seen;
    for (size_t mi : Unit) {
      const pir::Module &m = P.Modules[mi];
      for (const auto &f : m.Functions) {
        if (f.IsExtern && f.Module.empty())
          continue; // runtime: Runtime.h declares it
        std::string sym = funcSymbol(m, f.Name);
        if (!seen.insert(sym).second)
          continue;
        O << stmt(declare(cType(f.Sig.Ret), sym) +
                  signatureC(f.Sig, false, nullptr))
          << kNewline;
      }
    }
    O << kNewline;
  }

  void emitVTables() {
    std::set<std::string> seen;
    for (size_t mi : Unit) {
      const pir::Module &m = P.Modules[mi];
      for (const auto &c : m.Classes) {
        if (!seen.insert(c.Name).second)
          continue;
        const ClassDef *d = classDef(c.Name);
        if (!d)
          continue;
        if (!inUnit(d->Module)) {
          // extern pkrt_fn pkvt_X[];
          O << stmt(kExtern + std::string(kSpace) + kHelperFnType + kSpace +
                    subscript(classVTable(c.Name), ""))
            << kNewline;
          continue;
        }
        const pir::Class &def = *d->Cls;
        const pir::Module &defMod = P.Modules[d->Module];
        // pkrt_fn pkvt_X[n] = {
        O << binop(kHelperFnType + std::string(kSpace) +
                       subscript(classVTable(c.Name),
                                 std::to_string(
                                     std::max<size_t>(def.VTable.size(), 1))),
                   kOpAssign, kLBrace)
          << kNewline;
        for (const auto &e : def.VTable) {
          O << kIndentUnit;
          if (e.Target.empty())
            O << kNull;
          else
            O << cast(kHelperFnType) << funcSymbol(defMod, e.Target);
          O << kListSep << comment(e.Slot) << kNewline;
        }
        O << stmt(kRBrace) << kNewline;
      }
    }
    O << kNewline;
  }

  void emitNewHelpers() {
    for (size_t mi : Unit)
      for (const auto &c : P.Modules[mi].Classes) {
        if (c.IsExtern)
          continue;
        std::string s = classStruct(c.Name);
        std::string sPtr = pointerTo(s);
        // static PaykanObject *pknew_X(void) {
        O << kStatic << kSpace << declare(kRtObjPtr, classNew(c.Name))
          << paren(kVoid) << kSpace << kLBrace << kNewline;
        //   struct X *o = (struct X *)Paykan_malloc(sizeof(struct X));
        O << kIndentUnit
          << stmt(binop(declare(sPtr, kNewObj), kOpAssign,
                        cast(sPtr) +
                            call(names::kPaykanMalloc, {kSizeof + paren(s)})))
          << kNewline;
        O << kIndentUnit
          << stmt(binop(member(kNewObj, kFieldVTable), kOpAssign,
                        cast(kRtVTablePtr) + classVTable(c.Name)))
          << kNewline;
        O << kIndentUnit
          << stmt(binop(member(kNewObj, kFieldShared), kOpAssign, kNull))
          << kNewline;
        for (const auto &f : c.Fields) {
          const char *zero = kZero;
          switch (f.Ty) {
          case Type::F64:
            zero = kZeroF64;
            break;
          case Type::Bool:
            zero = kFalse;
            break;
          case Type::Box:
          case Type::Obj:
          case Type::Ptr:
            zero = kNull;
            break;
          default:
            break;
          }
          O << kIndentUnit
            << stmt(binop(member(kNewObj, fieldName(f.Name)), kOpAssign, zero))
            << kNewline;
        }
        O << kIndentUnit
          << stmt(kReturn + std::string(kSpace) + cast(kRtObjPtr) + kNewObj)
          << kNewline << kRBrace << kNewline << kNewline;
      }
  }

  void emitGlobals(const pir::Module &m) {
    // static const <ty> <sym>[<n>] = <init>;
    auto global = [&](const char *ty, const std::string &sym, size_t n,
                      const std::string &init) {
      O << stmt(binop(kStatic + std::string(kSpace) + kConst + kSpace + ty +
                          kSpace + subscript(sym, std::to_string(n)),
                      kOpAssign, init))
        << kNewline;
    };
    for (const auto &g : m.CStrs)
      global(kChar, symbolOf(m.Name, g.Name), g.Data.size() + 1,
             quoted(escapeCString(g.Data)));
    for (const auto &g : m.Datas) {
      std::vector<std::string> words;
      words.reserve(g.Words.size());
      for (int64_t w : g.Words)
        words.push_back(fmtI64(w));
      global(kInt64, symbolOf(m.Name, g.Name),
             std::max<size_t>(g.Words.size(), 1),
             kLBrace + list(words) + kRBrace);
    }
    for (const auto &g : m.Bytes) {
      std::vector<std::string> bytes;
      bytes.reserve(g.Bytes.size());
      for (uint8_t b : g.Bytes)
        bytes.push_back(std::to_string(static_cast<int>(b)));
      global(kUint8, symbolOf(m.Name, g.Name),
             std::max<size_t>(g.Bytes.size(), 1),
             kLBrace + list(bytes) + kRBrace);
    }
    if (!m.CStrs.empty() || !m.Datas.empty() || !m.Bytes.empty())
      O << kNewline;
  }

  // -- Functions --------------------------------------------------------------

  void defineValue(const pir::Value &v) {
    ValueNames[v.Id] = valueName(v);
    ValueTypes[v.Id] = v.Ty;
  }

  void emitFunction(const pir::Function &f) {
    ValueNames.clear();
    ValueTypes.clear();
    LocalNames.clear();
    for (const auto &p : f.Params)
      defineValue(p);

    O << declare(cType(f.Sig.Ret), symbolOf(CurMod->Name, f.Name))
      << signatureC(f.Sig, true, &f.Params) << kSpace << kLBrace << kNewline;
    Indent = 1;

    // Locals: `l_<name>`, or `l<k>_<name>` for the k-th further local of
    // the same name (shadowing in nested scopes).  The prefix keeps user
    // names apart from C keywords, the C library's macros and functions
    // (`errno`, `fmod`, `int64_t`, ...), the `v<id>` values and the `pk*`
    // symbols; the digits of `l<k>` end at the first '_', so the scheme
    // never produces one name twice.
    std::set<std::string> used;
    for (size_t i = 0; i < f.Locals.size(); ++i) {
      std::string base = sanitize(f.Locals[i].Name);
      std::string nm = kLocalPrefix + base;
      for (unsigned k = 1; used.count(nm); ++k) {
        nm = kLocalShadowPrefix;
        nm += std::to_string(k);
        nm += kLocalShadowSep;
        nm += base;
      }
      used.insert(nm);
      LocalNames.push_back(nm);
      line(stmt(declare(cType(f.Locals[i].Ty), nm)));
    }
    emitBlock(f.Body);
    Indent = 0;
    O << kRBrace << kNewline << kNewline;
  }

  void emitBlock(const pir::Block &b) {
    for (const auto &st : b.Stmts)
      emitStmt(st);
  }

  /// `<head> {`
  static std::string openBlock(const std::string &head) {
    return head + kSpace + kLBrace;
  }

  /// `if (<cond>)`
  static std::string ifHead(const std::string &cond) {
    return kIf + std::string(kSpace) + paren(cond);
  }

  void emitStmt(const pir::Stmt &st) {
    if (auto *i = std::get_if<pir::Instr>(&st)) {
      emitInstr(*i);
    } else if (auto *s = std::get_if<pir::If>(&st)) {
      if (s->Then->Stmts.empty() && s->Else && !s->Else->Stmts.empty()) {
        line(openBlock(ifHead(kOpNot + operand(s->Cond))));
        ++Indent;
        emitBlock(*s->Else);
        --Indent;
        line(kRBrace);
        return;
      }
      line(openBlock(ifHead(operand(s->Cond))));
      ++Indent;
      emitBlock(*s->Then);
      --Indent;
      if (s->Else && !s->Else->Stmts.empty()) {
        line(openBlock(kRBrace + std::string(kSpace) + kElse));
        ++Indent;
        emitBlock(*s->Else);
        --Indent;
      }
      line(kRBrace);
    } else if (auto *w = std::get_if<pir::While>(&st)) {
      // for (;;) { <cond block> if (!(<cond>)) break; <body> }
      line(openBlock(kFor + std::string(kSpace) +
                     paren(std::string(kSemi) + kSemi)));
      ++Indent;
      emitBlock(*w->CondBlock);
      line(ifHead(kOpNot + paren(operand(w->Cond))) + kSpace + stmt(kBreak));
      emitBlock(*w->Body);
      --Indent;
      line(kRBrace);
    } else if (std::holds_alternative<pir::Break>(st)) {
      line(stmt(kBreak));
    } else if (std::holds_alternative<pir::Continue>(st)) {
      line(stmt(kContinue));
    } else if (auto *r = std::get_if<pir::Return>(&st)) {
      if (r->Value)
        line(stmt(kReturn + std::string(kSpace) + operand(*r->Value)));
      else
        line(stmt(kReturn));
    } else if (std::holds_alternative<pir::Unreachable>(st)) {
      line(stmt(call(kAbort, {})) + kSpace + comment(kUnreachableNote));
    }
  }

  std::string resultPrefix(const pir::Instr &i) {
    if (i.Result.Ty == Type::Void)
      return "";
    defineValue(i.Result);
    return declare(cType(i.Result.Ty), ValueNames[i.Result.Id]) + kSpace +
           kOpAssign + kSpace;
  }

  std::string arith(const char *op, const pir::Instr &i) {
    Type ta, tb;
    std::string a = operand(i.Args[0], ta);
    std::string b = operand(i.Args[1], tb);
    if (ta == Type::F64)
      return binop(a, op, b);
    // Wrapping two's-complement arithmetic (signed overflow is UB in C).
    return cast(kInt64) +
           paren(binop(cast(kUint64) + a, op, cast(kUint64) + b));
  }

  void emitInstr(const pir::Instr &i) {
    using pir::Opcode;
    std::string pre;
    switch (i.Op) {
    case Opcode::Add:
      pre = resultPrefix(i);
      line(stmt(pre + arith(kOpAdd, i)));
      return;
    case Opcode::Sub:
      pre = resultPrefix(i);
      line(stmt(pre + arith(kOpSub, i)));
      return;
    case Opcode::Mul:
      pre = resultPrefix(i);
      line(stmt(pre + arith(kOpMul, i)));
      return;
    case Opcode::Div: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      line(stmt(pre + binop(a, kOpDiv, b)));
      return;
    }
    case Opcode::Rem: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      if (ta == Type::F64)
        line(stmt(pre + call(kFmod, {a, b})));
      else
        line(stmt(pre + binop(a, kOpRem, b)));
      return;
    }
    case Opcode::Neg: {
      pre = resultPrefix(i);
      Type ta;
      std::string a = operand(i.Args[0], ta);
      if (ta == Type::F64)
        line(stmt(pre + kOpNeg + a));
      else
        line(stmt(pre + cast(kInt64) +
                  paren(binop(kZero, kOpSub, cast(kUint64) + a))));
      return;
    }
    case Opcode::Not:
      pre = resultPrefix(i);
      line(stmt(pre + kOpNot + operand(i.Args[0])));
      return;
    case Opcode::Cmp: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      if (ta == Type::F64 && i.Pred == pir::CmpPred::Ne) {
        // Ordered "not equal": false for NaN operands (unlike C's !=).
        line(stmt(pre +
                  paren(binop(binop(a, kOpLt, b), kOpOr, binop(a, kOpGt, b)))));
        return;
      }
      line(stmt(pre + paren(binop(a, cmpOperator(i.Pred), b))));
      return;
    }
    case Opcode::Select:
      pre = resultPrefix(i);
      line(stmt(pre + paren(binop(binop(operand(i.Args[0]), kOpCondQ,
                                        operand(i.Args[1])),
                                  kOpCondColon, operand(i.Args[2])))));
      return;
    case Opcode::IToF:
      pre = resultPrefix(i);
      line(stmt(pre + cast(kDouble) + operand(i.Args[0])));
      return;
    case Opcode::Cast: {
      pre = resultPrefix(i);
      Type from;
      std::string a = operand(i.Args[0], from);
      Type to = i.CastTo;
      std::string expr;
      if (from == Type::F64 && to == Type::I64)
        expr = call(kHelperF64Bits, {a});
      else if (from == Type::I64 && to == Type::F64)
        expr = call(kHelperBitsF64, {a});
      else if (to == Type::Bool)
        expr = paren(binop(paren(binop(a, kOpBitAnd, kOne)), kOpNe, kZero));
      else if (to == Type::I64 && from == Type::Char)
        expr = cast(kInt64) + cast(kUint8) + a;
      else if (to == Type::I64 && from == Type::Bool)
        expr = cast(kInt64) + a;
      else if (to == Type::Char)
        expr = cast(kInt8) + a;
      else if (to == Type::I64 && isPointerType(from))
        expr = cast(kInt64) + cast(kIntptr) + a;
      else if (isPointerType(to) && from == Type::I64)
        expr = cast(cType(to)) + cast(kIntptr) + a;
      else
        expr = cast(cType(to)) + a;
      line(stmt(pre + expr));
      return;
    }
    case Opcode::Call: {
      const pir::Function *callee = CurMod->findFunction(i.Callee);
      if (!callee) {
        fail("call to undeclared function '" + i.Callee + "'");
        return;
      }
      pre = resultPrefix(i);
      std::vector<std::string> args;
      args.reserve(i.Args.size());
      std::string fn;
      if (callee->IsExtern && callee->Module.empty()) {
        const RuntimeProto *proto = findProto(callee->Name);
        if (!proto) {
          fail("unknown runtime function '" + callee->Name + "'");
          return;
        }
        fn = callee->Name;
        for (size_t a = 0; a < i.Args.size(); ++a) {
          Type ta;
          std::string arg = operand(i.Args[a], ta);
          args.push_back(a < proto->Params.size()
                             ? castTo(arg, ta, proto->Params[a])
                             : arg);
        }
        std::string c = call(fn, args);
        if (callee->Sig.Ret != Type::Void)
          c = castResult(c, proto->Ret, callee->Sig.Ret);
        line(stmt(pre + c));
        return;
      }
      fn = funcSymbol(*CurMod, i.Callee);
      for (const auto &arg : i.Args)
        args.push_back(operand(arg));
      line(stmt(pre + call(fn, args)));
      return;
    }
    case Opcode::VCall: {
      pre = resultPrefix(i);
      Type tr;
      std::string recv = operand(i.Args[0], tr);
      // <ret> (*)(<params>)
      std::vector<std::string> params;
      params.reserve(i.Sig.Params.size() + 1);
      for (Type p : i.Sig.Params)
        params.emplace_back(cType(p));
      if (params.empty())
        params.emplace_back(kVoid);
      std::string fnTy = cType(i.Sig.Ret) + std::string(kSpace) +
                         paren(kOpDeref) + paren(list(params));
      // ((<fnTy>)((pkrt_fn *)(<recv>)->vtable)[<slot>])(<args>)
      std::string slotFn =
          paren(cast(fnTy) + subscript(paren(cast(pointerTo(kHelperFnType)) +
                                             member(paren(recv), kFieldVTable)),
                                       std::to_string(i.Slot)));
      std::vector<std::string> args;
      args.reserve(i.Args.size());
      for (const auto &arg : i.Args)
        args.push_back(operand(arg));
      std::string note =
          i.ClassName.empty()
              ? ""
              : kSpace + comment(i.ClassName + kSpace + kSlotNote + kSpace +
                                 std::to_string(i.Slot));
      line(stmt(pre + call(slotFn, args)) + note);
      return;
    }
    case Opcode::Retain:
      line(stmt(call(names::kPaykanRetain, {operand(i.Args[0])})));
      return;
    case Opcode::Release:
      line(stmt(call(names::kPaykanRelease, {operand(i.Args[0])})));
      return;
    case Opcode::Box:
      pre = resultPrefix(i);
      line(stmt(pre + call(names::kPaykanSharedNew, {operand(i.Args[0])})));
      return;
    case Opcode::Unbox:
      pre = resultPrefix(i);
      line(stmt(pre + call(names::kPaykanSharedGet, {operand(i.Args[0])})));
      return;
    case Opcode::New:
      pre = resultPrefix(i);
      line(stmt(pre + call(classNew(i.ClassName), {})));
      return;
    case Opcode::Free:
      line(stmt(call(names::kPaykanFree, {operand(i.Args[0])})));
      return;
    case Opcode::FieldLoad:
      pre = resultPrefix(i);
      line(stmt(pre + member(paren(cast(pointerTo(classStruct(i.ClassName))) +
                                   operand(i.Args[0])),
                             fieldName(i.Field))));
      return;
    case Opcode::FieldStore:
      line(stmt(binop(member(paren(cast(pointerTo(classStruct(i.ClassName))) +
                                   operand(i.Args[0])),
                             fieldName(i.Field)),
                      kOpAssign, operand(i.Args[1]))));
      return;
    case Opcode::VTableLoad:
      pre = resultPrefix(i);
      line(stmt(pre + cast(kVoidPtr) +
                member(paren(operand(i.Args[0])), kFieldVTable)));
      return;
    case Opcode::VTableAddr: {
      pre = resultPrefix(i);
      if (!i.ClassName.empty()) {
        line(stmt(pre + cast(kVoidPtr) + classVTable(i.ClassName)));
        return;
      }
      // An extern (runtime) vtable global, named by the symbol operand.
      const auto *sym =
          i.Args.empty() ? nullptr : std::get_if<pir::SymbolRef>(&i.Args[0].V);
      if (!sym) {
        fail("vtable.addr without a class or a symbol");
        return;
      }
      line(stmt(pre + cast(kVoidPtr) + addressOf(sym->Name)));
      return;
    }
    case Opcode::Load:
      pre = resultPrefix(i);
      if (i.Local >= LocalNames.size()) {
        fail("load of an undeclared local");
        return;
      }
      line(stmt(pre + LocalNames[i.Local]));
      return;
    case Opcode::Store:
      if (i.Local >= LocalNames.size()) {
        fail("store to an undeclared local");
        return;
      }
      line(stmt(binop(LocalNames[i.Local], kOpAssign, operand(i.Args[0]))));
      return;
    }
  }

  /// `getenv("<var>") != NULL`
  static std::string envSet(const char *var) {
    return binop(call(kGetenv, {quoted(var)}), kOpNe, kNull);
  }

  void emitMain() {
    const pir::Module &main = P.Modules[0];
    const pir::Function *mainFn = main.findFunction(kMain);
    if (!mainFn || mainFn->IsExtern) {
      fail("the main module defines no 'main' function");
      return;
    }
    bool takesArgs = mainFn->Sig.Params.size() == 1;
    // int main(int argc, char **argv) {
    line(openBlock(
        declare(kInt, kMain) +
        paren(list({declare(kInt, kArgc), declare(kCharPtrPtr, kArgv)}))));
    Indent = 1;
    // int track = getenv("PAYKAN_TRACK_HEAP") != NULL;
    // if (track) { Paykan_heap_set_tracking(1); Paykan_heap_reset(); }
    line(stmt(
        binop(declare(kInt, kMainTrack), kOpAssign, envSet(kEnvTrackHeap))));
    line(openBlock(ifHead(kMainTrack)) + kSpace +
         stmt(call(kRtHeapSetTracking, {kOne})) + kSpace +
         stmt(call(names::kPaykanHeapReset, {})) + kSpace + kRBrace);
    if (takesArgs) {
      // PAYKAN_NO_ARGS: run with an empty argument list (the kernel always
      // supplies an argv[0], so a caller that wants argc == 0 says so).
      line(stmt(binop(declare(pointerTo(kRtArray), kMainArgs), kOpAssign,
                      call(names::kPaykanArrayNewObj, {kZero}))));
      line(ifHead(envSet(kEnvNoArgs)) + kSpace +
           stmt(binop(kArgc, kOpAssign, kZero)));
      // for (int i = 0; i < argc; ++i) {
      line(openBlock(
          kFor + std::string(kSpace) +
          paren(stmt(binop(declare(kInt, kMainIndex), kOpAssign, kZero)) +
                kSpace + stmt(binop(kMainIndex, kOpLt, kArgc)) + kSpace +
                kOpPreInc + kMainIndex)));
      ++Indent;
      std::string arg = subscript(kArgv, kMainIndex);
      line(stmt(binop(declare(pointerTo(kRtString), kMainStr), kOpAssign,
                      call(names::kPaykanStringNew,
                           {arg, cast(kInt64) + call(kStrlen, {arg})}))));
      line(stmt(
          binop(declare(pointerTo(kRtShared), kMainBox), kOpAssign,
                call(names::kPaykanSharedNew, {cast(kRtObjPtr) + kMainStr}))));
      line(stmt(call(names::kPaykanArrayPushObj, {kMainArgs, kMainBox})));
      line(stmt(call(names::kPaykanRelease, {kMainBox})));
      --Indent;
      line(kRBrace);
    } else {
      line(stmt(cast(kVoid) + kArgc) + kSpace + stmt(cast(kVoid) + kArgv));
    }
    std::vector<std::string> mainArgs;
    if (takesArgs)
      mainArgs.push_back(
          call(names::kPaykanSharedNew, {cast(kRtObjPtr) + kMainArgs}));
    line(stmt(binop(declare(kInt64, kMainRc), kOpAssign,
                    call(symbolOf(main.Name, kMain), mainArgs))));
    line(ifHead(kMainTrack) + kSpace + stmt(call(kRtHeapDump, {})));
    line(stmt(kReturn + std::string(kSpace) + cast(kInt) + kMainRc));
    Indent = 0;
    line(kRBrace);
  }

public:
  Emitter(const pir::Program &p, std::ostream &o, std::ostream &e)
      : P(p), O(o), E(e) {}

  bool run(std::vector<size_t> unit) {
    if (P.Modules.empty()) {
      fail("empty program");
      return false;
    }
    Unit = std::move(unit);
    collectSymbols();
    emitPrelude();
    emitClassLayouts();
    emitPrototypes();
    emitVTables();
    emitNewHelpers();
    for (size_t mi : Unit) {
      CurMod = &P.Modules[mi];
      CurModIdx = mi;
      O << kModuleBannerOpen << CurMod->Name << kModuleBannerClose << kNewline
        << kNewline;
      emitGlobals(*CurMod);
      for (const auto &f : CurMod->Functions)
        if (!f.IsExtern)
          emitFunction(f);
    }
    if (inUnit(0)) {
      CurMod = &P.Modules[0];
      CurModIdx = 0;
      emitMain();
    }
    return !Failed;
  }
};

} // namespace

bool emitC(const pir::Program &program, std::ostream &out, std::ostream &errs) {
  std::vector<size_t> all;
  for (size_t i = 0; i < program.Modules.size(); ++i)
    all.push_back(i);
  Emitter em(program, out, errs);
  return em.run(std::move(all));
}

bool emitModuleC(const pir::Program &program, size_t module, std::ostream &out,
                 std::ostream &errs) {
  if (module >= program.Modules.size()) {
    errs << "C backend: no module " << module << "\n";
    return false;
  }
  Emitter em(program, out, errs);
  return em.run({module});
}

} // namespace paykan::backend_c
