// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// PIR text parser: reads the format of docs/pir.md §10 (what Printer.cpp
// prints) into the in-memory form.  Recursive descent over a small token
// stream; everything the verifier checks (types, declarations, scoping) is
// left to the verifier, so a parsed module is well formed but not yet valid.

#include "paykan/pir/Parser.h"

#include "paykan/pir/Codes.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace paykan::pir {

std::string ParseError::str() const {
  if (Line == 0)
    return Message;
  return std::to_string(Line) + ":" + std::to_string(Column) + ": " + Message;
}

namespace {

// -- Lexer

enum class Tok {
  End,
  Word,     // bare identifier / keyword / type name: Text
  Symbol,   // @name: Text = name
  ValueRef, // %name: Text = name (as written)
  String,   // "...": Text = unescaped contents
  Int,      // Text = digits (with sign)
  Float,    // Text = literal
  Char,     // Text = the one character
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Colon,
  Equals,
  Arrow,
  Dot,
};

struct Token {
  Tok Kind = Tok::End;
  std::string Text;
  unsigned Line = 0;
  unsigned Column = 0;
};

bool isNameChar(char c) {
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
    return static_cast<unsigned char>(c) > 0x20;
  }
}

class Lexer {
public:
  Lexer(std::string_view text, ParseError &err) : Text(text), Err(err) {}

  /// The next token; on a lexical error sets Err and returns End.
  Token next() {
    skipSpace();
    Token t;
    t.Line = Line;
    t.Column = Col;
    if (Pos >= Text.size())
      return t;
    char c = Text[Pos];
    auto single = [&](Tok k) {
      advance();
      t.Kind = k;
      return t;
    };
    switch (c) {
    case '(':
      return single(Tok::LParen);
    case ')':
      return single(Tok::RParen);
    case '{':
      return single(Tok::LBrace);
    case '}':
      return single(Tok::RBrace);
    case '[':
      return single(Tok::LBracket);
    case ']':
      return single(Tok::RBracket);
    case ',':
      return single(Tok::Comma);
    case ':':
      return single(Tok::Colon);
    case '=':
      return single(Tok::Equals);
    case '"':
      t.Kind = Tok::String;
      if (!lexString(t.Text))
        return fail(t, "unterminated or malformed string");
      return t;
    case '\'':
      t.Kind = Tok::Char;
      if (!lexChar(t.Text))
        return fail(t, "malformed character literal");
      return t;
    case '@':
    case '%': {
      advance();
      t.Kind = c == '@' ? Tok::Symbol : Tok::ValueRef;
      if (Pos < Text.size() && Text[Pos] == '"') {
        if (!lexString(t.Text))
          return fail(t, "unterminated or malformed quoted name");
        // A quoted value name keeps its `.N` index outside the quotes
        // (`%"Pair<Str, int>.shared".4`); fold it into the token so the
        // text reads like the bare spelling `%name.N`.
        if (c == '%' && Pos + 1 < Text.size() && Text[Pos] == '.' &&
            std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
          t.Text.push_back('.');
          advance();
          while (Pos < Text.size() &&
                 std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            t.Text.push_back(Text[Pos]);
            advance();
          }
        }
      } else {
        lexName(t.Text);
      }
      if (t.Text.empty())
        return fail(t, std::string("empty name after '") + c + "'");
      return t;
    }
    default:
      break;
    }
    if (c == '-' && Pos + 1 < Text.size() && Text[Pos + 1] == '>') {
      advance();
      advance();
      t.Kind = Tok::Arrow;
      return t;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && Pos + 1 < Text.size() &&
         std::isdigit(static_cast<unsigned char>(Text[Pos + 1])))) {
      return lexNumber(t);
    }
    if (c == '.') {
      // A field separator (`Point.x`); names never start with '.', except
      // symbols (`@.str0`), which are lexed above.
      return single(Tok::Dot);
    }
    if (isNameChar(c)) {
      t.Kind = Tok::Word;
      lexName(t.Text);
      // `inf` / `nan` / `-inf` as float literals.
      if (t.Text == "inf" || t.Text == "nan" || t.Text == "-inf")
        t.Kind = Tok::Float;
      return t;
    }
    return fail(t, std::string("unexpected character '") + c + "'");
  }

