// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text printer.  The format is specified in docs/pir.md §10; the parser
// (Parser.cpp) reads exactly what this prints.

#include "paykan/pir/Printer.h"

#include <cmath>
#include <cstdio>
#include <ostream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace paykan::pir {

namespace {

/// Characters that may appear unquoted in a `@sym` / `%value` / class name.
bool isPlainNameChar(char c) {
  switch (c) {
  case ' ':
  case '\t':
  case '\n':
  case '\r':
  case '(':
  case ')':
  case ',':
  case '[':
  case ']':
  case '{':
  case '}':
  case ':':
  case '"':
    return false;
  default:
    return static_cast<unsigned char>(c) > 0x20 &&
           static_cast<unsigned char>(c) < 0x7f;
  }
}

void printEscaped(std::string_view s, std::ostream &os) {
  os << '"';
  for (unsigned char c : s) {
    switch (c) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\t':
      os << "\\t";
      break;
    case '\r':
      os << "\\r";
      break;
    case '\0':
      os << "\\0";
      break;
    default:
      if (c < 0x20 || c >= 0x7f) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\x%02x", c);
        os << buf;
      } else {
        os << static_cast<char>(c);
      }
    }
  }
  os << '"';
}

/// A symbol / class / field name: bare when every character is plain,
/// quoted otherwise.
void printName(std::string_view name, std::ostream &os) {
  bool plain = !name.empty();
  for (char c : name)
    plain = plain && isPlainNameChar(c);
  if (plain)
    os << name;
  else
    printEscaped(name, os);
}

void printSymbol(std::string_view name, std::ostream &os) {
  os << '@';
  printName(name, os);
}

void printValueRef(const Value &v, std::ostream &os) {
  os << '%';
  if (!v.Name.empty()) {
    printName(v.Name, os);
    os << '.';
  }
  os << v.Id;
}

void printChar(char c, std::ostream &os) {
  os << '\'';
  switch (c) {
  case '\\':
    os << "\\\\";
    break;
  case '\'':
    os << "\\'";
    break;
  case '\n':
    os << "\\n";
    break;
  case '\t':
    os << "\\t";
    break;
  case '\r':
    os << "\\r";
    break;
  case '\0':
    os << "\\0";
    break;
  default: {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u >= 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", u);
      os << buf;
    } else {
      os << c;
    }
  }
  }
  os << '\'';
}

void printF64(double d, std::ostream &os) {
  if (std::isnan(d)) {
    os << "nan";
    return;
  }
  if (std::isinf(d)) {
    os << (d < 0 ? "-inf" : "inf");
    return;
  }
  // Shortest form that round-trips, always marked as a float.
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.17g", d);
  std::string_view s(buf);
  os << s;
  if (s.find_first_of(".eE") == std::string_view::npos)
    os << ".0";
}

const char *predName(CmpPred p) {
  switch (p) {
  case CmpPred::Eq:
    return "eq";
  case CmpPred::Ne:
    return "ne";
  case CmpPred::Lt:
    return "lt";
  case CmpPred::Le:
    return "le";
  case CmpPred::Gt:
    return "gt";
  case CmpPred::Ge:
    return "ge";
  }
  return "?";
}

/// Prints one function body.  Operands only carry value ids, so the printer
/// looks names up in the function's definitions.
class FunctionPrinter {
public:
  FunctionPrinter(const Function &f, std::ostream &os) : F(f), OS(os) {
    for (const Value &p : f.Params)
      Names[p.Id] = &p;
    collect(f.Body);
  }

  void printBody() {
    for (size_t i = 0; i < F.Locals.size(); ++i) {
      indent(1);
      OS << "local ";
      printLocalRef(static_cast<LocalId>(i));
      OS << ": " << typeName(F.Locals[i].Ty) << "\n";
    }
    printBlock(F.Body, 1);
  }

