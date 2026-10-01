// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// C backend: PIR -> C11 source.  One translation unit for the whole program;
// every module-defined symbol is mangled with its module so modules never
// collide, and runtime symbols are used by their C name with the casts
// Runtime.h's prototypes need.

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

// -- Runtime prototypes (Runtime.h) ------------------------------------------
//
// The C types of every runtime function the lowering may call, so arguments
// and results are cast between the PIR types (obj = PaykanObject*, box =
// PaykanShared*, ...) and the declared ones.

struct RuntimeProto {
  const char *Name;
  const char *Ret;
  std::vector<const char *> Params;
};

const char *kObjC = "PaykanObject *";
const char *kBoxC = "PaykanShared *";
const char *kStrC = "PaykanString *";
const char *kArrC = "PaykanArray *";
const char *kTupC = "PaykanTuple *";

const RuntimeProto kRuntimeProtos[] = {
    {names::kPaykanRetain, "void", {kBoxC}},
    {names::kPaykanRelease, "void", {kBoxC}},
    {names::kPaykanSharedNew, kBoxC, {kObjC}},
    {names::kPaykanSharedGet, kObjC, {kBoxC}},
    {names::kPaykanMalloc, "void *", {"size_t"}},
    {names::kPaykanFree, "void", {"void *"}},
    {names::kPaykanPanicDivByZero, "void", {}},
    {names::kPaykanPanicDivOverflow, "void", {}},
    {names::kPaykanStringNew, kStrC, {"const char *", "int64_t"}},
    {names::kPaykanStringDestroy, "void", {kObjC}},
    {names::kPaykanStringConcat, kObjC, {kObjC, kObjC}},
    {names::kPaykanStringCharAt, "int8_t", {kObjC, "int64_t"}},
    {names::kPaykanStringFromInt, kStrC, {"int64_t"}},
    {names::kPaykanStringFromFloat, kStrC, {"double"}},
    {names::kPaykanStringFromBool, kStrC, {"int64_t"}},
    {names::kPaykanStringFromChar, kStrC, {"int8_t"}},
    {names::kPaykanStringEquals, "int64_t", {kObjC, kObjC}},
    {names::kPaykanStringToString, kBoxC, {kObjC}},
    {names::kPaykanStringLength, "int64_t", {kObjC}},
    {names::kPaykanArrayNew, kArrC, {"unsigned long"}},
    {names::kPaykanArrayNewObj, kArrC, {"unsigned long"}},
    {names::kPaykanArrayNewFromData, kArrC, {"unsigned long", "const void *"}},
    {names::kPaykanArrayGet, "void *", {kArrC, "unsigned long"}},
    {names::kPaykanArraySet, "void", {kArrC, "unsigned long", "void *"}},
    {names::kPaykanArraySetObj, "void", {kArrC, "unsigned long", kBoxC}},
    {names::kPaykanArrayPush, "void", {kArrC, "void *"}},
    {names::kPaykanArrayPushObj, "void", {kArrC, kBoxC}},
    {names::kPaykanArrayPop, "void *", {kArrC}},
    {names::kPaykanArrayPopObj, kBoxC, {kArrC}},
    {names::kPaykanTupleNew, kTupC, {"int64_t", "const uint8_t *"}},
    {names::kPaykanTupleGet, "int64_t", {kTupC, "int64_t"}},
    {names::kPaykanTupleSet, "void", {kTupC, "int64_t", "int64_t"}},
    {names::kPaykanTupleSetObj, "void", {kTupC, "int64_t", kBoxC}},
    {names::kPaykanFileOpen, kBoxC, {kObjC, kObjC}},
    {names::kPaykanIntFromStr, kBoxC, {kObjC}},
    {names::kPaykanFloatFromStr, kBoxC, {kObjC}},
    {names::kPaykanPrint, "void", {kObjC}},
    {names::kPaykanPrintln, "void", {kObjC}},
    {names::kPaykanErrPrint, "void", {kObjC}},
    {names::kPaykanErrPrintln, "void", {kObjC}},
    {names::kPaykanObjectDestroy, "void", {kObjC}},
    {names::kPaykanObjectToString, kBoxC, {kObjC}},
    {names::kPaykanObjectEquals, "int64_t", {kObjC, kObjC}},
    {names::kPaykanFileDestroy, "void", {kObjC}},
    {names::kPaykanFileToString, kBoxC, {kObjC}},
    {names::kPaykanFileEquals, "int64_t", {kObjC, kObjC}},
    {names::kPaykanFileWrite, "void", {kObjC, kObjC}},
    {names::kPaykanFileReadln, kBoxC, {kObjC}},
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
    return "void";
  case Type::I64:
    return "int64_t";
  case Type::F64:
    return "double";
  case Type::Bool:
    return "bool";
  case Type::Char:
    return "int8_t";
  case Type::Box:
    return "PaykanShared *";
  case Type::Obj:
    return "PaykanObject *";
  case Type::Ptr:
    return "void *";
  }
  return "void";
}