private:
  std::string_view Text;
  ParseError &Err;
  size_t Pos = 0;
  unsigned Line = 1;
  unsigned Col = 1;

  void advance() {
    if (Text[Pos] == '\n') {
      ++Line;
      Col = 1;
    } else {
      ++Col;
    }
    ++Pos;
  }

  void skipSpace() {
    while (Pos < Text.size()) {
      char c = Text[Pos];
      if (c == ';') {
        while (Pos < Text.size() && Text[Pos] != '\n')
          advance();
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        advance();
      } else {
        break;
      }
    }
  }

  Token fail(Token t, std::string msg) {
    if (Err.Message.empty()) {
      Err.Line = t.Line;
      Err.Column = t.Column;
      Err.Message = std::move(msg);
    }
    t.Kind = Tok::End;
    return t;
  }

  /// Bare name: everything up to a delimiter.  A '.' followed by digits at
  /// the end of a `%name.N` is part of the name here; the parser splits it.
  void lexName(std::string &out) {
    while (Pos < Text.size() && isNameChar(Text[Pos])) {
      out.push_back(Text[Pos]);
      advance();
    }
  }

  bool lexEscape(std::string &out) {
    // Pos is at the character after the backslash.
    if (Pos >= Text.size())
      return false;
    char e = Text[Pos];
    advance();
    switch (e) {
    case 'n':
      out.push_back('\n');
      return true;
    case 't':
      out.push_back('\t');
      return true;
    case 'r':
      out.push_back('\r');
      return true;
    case '0':
      out.push_back('\0');
      return true;
    case '\\':
    case '"':
    case '\'':
      out.push_back(e);
      return true;
    case 'x': {
      if (Pos + 1 >= Text.size())
        return false;
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9')
          return h - '0';
        if (h >= 'a' && h <= 'f')
          return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
          return h - 'A' + 10;
        return -1;
      };
      int hi = hex(Text[Pos]), lo = hex(Text[Pos + 1]);
      if (hi < 0 || lo < 0)
        return false;
      advance();
      advance();
      out.push_back(static_cast<char>(hi * 16 + lo));
      return true;
    }
    default:
      return false;
    }
  }

  bool lexString(std::string &out) {
    advance(); // opening quote
    while (Pos < Text.size()) {
      char c = Text[Pos];
      if (c == '"') {
        advance();
        return true;
      }
      if (c == '\n')
        return false;
      advance();
      if (c == '\\') {
        if (!lexEscape(out))
          return false;
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool lexChar(std::string &out) {
    advance(); // opening quote
    if (Pos >= Text.size())
      return false;
    char c = Text[Pos];
    advance();
    if (c == '\\') {
      if (!lexEscape(out))
        return false;
    } else if (c == '\'' || c == '\n') {
      return false;
    } else {
      out.push_back(c);
    }
    if (Pos >= Text.size() || Text[Pos] != '\'')
      return false;
    advance();
    return out.size() == 1;
  }

  Token lexNumber(Token t) {
    bool isFloat = false;
    if (Text[Pos] == '-') {
      t.Text.push_back('-');
      advance();
    }
    while (Pos < Text.size()) {
      char c = Text[Pos];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        t.Text.push_back(c);
      } else if (c == '.' && Pos + 1 < Text.size() &&
                 std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
        isFloat = true;
        t.Text.push_back(c);
      } else if ((c == 'e' || c == 'E') && !t.Text.empty()) {
        isFloat = true;
        t.Text.push_back(c);
        if (Pos + 1 < Text.size() &&
            (Text[Pos + 1] == '+' || Text[Pos + 1] == '-')) {
          advance();
          t.Text.push_back(Text[Pos]);
        }
      } else {
        break;
      }
      advance();
    }
    if (!isFloat && Pos < Text.size() && Text[Pos] == '.') {
      // `1.` is not a float in this format; `inf`/`nan` are words.
      return fail(t, "malformed number");
    }
    t.Kind = isFloat ? Tok::Float : Tok::Int;
    return t;
  }
};

// -- Parser

bool parseTypeName(std::string_view s, Type &out) {
  return typeFromName(s, out);
}

bool parseOpcodeName(std::string_view s, Opcode &out) {
  return opcodeFromName(s, out);
}

bool parsePredName(std::string_view s, CmpPred &out) {
  static const std::pair<const char *, CmpPred> kPreds[] = {
      {"eq", CmpPred::Eq}, {"ne", CmpPred::Ne}, {"lt", CmpPred::Lt},
      {"le", CmpPred::Le}, {"gt", CmpPred::Gt}, {"ge", CmpPred::Ge},
  };
  for (const auto &[name, p] : kPreds)
    if (s == name) {
      out = p;
      return true;
    }
  return false;
}

class Parser {
public:
  Parser(std::string_view text, ParseError &err)
      : TheLexer(text, err), Err(err) {
    Cur = TheLexer.next();
  }

  bool parseProgram(Program &out) {
    while (Cur.Kind != Tok::End) {
      Module m;
      if (!parseModule(m))
        return false;
      out.Modules.push_back(std::move(m));
    }
    if (!Err.Message.empty())
      return false;
    if (out.Modules.empty())
      return error("expected 'module'");
    return true;
  }

  bool parseOneModule(Module &out) {
    if (!parseModule(out))
      return false;
    if (Cur.Kind != Tok::End)
      return error("expected end of input after the module");
    return true;
  }

private:
  Lexer TheLexer;
  ParseError &Err;
  Token Cur;
  unsigned PrevLine = 0; // line of the token before Cur

  // Per-function state.
  Function *CurFn = nullptr;
  std::unordered_map<std::string, ValueId> ValueNames;
  std::unordered_map<std::string, LocalId> LocalNames;

  bool error(std::string msg) {
    if (Err.Message.empty()) {
      Err.Line = Cur.Line;
      Err.Column = Cur.Column;
      Err.Message = std::move(msg);
    }
    return false;
  }

  void advance() {
    PrevLine = Cur.Line;
    Cur = TheLexer.next();
  }

  bool isWord(const char *w) const {
    return Cur.Kind == Tok::Word && Cur.Text == w;
  }

  bool expect(Tok k, const char *what) {
    if (Cur.Kind != k)
      return error(std::string("expected ") + what);
    advance();
    return true;
  }

  bool expectWord(const char *w) {
    if (!isWord(w))
      return error(std::string("expected '") + w + "'");
    advance();
    return true;
  }

  /// A bare or quoted name (class, field, vtable slot).
  bool parseName(std::string &out, const char *what) {
    if (Cur.Kind == Tok::Word || Cur.Kind == Tok::String) {
      out = Cur.Text;
      advance();
      return true;
    }
    return error(std::string("expected ") + what);
  }

  bool parseSymbol(std::string &out) {
    if (Cur.Kind != Tok::Symbol)
      return error("expected '@symbol'");
    out = Cur.Text;
    advance();
    return true;
  }

  bool parseString(std::string &out, const char *what) {
    if (Cur.Kind != Tok::String)
      return error(std::string("expected ") + what);
    out = Cur.Text;
    advance();
    return true;
  }

  bool parseType(Type &out) {
    if (Cur.Kind == Tok::Word && parseTypeName(Cur.Text, out)) {
      advance();
      return true;
    }
    return error("expected a type");
  }

  bool parseInt(int64_t &out) {
    if (Cur.Kind != Tok::Int)
      return error("expected an integer");
    errno = 0;
    char *end = nullptr;
    long long v = std::strtoll(Cur.Text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end)
      return error("integer out of range");
    out = v;
    advance();
    return true;
  }

  /// `(T, T) -> R`
  bool parseSignature(Signature &sig) {
    if (!expect(Tok::LParen, "'('"))
      return false;
    while (Cur.Kind != Tok::RParen) {
      Type t;
      if (!parseType(t))
        return false;
      sig.Params.push_back(t);
      if (Cur.Kind == Tok::Comma)
        advance();
      else if (Cur.Kind != Tok::RParen)
        return error("expected ',' or ')'");
    }
    advance();
    if (!expect(Tok::Arrow, "'->'"))
      return false;
    return parseType(sig.Ret);
  }

  // -- Module items

  bool parseModule(Module &m) {
    if (!expectWord("module"))
      return false;
    if (!parseString(m.Name, "the module name (a string)"))
      return false;
    while (Cur.Kind != Tok::End && !isWord("module")) {
      if (isWord("cstr")) {
        advance();
        CStrGlobal g;
        if (!parseSymbol(g.Name) || !expect(Tok::Equals, "'='") ||
            !parseString(g.Data, "a string"))
          return false;
        if (isWord("len")) {
          advance();
          int64_t len;
          if (!parseInt(len))
            return false;
          if (len != static_cast<int64_t>(g.Data.size()))
            return error("cstr length does not match the literal");
        }
        m.CStrs.push_back(std::move(g));
      } else if (isWord("data")) {
        advance();
        DataGlobal g;
        if (!parseSymbol(g.Name) || !expect(Tok::Equals, "'='") ||
            !expect(Tok::LBracket, "'['"))
          return false;
        while (Cur.Kind != Tok::RBracket) {
          int64_t v;
          if (!parseInt(v))
            return false;
          g.Words.push_back(v);
          if (Cur.Kind == Tok::Comma)
            advance();
          else if (Cur.Kind != Tok::RBracket)
            return error("expected ',' or ']'");
        }
        advance();
        m.Datas.push_back(std::move(g));
      } else if (isWord("bytes")) {
        advance();
        BytesGlobal g;
        if (!parseSymbol(g.Name) || !expect(Tok::Equals, "'='") ||
            !expect(Tok::LBracket, "'['"))
          return false;
        while (Cur.Kind != Tok::RBracket) {
          int64_t v;
          if (!parseInt(v))
            return false;
          if (v < 0 || v > 255)
            return error("byte out of range");
          g.Bytes.push_back(static_cast<uint8_t>(v));
          if (Cur.Kind == Tok::Comma)
            advance();
          else if (Cur.Kind != Tok::RBracket)
            return error("expected ',' or ']'");
        }
        advance();
        m.Bytes.push_back(std::move(g));
      } else if (isWord("extern")) {
        advance();
        if (isWord("obj") || isWord("vtable")) {
          ExternGlobal g;
          g.K = isWord("obj") ? ExternGlobal::Object : ExternGlobal::VTable;
          advance();
          if (!parseSymbol(g.Name))
            return false;
          m.Externs.push_back(std::move(g));
        } else if (isWord("fn")) {
          advance();
          Function f;
          f.IsExtern = true;
          if (!parseSymbol(f.Name) || !parseSignature(f.Sig))
            return false;
          // The optional `module "<name>"` clause is on the declaration's
          // line; a `module` on a later line is the next module's header.
          if (isWord("module") && Cur.Line == PrevLine) {
            advance();
            if (!parseString(f.Module, "a module name"))
              return false;
          }
          // `symbol @name`: the defining module's name for it (the verifier
          // rejects it on a runtime extern).
          if (isWord("symbol") && Cur.Line == PrevLine) {
            advance();
            if (!parseSymbol(f.Symbol))
              return false;
          }
          m.Functions.push_back(std::move(f));
        } else if (isWord("class")) {
          advance();
          Class c;
          c.IsExtern = true;
          if (!parseClass(c))
            return false;
          m.Classes.push_back(std::move(c));
        } else {
          return error("expected 'obj', 'vtable', 'fn' or 'class' after "
                       "'extern'");
        }
      } else if (isWord("class")) {
        advance();
        Class c;
        if (!parseClass(c))
          return false;
        m.Classes.push_back(std::move(c));
      } else if (isWord("fn")) {
        advance();
        Function f;
        if (!parseFunction(f))
          return false;
        m.Functions.push_back(std::move(f));
      } else {
        return error("expected a module item ('cstr', 'data', 'bytes', "
                     "'extern', 'class' or 'fn')");
      }
    }
    return resolveFixups(m);
  }

  bool parseClass(Class &c) {
    if (!parseName(c.Name, "a class name"))
      return false;
    if (Cur.Kind == Tok::Colon) {
      advance();
      if (!parseName(c.Super, "the superclass name"))
        return false;
    }
    if (isWord("module")) {
      advance();
      if (!parseString(c.Module, "a module name"))
        return false;
    }
    if (!expect(Tok::LBrace, "'{'"))
      return false;
    while (Cur.Kind != Tok::RBrace) {
      if (isWord("field")) {
        advance();
        Field f;
        if (!parseName(f.Name, "a field name") || !expect(Tok::Colon, "':'") ||
            !parseType(f.Ty))
          return false;
        c.Fields.push_back(std::move(f));
      } else if (isWord("vtable")) {
        advance();
        if (!expect(Tok::LBrace, "'{'"))
          return false;
        while (Cur.Kind != Tok::RBrace) {
          VTableEntry e;
          if (!parseName(e.Slot, "a slot name") || !expect(Tok::Equals, "'='"))
            return false;
          if (isWord("null")) {
            advance();
          } else if (!parseSymbol(e.Target)) {
            return false;
          }
          if (!expect(Tok::Colon, "':'") || !parseSignature(e.Sig))
            return false;
          c.VTable.push_back(std::move(e));
        }
        advance();
      } else {
        return error("expected 'field', 'vtable' or '}'");
      }
    }
    advance();
    return true;
  }

  // -- Functions

  /// `%name.N` -> (name, N); `%N` -> ("", N); `%name` -> (name, 0 = assign).
  static void splitValueRef(const std::string &text, std::string &name,
                            ValueId &id) {
    size_t dot = text.rfind('.');
    auto allDigits = [](std::string_view s) {
      if (s.empty())
        return false;
      for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c)))
          return false;
      return true;
    };
    if (allDigits(text)) {
      name.clear();
      id = static_cast<ValueId>(std::strtoul(text.c_str(), nullptr, 10));
    } else if (dot != std::string::npos && dot + 1 < text.size() &&
               allDigits(std::string_view(text).substr(dot + 1))) {
      name = text.substr(0, dot);
      id = static_cast<ValueId>(
          std::strtoul(text.c_str() + dot + 1, nullptr, 10));
    } else {
      name = text;
      id = kNoValue;
    }
  }

  /// Define a value from a `%ref` token.
  bool defineValue(const Token &t, Type ty, Value &out) {
    std::string name;
    ValueId id;
    splitValueRef(t.Text, name, id);
    if (id == kNoValue)
      id = CurFn->NextValueId;
    if (ValueNames.count(t.Text))
      return error("value '%" + t.Text + "' defined twice");
    out.Id = id;
    out.Ty = ty;
    out.Name = name;
    ValueNames[t.Text] = id;
    if (!name.empty())
      ValueNames[name] = id; // a bare `%name` use refers to it too
    if (id >= CurFn->NextValueId)
      CurFn->NextValueId = id + 1;
    return true;
  }

  bool parseFunction(Function &f) {
    CurFn = &f;
    ValueNames.clear();
    LocalNames.clear();
    if (!parseSymbol(f.Name) || !expect(Tok::LParen, "'('"))
      return false;
    while (Cur.Kind != Tok::RParen) {
      if (Cur.Kind != Tok::ValueRef)
        return error("expected a '%parameter'");
      Token t = Cur;
      advance();
      Type ty;
      if (!expect(Tok::Colon, "':'") || !parseType(ty))
        return false;
      Value v;
      if (!defineValue(t, ty, v))
        return false;
      f.Params.push_back(v);
      f.Sig.Params.push_back(ty);
      if (Cur.Kind == Tok::Comma)
        advance();
      else if (Cur.Kind != Tok::RParen)
        return error("expected ',' or ')'");
    }
    advance();
    if (!expect(Tok::Arrow, "'->'") || !parseType(f.Sig.Ret) ||
        !expect(Tok::LBrace, "'{'"))
      return false;
    while (isWord("local")) {
      advance();
      if (Cur.Kind != Tok::ValueRef)
        return error("expected a '%local' name");
      // Written `%name.index` by the printer, `%name` by hand; references
      // use the same spelling as the declaration.  The stored name drops an
      // index suffix that matches the slot's position.
      std::string written = Cur.Text;
      advance();
      Local l;
      ValueId idx;
      splitValueRef(written, l.Name, idx);
      if (idx != static_cast<ValueId>(f.Locals.size()) ||
          written == std::to_string(idx))
        l.Name = written;
      if (!expect(Tok::Colon, "':'") || !parseType(l.Ty))
        return false;
      if (LocalNames.count(written))
        return error("local '%" + written + "' declared twice");
      LocalNames[written] = static_cast<LocalId>(f.Locals.size());
      f.Locals.push_back(std::move(l));
    }
    if (!parseBlockBody(f.Body))
      return false;
    if (!expect(Tok::RBrace, "'}'"))
      return false;
    CurFn = nullptr;
    return true;
  }

  /// Statements up to the closing '}' (not consumed).
  bool parseBlockBody(Block &b) {
    while (Cur.Kind != Tok::RBrace) {
      if (Cur.Kind == Tok::End)
        return error("unexpected end of input inside a block");
      Stmt s;
      if (!parseStmt(s))
        return false;
      b.Stmts.push_back(std::move(s));
    }
    return true;
  }

  bool parseBracedBlock(std::unique_ptr<Block> &out) {
    if (!expect(Tok::LBrace, "'{'"))
      return false;
    out = std::make_unique<Block>();
    if (!parseBlockBody(*out))
      return false;
    return expect(Tok::RBrace, "'}'");
  }

  bool parseOperand(Operand &out) {
    switch (Cur.Kind) {
    case Tok::ValueRef: {
      auto it = ValueNames.find(Cur.Text);
      if (it == ValueNames.end()) {
        std::string name;
        ValueId id;
        splitValueRef(Cur.Text, name, id);
        if (id == kNoValue || ValueNames.count(name) == 0)
          return error("use of undefined value '%" + Cur.Text + "'");
        out = Operand::value(id);
      } else {
        out = Operand::value(it->second);
      }
      advance();
      return true;
    }
    case Tok::Int: {
      int64_t v;
      if (!parseInt(v))
        return false;
      out = Operand::i64(v);
      return true;
    }
    case Tok::Float: {
      errno = 0;
      double d = std::strtod(Cur.Text.c_str(), nullptr);
      // ERANGE also flags a subnormal result, which is representable (and
      // printed by the printer); only overflow to ±inf and a nonzero value
      // that underflows to 0 are out of range.
      if (errno == ERANGE && (std::isinf(d) || d == 0.0))
        return error("float out of range");
      out = Operand::f64(d);
      advance();
      return true;
    }
    case Tok::Char:
      out = Operand::chr(Cur.Text[0]);
      advance();
      return true;
    case Tok::Symbol:
      out = Operand::symbol(Cur.Text);
      advance();
      return true;
    case Tok::Word:
      if (Cur.Text == "true" || Cur.Text == "false") {
        out = Operand::boolean(Cur.Text == "true");
        advance();
        return true;
      }
      if (Cur.Text == "null") {
        advance();
        Type t = Type::Box;
        if (Cur.Kind == Tok::Word && parseTypeName(Cur.Text, t)) {
          if (t != Type::Box && t != Type::Obj)
            return error("null must be 'box' or 'obj'");
          advance();
        }
        out = Operand::null(t);
        return true;
      }
      return error("expected an operand, found '" + Cur.Text + "'");
    default:
      return error("expected an operand");
    }
  }

  bool parseOperandList(std::vector<Operand> &args, Tok close) {
    while (Cur.Kind != close) {
      Operand op;
      if (!parseOperand(op))
        return false;
      args.push_back(op);
      if (Cur.Kind == Tok::Comma)
        advance();
      else if (Cur.Kind != close)
        return error("expected ','");
    }
    advance();
    return true;
  }

  /// `(a, b)` argument list.
  bool parseArgs(std::vector<Operand> &args) {
    if (!expect(Tok::LParen, "'('"))
      return false;
    return parseOperandList(args, Tok::RParen);
  }

  /// `a, b, c` to the end of the instruction (no closing token: operands are
  /// read while they are followed by commas).
  bool parseCommaOperands(std::vector<Operand> &args, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      if (i && !expect(Tok::Comma, "','"))
        return false;
      Operand op;
      if (!parseOperand(op))
        return false;
      args.push_back(op);
    }
    return true;
  }

  bool parseLocalRef(LocalId &out) {
    if (Cur.Kind != Tok::ValueRef)
      return error("expected a '%local'");
    auto it = LocalNames.find(Cur.Text);
    if (it == LocalNames.end())
      return error("use of undeclared local '%" + Cur.Text + "'");
    out = it->second;
    advance();
    return true;
  }

  /// `Class.field`.  A bare class name and the field scan as one word
  /// (`Point.x`, split at the last '.'); a quoted class name is followed by
  /// a '.' token and the field.
  bool parseFieldRef(Instr &in) {
    if (Cur.Kind == Tok::Word) {
      size_t dot = Cur.Text.rfind('.');
      if (dot == std::string::npos || dot == 0 || dot + 1 == Cur.Text.size())
        return error("expected 'Class.field'");
      in.ClassName = Cur.Text.substr(0, dot);
      in.Field = Cur.Text.substr(dot + 1);
      advance();
      return true;
    }
    if (!parseName(in.ClassName, "'Class.field'") || !expect(Tok::Dot, "'.'") ||
        !parseName(in.Field, "a field name"))
      return false;
    return true;
  }

  bool parseStmt(Stmt &out) {
    if (isWord("if")) {
      advance();
      If i;
      if (!parseOperand(i.Cond) || !parseBracedBlock(i.Then))
        return false;
      if (isWord("else")) {
        advance();
        if (!parseBracedBlock(i.Else))
          return false;
      }
      out = std::move(i);
      return true;
    }
    if (isWord("while")) {
      advance();
      While w;
      if (!expect(Tok::LBrace, "'{'"))
        return false;
      w.CondBlock = std::make_unique<Block>();
      // Statements, then `cond %c` closes the condition region.
      while (!isWord("cond")) {
        if (Cur.Kind == Tok::RBrace || Cur.Kind == Tok::End)
          return error("expected 'cond' at the end of the while condition");
        Stmt s;
        if (!parseStmt(s))
          return false;
        w.CondBlock->Stmts.push_back(std::move(s));
      }
      advance();
      if (!parseOperand(w.Cond) || !expect(Tok::RBrace, "'}'") ||
          !parseBracedBlock(w.Body))
        return false;
      out = std::move(w);
      return true;
    }
    if (isWord("break")) {
      advance();
      out = Break{};
      return true;
    }
    if (isWord("continue")) {
      advance();
      out = Continue{};
      return true;
    }
    if (isWord("unreachable")) {
      advance();
      out = Unreachable{};
      return true;
    }
    if (isWord("ret")) {
      advance();
      Return r;
      // `ret` alone: the next token starts a statement or closes the block.
      if (Cur.Kind != Tok::RBrace && !startsStmt()) {
        Operand v;
        if (!parseOperand(v))
          return false;
        r.Value = v;
      }
      out = std::move(r);
      return true;
    }
    Instr in;
    if (!parseInstr(in))
      return false;
    out = std::move(in);
    return true;
  }

  /// True when the current token can only begin a statement, never an
  /// operand (used to decide whether `ret` carries a value).
  bool startsStmt() const {
    if (Cur.Kind == Tok::ValueRef) {
      // `%r = ...` starts an instruction; a lone `%v` is an operand.  Peek is
      // not available, so treat a defined value as an operand and anything
      // else as a definition.
      return ValueNames.count(Cur.Text) == 0;
    }
    if (Cur.Kind != Tok::Word)
      return false;
    static const char *const kStmtWords[] = {
        "if",          "while", "break",       "continue", "ret",
        "unreachable", "cond",  "call",        "vcall",    "retain",
        "release",     "free",  "field.store", "store",    "ptr.store"};
    for (const char *w : kStmtWords)
      if (Cur.Text == w)
        return true;
    return false;
  }

  bool parseInstr(Instr &in) {
    Token result;
    bool hasResult = false;
    if (Cur.Kind == Tok::ValueRef) {
      result = Cur;
      hasResult = true;
      advance();
      if (!expect(Tok::Equals, "'=' after the result name"))
        return false;
    }
    if (Cur.Kind != Tok::Word || !parseOpcodeName(Cur.Text, in.Op))
      return error("expected an instruction");
    advance();

    // The result type, when it is known from the instruction itself.
    Type resultTy = Type::Void;
    bool needsFixup = false;
    switch (in.Op) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
      if (!parseCommaOperands(in.Args, 2))
        return false;
      needsFixup = true; // the type of the operands
      break;
    case Opcode::Neg:
    case Opcode::Not:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      needsFixup = true;
      break;
    case Opcode::Cmp:
      if (Cur.Kind != Tok::Word || !parsePredName(Cur.Text, in.Pred))
        return error("expected a comparison predicate (eq ne lt le gt ge)");
      advance();
      if (!parseCommaOperands(in.Args, 2))
        return false;
      resultTy = Type::Bool;
      break;
    case Opcode::Select:
      if (!parseCommaOperands(in.Args, 3))
        return false;
      needsFixup = true;
      break;
    case Opcode::IToF:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      resultTy = Type::F64;
      break;
    case Opcode::FToI:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      resultTy = Type::I64;
      break;
    case Opcode::Cast:
      if (!parseCommaOperands(in.Args, 1) || !expectWord("to") ||
          !parseType(in.CastTo))
        return false;
      resultTy = in.CastTo;
      break;
    case Opcode::Call:
      if (!parseSymbol(in.Callee) || !parseArgs(in.Args))
        return false;
      needsFixup = true;
      break;
    case Opcode::VCall: {
      Operand recv;
      if (!parseOperand(recv) || !expect(Tok::Colon, "':'"))
        return false;
      in.Args.push_back(recv);
      if (Cur.Kind == Tok::LParen) {
        if (!parseSignature(in.Sig))
          return false;
      } else if (!parseName(in.ClassName, "a class name or a signature")) {
        return false;
      }
      int64_t slot;
      if (!expect(Tok::LBracket, "'['") || !parseInt(slot) ||
          !expect(Tok::RBracket, "']'"))
        return false;
      if (slot < 0)
        return error("negative vtable slot");
      in.Slot = static_cast<uint32_t>(slot);
      if (!parseArgs(in.Args))
        return false;
      needsFixup = true;
      break;
    }
    case Opcode::Retain:
    case Opcode::Release:
    case Opcode::Free:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      break;
    case Opcode::Box:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      resultTy = Type::Box;
      break;
    case Opcode::Unbox:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      resultTy = Type::Obj;
      break;
    case Opcode::New:
      if (!parseName(in.ClassName, "a class name"))
        return false;
      resultTy = Type::Obj;
      break;
    case Opcode::FieldLoad:
      if (!parseCommaOperands(in.Args, 1) || !expect(Tok::Comma, "','") ||
          !parseFieldRef(in))
        return false;
      needsFixup = true;
      break;
    case Opcode::FieldStore: {
      if (!parseCommaOperands(in.Args, 1) || !expect(Tok::Comma, "','") ||
          !parseFieldRef(in) || !expect(Tok::Comma, "','"))
        return false;
      Operand v;
      if (!parseOperand(v))
        return false;
      in.Args.push_back(v);
      break;
    }
    case Opcode::VTableLoad:
      if (!parseCommaOperands(in.Args, 1))
        return false;
      resultTy = Type::Ptr;
      break;
    case Opcode::VTableAddr:
      if (Cur.Kind == Tok::Symbol) {
        in.Args.push_back(Operand::symbol(Cur.Text));
        advance();
      } else if (!parseName(in.ClassName, "a class name or '@vtable'")) {
        return false;
      }
      resultTy = Type::Ptr;
      break;
    case Opcode::Load:
      if (!parseLocalRef(in.Local))
        return false;
      resultTy = CurFn->Locals[in.Local].Ty;
      break;
    case Opcode::Store: {
      if (!parseLocalRef(in.Local) || !expect(Tok::Comma, "','"))
        return false;
      Operand v;
      if (!parseOperand(v))
        return false;
      in.Args.push_back(v);
      break;
    }
    case Opcode::LocalAddr:
      if (!parseLocalRef(in.Local))
        return false;
      resultTy = Type::Ptr;
      break;
    case Opcode::FieldAddr:
      if (!parseCommaOperands(in.Args, 1) || !expect(Tok::Comma, "','") ||
          !parseFieldRef(in))
        return false;
      resultTy = Type::Ptr;
      break;
    case Opcode::PtrLoad:
      if (!parseType(resultTy) || !expect(Tok::Comma, "','") ||
          !parseCommaOperands(in.Args, 1))
        return false;
      if (resultTy == Type::Void)
        return error("ptr.load of void");
      break;
    case Opcode::PtrStore:
      if (!parseCommaOperands(in.Args, 2))
        return false;
      break;
    }

    if (hasResult) {
      // A result typed Void here is resolved by resolveFixups once the whole
      // module is known.
      if (!defineValue(result, needsFixup ? Type::Void : resultTy, in.Result))
        return false;
    } else if (resultTy != Type::Void) {
      return error("instruction produces a value but has no '%result ='");
    }
    return true;
  }

  // -- Deferred result types

  /// The type of an operand inside @p f (values are looked up in the
  /// function; constants carry their own type; a symbol is a `ptr`, except an
  /// extern object singleton of @p m, which is an `obj`).  Void when unknown.
  static Type operandType(const Module &m, const Function &f,
                          const Operand &op) {
    struct V {
      const Module &M;
      const Function &F;
      Type operator()(ValueId id) const {
        for (const Value &p : F.Params)
          if (p.Id == id)
            return p.Ty;
        return findIn(F.Body, id);
      }
      static Type findIn(const Block &b, ValueId id) {
        for (const Stmt &s : b.Stmts) {
          if (const auto *i = std::get_if<Instr>(&s)) {
            if (i->Result.Id == id)
              return i->Result.Ty;
          } else if (const auto *i2 = std::get_if<If>(&s)) {
            Type t = i2->Then ? findIn(*i2->Then, id) : Type::Void;
            if (t == Type::Void && i2->Else)
              t = findIn(*i2->Else, id);
            if (t != Type::Void)
              return t;
          } else if (const auto *w = std::get_if<While>(&s)) {
            Type t = w->CondBlock ? findIn(*w->CondBlock, id) : Type::Void;
            if (t == Type::Void && w->Body)
              t = findIn(*w->Body, id);
            if (t != Type::Void)
              return t;
          }
        }
        return Type::Void;
      }
      Type operator()(int64_t) const { return Type::I64; }
      Type operator()(double) const { return Type::F64; }
      Type operator()(bool) const { return Type::Bool; }
      Type operator()(char) const { return Type::Char; }
      Type operator()(const Operand::Null &n) const { return n.Ty; }
      Type operator()(const SymbolRef &s) const {
        for (const ExternGlobal &g : M.Externs)
          if (g.Name == s.Name)
            return g.K == ExternGlobal::Object ? Type::Obj : Type::Ptr;
        return Type::Ptr;
      }
    };
    return std::visit(V{m, f}, op.V);
  }

  /// Fill in the result types that depend on declarations elsewhere in the
  /// module (callee declarations, class vtables and fields may follow their
  /// use) or on earlier values.  Instructions are revisited in source order,
  /// so an arithmetic result that feeds a later one is typed first.  Anything
  /// still unknown is left Void for the verifier to report.
  bool resolveFixups(Module &m) {
    for (Function &f : m.Functions)
      if (!f.IsExtern)
        resolveBlock(m, f, f.Body);
    return true;
  }

  void resolveBlock(const Module &m, const Function &f, Block &b) {
    for (Stmt &s : b.Stmts) {
      if (auto *in = std::get_if<Instr>(&s)) {
        if (in->Result.Id != kNoValue && in->Result.Ty == Type::Void)
          resolveInstr(m, f, *in);
      } else if (auto *i = std::get_if<If>(&s)) {
        if (i->Then)
          resolveBlock(m, f, *i->Then);
        if (i->Else)
          resolveBlock(m, f, *i->Else);
      } else if (auto *w = std::get_if<While>(&s)) {
        if (w->CondBlock)
          resolveBlock(m, f, *w->CondBlock);
        if (w->Body)
          resolveBlock(m, f, *w->Body);
      }
    }
  }

  static void resolveInstr(const Module &m, const Function &f, Instr &in) {
    switch (in.Op) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::Neg:
    case Opcode::Not:
      in.Result.Ty =
          in.Args.empty() ? Type::Void : operandType(m, f, in.Args[0]);
      break;
    case Opcode::Select:
      in.Result.Ty =
          in.Args.size() < 3 ? Type::Void : operandType(m, f, in.Args[1]);
      break;
    case Opcode::Call:
      if (const Function *callee = m.findFunction(in.Callee))
        in.Result.Ty = callee->Sig.Ret;
      break;
    case Opcode::VCall:
      if (in.ClassName.empty()) {
        in.Result.Ty = in.Sig.Ret;
      } else if (const Class *c = m.findClass(in.ClassName)) {
        if (in.Slot < c->VTable.size()) {
          in.Sig = c->VTable[in.Slot].Sig;
          in.Result.Ty = in.Sig.Ret;
        }
      }
      break;
    case Opcode::FieldLoad:
      if (const Class *c = m.findClass(in.ClassName))
        for (const Field &fld : c->Fields)
          if (fld.Name == in.Field)
            in.Result.Ty = fld.Ty;
      break;
    default:
      break;
    }
  }
};

} // namespace

std::optional<Program> parseProgram(std::string_view text, ParseError &err) {
  err = ParseError();
  Program p;
  Parser parser(text, err);
  if (!parser.parseProgram(p))
    return std::nullopt;
  return p;
}

std::optional<Module> parseModule(std::string_view text, ParseError &err) {
  err = ParseError();
  Module m;
  Parser parser(text, err);
  if (!parser.parseOneModule(m))
    return std::nullopt;
  return m;
}

} // namespace paykan::pir