  void printOperand(const Operand &op) {
    if (op.isValue()) {
      auto it = Names.find(op.valueId());
      if (it != Names.end())
        printValueRef(*it->second, OS);
      else
        OS << '%' << op.valueId();
      return;
    }
    print(op, OS);
  }

private:
  const Function &F;
  std::ostream &OS;
  std::unordered_map<ValueId, const Value *> Names;

  void collect(const Block &b) {
    for (const Stmt &s : b.Stmts) {
      if (const auto *i = std::get_if<Instr>(&s)) {
        if (i->Result.Id != kNoValue)
          Names[i->Result.Id] = &i->Result;
      } else if (const auto *i2 = std::get_if<If>(&s)) {
        if (i2->Then)
          collect(*i2->Then);
        if (i2->Else)
          collect(*i2->Else);
      } else if (const auto *w = std::get_if<While>(&s)) {
        if (w->CondBlock)
          collect(*w->CondBlock);
        if (w->Body)
          collect(*w->Body);
      }
    }
  }

  void indent(unsigned depth) {
    for (unsigned i = 0; i < depth; ++i)
      OS << "  ";
  }

  /// Locals print as `%name.index`: several locals may share a name (two
  /// match arms binding `a`, a name declared in sibling scopes), and the index
  /// keeps the reference unambiguous.
  void printLocalRef(LocalId id) {
    OS << '%';
    if (id < F.Locals.size()) {
      if (!F.Locals[id].Name.empty()) {
        printName(F.Locals[id].Name, OS);
        OS << '.';
      }
      OS << id;
    } else {
      OS << "<local " << id << ">";
    }
  }

  void printArgs(const std::vector<Operand> &args, size_t from) {
    OS << '(';
    for (size_t i = from; i < args.size(); ++i) {
      if (i > from)
        OS << ", ";
      printOperand(args[i]);
    }
    OS << ')';
  }

  void printOperandList(const std::vector<Operand> &args, size_t from = 0) {
    for (size_t i = from; i < args.size(); ++i) {
      if (i > from)
        OS << ", ";
      printOperand(args[i]);
    }
  }

  void printInstr(const Instr &in) {
    if (in.Result.Id != kNoValue) {
      printValueRef(in.Result, OS);
      OS << " = ";
    }
    OS << opcodeName(in.Op);
    switch (in.Op) {
    case Opcode::Cmp:
      OS << ' ' << predName(in.Pred) << ' ';
      printOperandList(in.Args);
      break;
    case Opcode::Cast:
      OS << ' ';
      printOperandList(in.Args);
      OS << " to " << typeName(in.CastTo);
      break;
    case Opcode::Call:
      OS << ' ';
      printSymbol(in.Callee, OS);
      printArgs(in.Args, 0);
      break;
    case Opcode::VCall:
      OS << ' ';
      if (!in.Args.empty())
        printOperand(in.Args[0]);
      OS << " : ";
      if (!in.ClassName.empty())
        printName(in.ClassName, OS);
      else
        print(in.Sig, OS);
      OS << " [" << in.Slot << "] ";
      printArgs(in.Args, 1);
      break;
    case Opcode::New:
      OS << ' ';
      printName(in.ClassName, OS);
      break;
    case Opcode::FieldLoad:
    case Opcode::FieldStore:
      OS << ' ';
      if (!in.Args.empty())
        printOperand(in.Args[0]);
      OS << ", ";
      printName(in.ClassName, OS);
      OS << '.';
      printName(in.Field, OS);
      if (in.Op == Opcode::FieldStore && in.Args.size() > 1) {
        OS << ", ";
        printOperand(in.Args[1]);
      }
      break;
    case Opcode::VTableAddr:
      OS << ' ';
      if (!in.ClassName.empty())
        printName(in.ClassName, OS);
      else if (!in.Args.empty())
        printOperand(in.Args[0]);
      break;
    case Opcode::Load:
      OS << ' ';
      printLocalRef(in.Local);
      break;
    case Opcode::Store:
      OS << ' ';
      printLocalRef(in.Local);
      OS << ", ";
      printOperandList(in.Args);
      break;
    default:
      if (!in.Args.empty()) {
        OS << ' ';
        printOperandList(in.Args);
      }
      break;
    }
    OS << "\n";
  }