bool isPointerCType(const std::string &c) {
  return c.find('*') != std::string::npos;
}
bool isPointerType(Type t) {
  return t == Type::Box || t == Type::Obj || t == Type::Ptr;
}

const std::set<std::string> kCKeywords = {
    "auto",     "break",    "case",     "char",   "const",   "continue",
    "default",  "do",       "double",   "else",   "enum",    "extern",
    "float",    "for",      "goto",     "if",     "inline",  "int",
    "long",     "register", "restrict", "return", "short",   "signed",
    "sizeof",   "static",   "struct",   "switch", "typedef", "union",
    "unsigned", "void",     "volatile", "while",  "bool",    "true",
    "false",    "main",     "argc",     "argv",   "NULL",    "INFINITY",
    "NAN",      "INT64_MIN"};

/// Keep [A-Za-z0-9_]; escape anything else as _XX (hex).
std::string sanitize(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '_') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "_%02X", c);
      out += buf;
    }
  }
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
    out = "_" + out;
  return out;
}

std::string escapeCString(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '?': // trigraph-proof
      out += "\\?";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      if (c >= 0x20 && c < 0x7f) {
        out += static_cast<char>(c);
      } else {
        // Exactly three octal digits: never swallows a following digit.
        char buf[6];
        std::snprintf(buf, sizeof buf, "\\%03o", c);
        out += buf;
      }
    }
  }
  return out;
}

std::string fmtI64(int64_t v) {
  if (v == INT64_MIN)
    return "INT64_MIN";
  return "INT64_C(" + std::to_string(v) + ")";
}

std::string fmtF64(double v) {
  if (std::isnan(v))
    return "NAN";
  if (std::isinf(v))
    return v < 0 ? "(-INFINITY)" : "INFINITY";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s = buf;
  if (s.find_first_of(".eE") == std::string::npos)
    s += ".0";
  return s;
}