  void printBlock(const Block &b, unsigned depth) {
    for (const Stmt &s : b.Stmts) {
      indent(depth);
      if (const auto *in = std::get_if<Instr>(&s)) {
        printInstr(*in);
      } else if (const auto *i = std::get_if<If>(&s)) {
        OS << "if ";
        printOperand(i->Cond);
        OS << " {\n";
        if (i->Then)
          printBlock(*i->Then, depth + 1);
        indent(depth);
        OS << "}";
        if (i->Else) {
          OS << " else {\n";
          printBlock(*i->Else, depth + 1);
          indent(depth);
          OS << "}";
        }
        OS << "\n";
      } else if (const auto *w = std::get_if<While>(&s)) {
        OS << "while {\n";
        if (w->CondBlock)
          printBlock(*w->CondBlock, depth + 1);
        indent(depth + 1);
        OS << "cond ";
        printOperand(w->Cond);
        OS << "\n";
        indent(depth);
        OS << "} {\n";
        if (w->Body)
          printBlock(*w->Body, depth + 1);
        indent(depth);
        OS << "}\n";
      } else if (std::holds_alternative<Break>(s)) {
        OS << "break\n";
      } else if (std::holds_alternative<Continue>(s)) {
        OS << "continue\n";
      } else if (const auto *r = std::get_if<Return>(&s)) {
        OS << "ret";
        if (r->Value) {
          OS << ' ';
          printOperand(*r->Value);
        }
        OS << "\n";
      } else {
        OS << "unreachable\n";
      }
    }
  }
};

void printFunction(const Function &f, std::ostream &os) {
  if (f.IsExtern) {
    os << "extern fn ";
    printSymbol(f.Name, os);
    print(f.Sig, os);
    if (!f.Module.empty()) {
      os << " module ";
      printEscaped(f.Module, os);
    }
    os << "\n";
    return;
  }
  os << "fn ";
  printSymbol(f.Name, os);
  os << '(';
  for (size_t i = 0; i < f.Params.size(); ++i) {
    if (i)
      os << ", ";
    printValueRef(f.Params[i], os);
    os << ": " << typeName(f.Params[i].Ty);
  }
  os << ") -> " << typeName(f.Sig.Ret) << " {\n";
  FunctionPrinter(f, os).printBody();
  os << "}\n";
}

void printClass(const Class &c, std::ostream &os) {
  if (c.IsExtern)
    os << "extern ";
  os << "class ";
  printName(c.Name, os);
  if (!c.Super.empty()) {
    os << " : ";
    printName(c.Super, os);
  }
  if (c.IsExtern && !c.Module.empty()) {
    os << " module ";
    printEscaped(c.Module, os);
  }
  os << " {\n";
  for (const Field &f : c.Fields) {
    os << "  field ";
    printName(f.Name, os);
    os << ": " << typeName(f.Ty) << "\n";
  }
  if (!c.VTable.empty()) {
    os << "  vtable {\n";
    for (const VTableEntry &e : c.VTable) {
      os << "    ";
      printName(e.Slot, os);
      os << " = ";
      if (e.Target.empty())
        os << "null";
      else
        printSymbol(e.Target, os);
      os << " : ";
      print(e.Sig, os);
      os << "\n";
    }
    os << "  }\n";
  }
  os << "}\n";
}

} // namespace

const char *opcodeName(Opcode op) {
  switch (op) {
  case Opcode::Add:
    return "add";
  case Opcode::Sub:
    return "sub";
  case Opcode::Mul:
    return "mul";
  case Opcode::Div:
    return "div";
  case Opcode::Rem:
    return "rem";
  case Opcode::Neg:
    return "neg";
  case Opcode::Not:
    return "not";
  case Opcode::Cmp:
    return "cmp";
  case Opcode::Select:
    return "select";
  case Opcode::IToF:
    return "itof";
  case Opcode::FToI:
    return "ftoi";
  case Opcode::Cast:
    return "cast";
  case Opcode::Call:
    return "call";
  case Opcode::VCall:
    return "vcall";
  case Opcode::Retain:
    return "retain";
  case Opcode::Release:
    return "release";
  case Opcode::Box:
    return "box";
  case Opcode::Unbox:
    return "unbox";
  case Opcode::New:
    return "new";
  case Opcode::Free:
    return "free";
  case Opcode::FieldLoad:
    return "field.load";
  case Opcode::FieldStore:
    return "field.store";
  case Opcode::VTableLoad:
    return "vtable.load";
  case Opcode::VTableAddr:
    return "vtable.addr";
  case Opcode::Load:
    return "load";
  case Opcode::Store:
    return "store";
  }
  return "?";
}

void print(const Signature &sig, std::ostream &os) {
  os << '(';
  for (size_t i = 0; i < sig.Params.size(); ++i) {
    if (i)
      os << ", ";
    os << typeName(sig.Params[i]);
  }
  os << ") -> " << typeName(sig.Ret);
}

void print(const Operand &op, std::ostream &os) {
  struct Visitor {
    std::ostream &OS;
    void operator()(ValueId id) const { OS << '%' << id; }
    void operator()(int64_t v) const { OS << v; }
    void operator()(double v) const { printF64(v, OS); }
    void operator()(bool v) const { OS << (v ? "true" : "false"); }
    void operator()(char v) const { printChar(v, OS); }
    void operator()(const Operand::Null &n) const {
      OS << "null " << typeName(n.Ty);
    }
    void operator()(const SymbolRef &s) const { printSymbol(s.Name, OS); }
  };
  std::visit(Visitor{os}, op.V);
}

void print(const Module &m, std::ostream &os) {
  os << "module ";
  printEscaped(m.Name, os);
  os << "\n";
  for (const CStrGlobal &g : m.CStrs) {
    os << "cstr ";
    printSymbol(g.Name, os);
    os << " = ";
    printEscaped(g.Data, os);
    os << " len " << g.Data.size() << "\n";
  }
  for (const DataGlobal &g : m.Datas) {
    os << "data ";
    printSymbol(g.Name, os);
    os << " = [";
    for (size_t i = 0; i < g.Words.size(); ++i)
      os << (i ? ", " : "") << g.Words[i];
    os << "]\n";
  }
  for (const BytesGlobal &g : m.Bytes) {
    os << "bytes ";
    printSymbol(g.Name, os);
    os << " = [";
    for (size_t i = 0; i < g.Bytes.size(); ++i)
      os << (i ? ", " : "") << static_cast<unsigned>(g.Bytes[i]);
    os << "]\n";
  }
  for (const ExternGlobal &g : m.Externs) {
    os << "extern " << (g.K == ExternGlobal::Object ? "obj " : "vtable ");
    printSymbol(g.Name, os);
    os << "\n";
  }
  for (const Class &c : m.Classes) {
    os << "\n";
    printClass(c, os);
  }
  // Extern declarations form one group; every definition gets its own
  // paragraph.
  bool first = true;
  for (const Function &f : m.Functions) {
    if (!f.IsExtern || first)
      os << "\n";
    first = false;
    printFunction(f, os);
  }
}

void print(const Program &p, std::ostream &os) {
  for (size_t i = 0; i < p.Modules.size(); ++i) {
    if (i)
      os << "\n";
    print(p.Modules[i], os);
  }
}

std::string toString(const Module &m) {
  std::ostringstream os;
  print(m, os);
  return os.str();
}

std::string toString(const Program &p) {
  std::ostringstream os;
  print(p, os);
  return os.str();
}

} // namespace paykan::pir