// -- The emitter
// ---------------------------------------------------------------------

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

  std::string ind() const { return std::string(Indent * 2, ' '); }

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

  /// Reserve a unique C symbol for (module, name).
  void defineSymbol(const pir::Module &m, const std::string &name,
                    const std::string &base) {
    std::string key = symKey(m.Name, name);
    if (Symbols.count(key))
      return;
    std::string sym = base;
    unsigned n = 1;
    while (UsedSymbols.count(sym) || findProto(sym) || kCKeywords.count(sym))
      sym = base + "_" + std::to_string(n++);
    UsedSymbols.insert(sym);
    Symbols[key] = sym;
  }

  const std::string &symbolOf(const std::string &mod, const std::string &name) {
    static const std::string missing = "/*missing*/";
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
      return "/*undeclared*/";
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
    return d ? symbolOf(P.Modules[d->Module].Name, name) : "/*noclass*/";
  }
  std::string classStruct(const std::string &name) {
    return "struct " + classSymbol(name);
  }
  std::string classVTable(const std::string &name) {
    return classSymbol(name) + "_vtable";
  }
  std::string classNew(const std::string &name) {
    return classSymbol(name) + "__new";
  }

  std::string valueName(const pir::Value &v) {
    std::string nm = v.Name;
    for (char &c : nm)
      if (c == '.')
        c = '_';
    std::string base = nm.empty() ? "v" : sanitize(nm);
    return base + "_" + std::to_string(v.Id);
  }

  std::string operand(const pir::Operand &op, Type &ty) {
    if (auto *id = std::get_if<pir::ValueId>(&op.V)) {
      auto it = ValueNames.find(*id);
      if (it == ValueNames.end()) {
        fail("use of undefined value %" + std::to_string(*id));
        ty = Type::Void;
        return "/*undef*/0";
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
      return *b ? "true" : "false";
    }
    if (auto *c = std::get_if<char>(&op.V)) {
      ty = Type::Char;
      return "(int8_t)" + std::to_string(static_cast<int>(*c));
    }
    if (auto *n = std::get_if<pir::Operand::Null>(&op.V)) {
      ty = n->Ty;
      return "NULL";
    }
    if (auto *s = std::get_if<pir::SymbolRef>(&op.V))
      return symbolOperand(s->Name, ty);
    ty = Type::Void;
    return "/*?*/0";
  }

  std::string operand(const pir::Operand &op) {
    Type t;
    return operand(op, t);
  }

  std::string symbolOperand(const std::string &name, Type &ty) {
    for (const auto &g : CurMod->CStrs)
      if (g.Name == name) {
        ty = Type::Ptr;
        return "(void *)" + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Datas)
      if (g.Name == name) {
        ty = Type::Ptr;
        return "(void *)" + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Bytes)
      if (g.Name == name) {
        ty = Type::Ptr;
        return "(void *)" + symbolOf(CurMod->Name, name);
      }
    for (const auto &g : CurMod->Externs)
      if (g.Name == name) {
        if (g.K == pir::ExternGlobal::Object) {
          ty = Type::Obj;
          return "(PaykanObject *)&" + name;
        }
        ty = Type::Ptr;
        return "(void *)&" + name;
      }
    fail("unknown symbol '@" + name + "'");
    ty = Type::Void;
    return "/*unknown*/0";
  }

  /// Cast a PIR-typed C expression to a runtime parameter's C type.
  static std::string castTo(const std::string &expr, Type from,
                            const std::string &toC) {
    bool toPtr = isPointerCType(toC);
    if (toPtr && from == Type::I64)
      return "(" + toC + ")(intptr_t)" + expr;
    if (!toPtr && isPointerType(from))
      return "(" + toC + ")(intptr_t)" + expr;
    if (toC == "bool" || toC == "int64_t" || toC == "double" ||
        toC == "int8_t" || toC == "unsigned long" || toC == "size_t")
      return "(" + toC + ")" + expr;
    return "(" + toC + ")" + expr;
  }

  static std::string castResult(const std::string &call,
                                const std::string &fromC, Type to) {
    bool fromPtr = isPointerCType(fromC);
    if (fromPtr && to == Type::I64)
      return "(int64_t)(intptr_t)" + call;
    return "(" + std::string(cType(to)) + ")" + call;
  }

  void line(const std::string &s) { O << ind() << s << "\n"; }

  // -- Program-level passes
  // ---------------------------------------------------------

  void collectSymbols() {
    for (size_t i = 0; i < P.Modules.size(); ++i) {
      const pir::Module &m = P.Modules[i];
      ModuleIndex[m.Name] = i;
      std::string stem = moduleStem(m);
      for (const auto &c : m.Classes)
        if (!c.IsExtern) {
          if (ClassDefs.count(c.Name)) {
            fail("class '" + c.Name + "' is defined by two modules");
            continue;
          }
          ClassDefs[c.Name] = {i, &c};
          defineSymbol(m, c.Name, "pk_" + stem + "_" + sanitize(c.Name));
        }
      for (const auto &f : m.Functions)
        if (!f.IsExtern)
          defineSymbol(m, f.Name, "pk_" + stem + "_" + sanitize(f.Name));
      for (const auto &g : m.CStrs)
        defineSymbol(m, g.Name,
                     "pk_" + stem + "_str" + std::to_string(&g - &m.CStrs[0]));
      for (const auto &g : m.Datas)
        defineSymbol(m, g.Name,
                     "pk_" + stem + "_data" + std::to_string(&g - &m.Datas[0]));
      for (const auto &g : m.Bytes)
        defineSymbol(m, g.Name,
                     "pk_" + stem + "_kinds" +
                         std::to_string(&g - &m.Bytes[0]));
    }
  }

  std::string signatureC(const pir::Signature &sig, bool withNames,
                         const std::vector<pir::Value> *params) {
    std::string s = "(";
    if (sig.Params.empty())
      s += "void";
    for (size_t i = 0; i < sig.Params.size(); ++i) {
      if (i)
        s += ", ";
      s += cType(sig.Params[i]);
      if (withNames && params && i < params->size()) {
        if (!s.empty() && s.back() != '*')
          s += " ";
        s += valueName((*params)[i]);
      }
    }
    return s + ")";
  }

  void emitPrelude() {
    O << "/* Generated by the Paykan C backend. */\n"
         "#include <math.h>\n"
         "#include <stdbool.h>\n"
         "#include <stdint.h>\n"
         "#include <stdlib.h>\n"
         "#include <string.h>\n"
         "#include \"Runtime.h\"\n\n"
         "typedef void (*pk_fn)(void);\n\n"
         "static inline int64_t pk_f64_bits(double d) {\n"
         "  int64_t i; memcpy(&i, &d, sizeof i); return i;\n"
         "}\n"
         "static inline double pk_bits_f64(int64_t i) {\n"
         "  double d; memcpy(&d, &i, sizeof d); return d;\n"
         "}\n\n";
  }

  // -- Translation units
  // ---------------------------------------------------------
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
      O << classStruct(c->Name) << ";\n";
    if (!classes.empty())
      O << "\n";
    for (const pir::Class *c : classes) {
      O << classStruct(c->Name) << " { /* " << c->Name;
      if (!c->Super.empty())
        O << " : " << c->Super;
      O << " */\n";
      O << "  PaykanObjectVTable *vtable;\n  PaykanShared *shared;\n";
      for (const auto &f : c->Fields) {
        std::string ct = cType(f.Ty);
        O << "  " << ct << (ct.back() == '*' ? "" : " ") << sanitize(f.Name)
          << ";\n";
      }
      O << "};\n\n";
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
        std::string ret = cType(f.Sig.Ret);
        O << ret << (ret.back() == '*' ? "" : " ") << sym
          << signatureC(f.Sig, false, nullptr) << ";\n";
      }
    }
    O << "\n";
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
          O << "extern pk_fn " << classVTable(c.Name) << "[];\n";
          continue;
        }
        const pir::Class &def = *d->Cls;
        const pir::Module &defMod = P.Modules[d->Module];
        O << "pk_fn " << classVTable(c.Name) << "["
          << std::max<size_t>(def.VTable.size(), 1) << "] = {\n";
        for (const auto &e : def.VTable) {
          O << "  ";
          if (e.Target.empty())
            O << "NULL";
          else
            O << "(pk_fn)" << funcSymbol(defMod, e.Target);
          O << ", /* " << e.Slot << " */\n";
        }
        O << "};\n";
      }
    }
    O << "\n";
  }

  void emitNewHelpers() {
    for (size_t mi : Unit)
      for (const auto &c : P.Modules[mi].Classes) {
        if (c.IsExtern)
          continue;
        std::string s = classStruct(c.Name);
        O << "static PaykanObject *" << classNew(c.Name) << "(void) {\n"
          << "  " << s << " *o = (" << s << " *)Paykan_malloc(sizeof(" << s
          << "));\n"
          << "  o->vtable = (PaykanObjectVTable *)" << classVTable(c.Name)
          << ";\n  o->shared = NULL;\n";
        for (const auto &f : c.Fields) {
          O << "  o->" << sanitize(f.Name) << " = ";
          switch (f.Ty) {
          case Type::F64:
            O << "0.0";
            break;
          case Type::Bool:
            O << "false";
            break;
          case Type::Box:
          case Type::Obj:
          case Type::Ptr:
            O << "NULL";
            break;
          default:
            O << "0";
          }
          O << ";\n";
        }
        O << "  return (PaykanObject *)o;\n}\n\n";
      }
  }

  void emitGlobals(const pir::Module &m) {
    for (const auto &g : m.CStrs)
      O << "static const char " << symbolOf(m.Name, g.Name) << "["
        << g.Data.size() + 1 << "] = \"" << escapeCString(g.Data) << "\";\n";
    for (const auto &g : m.Datas) {
      O << "static const int64_t " << symbolOf(m.Name, g.Name) << "["
        << std::max<size_t>(g.Words.size(), 1) << "] = {";
      for (size_t i = 0; i < g.Words.size(); ++i)
        O << (i ? ", " : "") << fmtI64(g.Words[i]);
      O << "};\n";
    }
    for (const auto &g : m.Bytes) {
      O << "static const uint8_t " << symbolOf(m.Name, g.Name) << "["
        << std::max<size_t>(g.Bytes.size(), 1) << "] = {";
      for (size_t i = 0; i < g.Bytes.size(); ++i)
        O << (i ? ", " : "") << static_cast<int>(g.Bytes[i]);
      O << "};\n";
    }
    if (!m.CStrs.empty() || !m.Datas.empty() || !m.Bytes.empty())
      O << "\n";
  }

  // -- Functions
  // ----------------------------------------------------------------------

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

    std::string ret = cType(f.Sig.Ret);
    O << ret << (ret.back() == '*' ? "" : " ") << symbolOf(CurMod->Name, f.Name)
      << signatureC(f.Sig, true, &f.Params) << " {\n";
    Indent = 1;

    // Locals: readable names when unique, suffixed otherwise.
    std::set<std::string> used;
    for (const auto &p : f.Params)
      used.insert(valueName(p));
    for (size_t i = 0; i < f.Locals.size(); ++i) {
      std::string base = sanitize(f.Locals[i].Name);
      std::string nm = base;
      if (kCKeywords.count(nm) || used.count(nm) || findProto(nm) ||
          UsedSymbols.count(nm) || nm.rfind("pk_", 0) == 0)
        nm = base + "_l" + std::to_string(i);
      while (used.count(nm))
        nm += "_";
      used.insert(nm);
      LocalNames.push_back(nm);
      std::string ct = cType(f.Locals[i].Ty);
      line(ct + (ct.back() == '*' ? "" : " ") + nm + ";");
    }
    emitBlock(f.Body);
    Indent = 0;
    O << "}\n\n";
  }

  void emitBlock(const pir::Block &b) {
    for (const auto &st : b.Stmts)
      emitStmt(st);
  }

  void emitStmt(const pir::Stmt &st) {
    if (auto *i = std::get_if<pir::Instr>(&st)) {
      emitInstr(*i);
    } else if (auto *s = std::get_if<pir::If>(&st)) {
      if (s->Then->Stmts.empty() && s->Else && !s->Else->Stmts.empty()) {
        line("if (!" + operand(s->Cond) + ") {");
        ++Indent;
        emitBlock(*s->Else);
        --Indent;
        line("}");
        return;
      }
      line("if (" + operand(s->Cond) + ") {");
      ++Indent;
      emitBlock(*s->Then);
      --Indent;
      if (s->Else && !s->Else->Stmts.empty()) {
        line("} else {");
        ++Indent;
        emitBlock(*s->Else);
        --Indent;
      }
      line("}");
    } else if (auto *w = std::get_if<pir::While>(&st)) {
      line("for (;;) {");
      ++Indent;
      emitBlock(*w->CondBlock);
      line("if (!(" + operand(w->Cond) + ")) break;");
      emitBlock(*w->Body);
      --Indent;
      line("}");
    } else if (std::holds_alternative<pir::Break>(st)) {
      line("break;");
    } else if (std::holds_alternative<pir::Continue>(st)) {
      line("continue;");
    } else if (auto *r = std::get_if<pir::Return>(&st)) {
      if (r->Value)
        line("return " + operand(*r->Value) + ";");
      else
        line("return;");
    } else if (std::holds_alternative<pir::Unreachable>(st)) {
      line("abort(); /* unreachable */");
    }
  }

  std::string resultPrefix(const pir::Instr &i) {
    if (i.Result.Ty == Type::Void)
      return "";
    defineValue(i.Result);
    std::string ct = cType(i.Result.Ty);
    return ct + (ct.back() == '*' ? "" : " ") + ValueNames[i.Result.Id] + " = ";
  }

  std::string arith(const char *op, const pir::Instr &i) {
    Type ta, tb;
    std::string a = operand(i.Args[0], ta);
    std::string b = operand(i.Args[1], tb);
    if (ta == Type::F64)
      return a + " " + op + " " + b;
    // Wrapping two's-complement arithmetic (signed overflow is UB in C).
    return "(int64_t)((uint64_t)" + a + " " + op + " (uint64_t)" + b + ")";
  }

  void emitInstr(const pir::Instr &i) {
    using pir::Opcode;
    std::string pre;
    switch (i.Op) {
    case Opcode::Add:
      pre = resultPrefix(i);
      line(pre + arith("+", i) + ";");
      return;
    case Opcode::Sub:
      pre = resultPrefix(i);
      line(pre + arith("-", i) + ";");
      return;
    case Opcode::Mul:
      pre = resultPrefix(i);
      line(pre + arith("*", i) + ";");
      return;
    case Opcode::Div: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      line(pre + a + " / " + b + ";");
      return;
    }
    case Opcode::Rem: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      if (ta == Type::F64)
        line(pre + "fmod(" + a + ", " + b + ");");
      else
        line(pre + a + " % " + b + ";");
      return;
    }
    case Opcode::Neg: {
      pre = resultPrefix(i);
      Type ta;
      std::string a = operand(i.Args[0], ta);
      if (ta == Type::F64)
        line(pre + "-" + a + ";");
      else
        line(pre + "(int64_t)(0 - (uint64_t)" + a + ");");
      return;
    }
    case Opcode::Not:
      pre = resultPrefix(i);
      line(pre + "!" + operand(i.Args[0]) + ";");
      return;
    case Opcode::Cmp: {
      pre = resultPrefix(i);
      Type ta, tb;
      std::string a = operand(i.Args[0], ta), b = operand(i.Args[1], tb);
      const char *op = "==";
      switch (i.Pred) {
      case pir::CmpPred::Eq:
        op = "==";
        break;
      case pir::CmpPred::Ne:
        op = "!=";
        break;
      case pir::CmpPred::Lt:
        op = "<";
        break;
      case pir::CmpPred::Le:
        op = "<=";
        break;
      case pir::CmpPred::Gt:
        op = ">";
        break;
      case pir::CmpPred::Ge:
        op = ">=";
        break;
      }
      if (ta == Type::F64 && i.Pred == pir::CmpPred::Ne) {
        // Ordered "not equal": false for NaN operands (unlike C's !=).
        line(pre + "(" + a + " < " + b + " || " + a + " > " + b + ");");
        return;
      }
      line(pre + "(" + a + " " + op + " " + b + ");");
      return;
    }
    case Opcode::Select:
      pre = resultPrefix(i);
      line(pre + "(" + operand(i.Args[0]) + " ? " + operand(i.Args[1]) + " : " +
           operand(i.Args[2]) + ");");
      return;
    case Opcode::IToF:
      pre = resultPrefix(i);
      line(pre + "(double)" + operand(i.Args[0]) + ";");
      return;
    case Opcode::Cast: {
      pre = resultPrefix(i);
      Type from;
      std::string a = operand(i.Args[0], from);
      Type to = i.CastTo;
      std::string expr;
      if (from == Type::F64 && to == Type::I64)
        expr = "pk_f64_bits(" + a + ")";
      else if (from == Type::I64 && to == Type::F64)
        expr = "pk_bits_f64(" + a + ")";
      else if (to == Type::Bool)
        expr = "((" + a + " & 1) != 0)";
      else if (to == Type::I64 && from == Type::Char)
        expr = "(int64_t)(uint8_t)" + a;
      else if (to == Type::I64 && from == Type::Bool)
        expr = "(int64_t)" + a;
      else if (to == Type::Char)
        expr = "(int8_t)" + a;
      else if (to == Type::I64 && isPointerType(from))
        expr = "(int64_t)(intptr_t)" + a;
      else if (isPointerType(to) && from == Type::I64)
        expr = "(" + std::string(cType(to)) + ")(intptr_t)" + a;
      else
        expr = "(" + std::string(cType(to)) + ")" + a;
      line(pre + expr + ";");
      return;
    }
    case Opcode::Call: {
      const pir::Function *callee = CurMod->findFunction(i.Callee);
      if (!callee) {
        fail("call to undeclared function '" + i.Callee + "'");
        return;
      }
      pre = resultPrefix(i);
      std::string call;
      if (callee->IsExtern && callee->Module.empty()) {
        const RuntimeProto *proto = findProto(callee->Name);
        if (!proto) {
          fail("unknown runtime function '" + callee->Name + "'");
          return;
        }
        call = callee->Name + "(";
        for (size_t a = 0; a < i.Args.size(); ++a) {
          Type ta;
          std::string arg = operand(i.Args[a], ta);
          if (a)
            call += ", ";
          call += a < proto->Params.size() ? castTo(arg, ta, proto->Params[a])
                                           : arg;
        }
        call += ")";
        if (callee->Sig.Ret != Type::Void)
          call = castResult(call, proto->Ret, callee->Sig.Ret);
      } else {
        call = funcSymbol(*CurMod, i.Callee) + "(";
        for (size_t a = 0; a < i.Args.size(); ++a) {
          if (a)
            call += ", ";
          call += operand(i.Args[a]);
        }
        call += ")";
      }
      line(pre + call + ";");
      return;
    }
    case Opcode::VCall: {
      pre = resultPrefix(i);
      Type tr;
      std::string recv = operand(i.Args[0], tr);
      std::string fnTy = std::string(cType(i.Sig.Ret)) + " (*)(";
      for (size_t p = 0; p < i.Sig.Params.size(); ++p)
        fnTy += std::string(p ? ", " : "") + cType(i.Sig.Params[p]);
      if (i.Sig.Params.empty())
        fnTy += "void";
      fnTy += ")";
      std::string call = "((" + fnTy + ")((pk_fn *)(" + recv + ")->vtable)[" +
                         std::to_string(i.Slot) + "])(";
      for (size_t a = 0; a < i.Args.size(); ++a) {
        if (a)
          call += ", ";
        call += operand(i.Args[a]);
      }
      call += ")";
      std::string comment = i.ClassName.empty()
                                ? ""
                                : " /* " + i.ClassName + " slot " +
                                      std::to_string(i.Slot) + " */";
      line(pre + call + ";" + comment);
      return;
    }
    case Opcode::Retain:
      line("Paykan_retain(" + operand(i.Args[0]) + ");");
      return;
    case Opcode::Release:
      line("Paykan_release(" + operand(i.Args[0]) + ");");
      return;
    case Opcode::Box:
      pre = resultPrefix(i);
      line(pre + "PaykanShared_new(" + operand(i.Args[0]) + ");");
      return;
    case Opcode::Unbox:
      pre = resultPrefix(i);
      line(pre + "PaykanShared_get(" + operand(i.Args[0]) + ");");
      return;
    case Opcode::New:
      pre = resultPrefix(i);
      line(pre + classNew(i.ClassName) + "();");
      return;
    case Opcode::Free:
      line("Paykan_free(" + operand(i.Args[0]) + ");");
      return;
    case Opcode::FieldLoad:
      pre = resultPrefix(i);
      line(pre + "((" + classStruct(i.ClassName) + " *)" + operand(i.Args[0]) +
           ")->" + sanitize(i.Field) + ";");
      return;
    case Opcode::FieldStore:
      line("((" + classStruct(i.ClassName) + " *)" + operand(i.Args[0]) +
           ")->" + sanitize(i.Field) + " = " + operand(i.Args[1]) + ";");
      return;
    case Opcode::VTableLoad:
      pre = resultPrefix(i);
      line(pre + "(void *)(" + operand(i.Args[0]) + ")->vtable;");
      return;
    case Opcode::VTableAddr: {
      pre = resultPrefix(i);
      if (!i.ClassName.empty()) {
        line(pre + "(void *)" + classVTable(i.ClassName) + ";");
        return;
      }
      // An extern (runtime) vtable global, named by the symbol operand.
      const auto *sym =
          i.Args.empty() ? nullptr : std::get_if<pir::SymbolRef>(&i.Args[0].V);
      if (!sym) {
        fail("vtable.addr without a class or a symbol");
        return;
      }
      line(pre + "(void *)&" + sym->Name + ";");
      return;
    }
    case Opcode::Load:
      pre = resultPrefix(i);
      if (i.Local >= LocalNames.size()) {
        fail("load of an undeclared local");
        return;
      }
      line(pre + LocalNames[i.Local] + ";");
      return;
    case Opcode::Store:
      if (i.Local >= LocalNames.size()) {
        fail("store to an undeclared local");
        return;
      }
      line(LocalNames[i.Local] + " = " + operand(i.Args[0]) + ";");
      return;
    }
  }

  void emitMain() {
    const pir::Module &main = P.Modules[0];
    const pir::Function *mainFn = main.findFunction("main");
    if (!mainFn || mainFn->IsExtern) {
      fail("the main module defines no 'main' function");
      return;
    }
    bool takesArgs = mainFn->Sig.Params.size() == 1;
    O << "int main(int argc, char **argv) {\n"
         "  int track = getenv(\"PAYKAN_TRACK_HEAP\") != NULL;\n"
         "  if (track) { Paykan_heap_set_tracking(1); Paykan_heap_reset(); }\n";
    if (takesArgs) {
      // PAYKAN_NO_ARGS: run with an empty argument list (the kernel always
      // supplies an argv[0], so a caller that wants argc == 0 says so).
      O << "  PaykanArray *args = PaykanArray_new_obj(0);\n"
           "  if (getenv(\"PAYKAN_NO_ARGS\") != NULL) argc = 0;\n"
           "  for (int i = 0; i < argc; ++i) {\n"
           "    PaykanString *s = PaykanString_new(argv[i], "
           "(int64_t)strlen(argv[i]));\n"
           "    PaykanShared *b = PaykanShared_new((PaykanObject *)s);\n"
           "    PaykanArray_push_obj(args, b);\n"
           "    Paykan_release(b);\n"
           "  }\n";
    } else {
      O << "  (void)argc; (void)argv;\n";
    }
    O << "  int64_t rc = " << symbolOf(main.Name, "main") << "(";
    if (takesArgs)
      O << "PaykanShared_new((PaykanObject *)args)";
    O << ");\n"
         "  if (track) Paykan_heap_dump();\n"
         "  return (int)rc;\n"
         "}\n";
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
      O << "/* ---- module " << CurMod->Name << " ---- */\n\n";
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
