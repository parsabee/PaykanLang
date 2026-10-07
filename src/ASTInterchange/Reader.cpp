// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The AST interchange format's reader (paykan/ast/Interchange.h,
// docs/plugins/ast-format.md).  Two steps: the text is read into a tree of
// lists and atoms (the S-expression syntax), then each node is built from
// its list, checking the tag, the number and the kind of its fields, and the
// shape rules the grammar gives the AST (a function body is a block, a tuple
// has two elements or more, ...).  Types are built the way the
// recursive-descent frontend builds them, so the same program gives the
// same AST through either path.
//
// The text comes from a plugin: nothing in it is trusted.  Every failure is
// a ReadError with a position, never an assertion; nesting is bounded
// (kMaxDepth), and so is every recursion here.

#include "paykan/Frontend.h"
#include "paykan/ast/Interchange.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace paykan::ast::interchange {

std::string ReadError::str() const {
  if (!Line)
    return Message;
  return std::to_string(Line) + ":" + std::to_string(Column) + ": " + Message;
}

namespace {

// -- Step 1: the S-expression syntax

/// A list or an atom of the text.
struct SExpr {
  enum Kind { List, Symbol, String, Number, Location };
  Kind K = Symbol;
  size_t Line = 0, Column = 0;
  /// The atom's text (a string's decoded bytes); a list's tag (its first
  /// element, which must be a symbol).
  std::string Text;
  /// A list's elements after the tag.
  std::vector<SExpr> Items;
};

class Lexer {
public:
  explicit Lexer(std::string_view text) : Text(text) {}

  /// The whole text as one list; false with Err set on a syntax error.
  bool top(SExpr &out) {
    skipSpace();
    if (Pos >= Text.size())
      return fail("the text is empty: expected (paykan-ast ...)");
    if (Text[Pos] != '(')
      return fail("expected (paykan-ast ...)");
    if (!list(out, 1))
      return false;
    skipSpace();
    if (Pos < Text.size())
      return fail("unexpected text after the (paykan-ast ...) form");
    return true;
  }

  ReadError Err;

private:
  std::string_view Text;
  size_t Pos = 0, Line = 1, Column = 1;

  bool fail(const std::string &msg) {
    Err.Line = Line;
    Err.Column = Column;
    Err.Message = msg;
    return false;
  }

  void advance() {
    if (Text[Pos] == '\n') {
      ++Line;
      Column = 1;
    } else {
      ++Column;
    }
    ++Pos;
  }

  /// Whitespace and `;` comments (to the end of the line).
  void skipSpace() {
    while (Pos < Text.size()) {
      char c = Text[Pos];
      if (c == ';') {
        while (Pos < Text.size() && Text[Pos] != '\n')
          advance();
      } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        advance();
      } else {
        return;
      }
    }
  }

  static bool delimiter(char c) {
    return c == '(' || c == ')' || c == '"' || c == ';' || c == ' ' ||
           c == '\t' || c == '\n' || c == '\r';
  }

  bool list(SExpr &out, unsigned depth) {
    if (depth > kMaxDepth)
      return fail("nesting too deep (more than " + std::to_string(kMaxDepth) +
                  " levels)");
    out.K = SExpr::List;
    out.Line = Line;
    out.Column = Column;
    advance(); // '('
    skipSpace();
    if (Pos >= Text.size() || delimiter(Text[Pos]))
      return fail("expected a tag after '('");
    SExpr tag;
    atom(tag);
    if (tag.K != SExpr::Symbol)
      return fail("expected a tag after '(', not '" + tag.Text + "'");
    out.Text = std::move(tag.Text);
    for (;;) {
      skipSpace();
      if (Pos >= Text.size())
        return fail("unterminated list (" + out.Text + " ...) from " +
                    std::to_string(out.Line) + ":" +
                    std::to_string(out.Column));
      char c = Text[Pos];
      if (c == ')') {
        advance();
        return true;
      }
      out.Items.emplace_back();
      SExpr &item = out.Items.back();
      if (c == '(') {
        if (!list(item, depth + 1))
          return false;
      } else if (c == '"') {
        if (!string(item))
          return false;
      } else {
        atom(item);
      }
    }
  }

  static int hexDigit(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  bool string(SExpr &out) {
    out.K = SExpr::String;
    out.Line = Line;
    out.Column = Column;
    advance(); // '"'
    for (;;) {
      if (Pos >= Text.size())
        return fail("unterminated string");
      char c = Text[Pos];
      if (c == '"') {
        advance();
        return true;
      }
      if (c == '\n')
        return fail("a newline in a string (write it as \\n)");
      if (c != '\\') {
        out.Text += c;
        advance();
        continue;
      }
      advance();
      if (Pos >= Text.size())
        return fail("unterminated string");
      char e = Text[Pos];
      switch (e) {
      case '"':
      case '\\':
        out.Text += e;
        break;
      case 'n':
        out.Text += '\n';
        break;
      case 't':
        out.Text += '\t';
        break;
      case 'r':
        out.Text += '\r';
        break;
      case 'x': {
        int hi = Pos + 1 < Text.size() ? hexDigit(Text[Pos + 1]) : -1;
        int lo = Pos + 2 < Text.size() ? hexDigit(Text[Pos + 2]) : -1;
        if (hi < 0 || lo < 0)
          return fail("\\x needs two hexadecimal digits");
        out.Text += static_cast<char>(hi * 16 + lo);
        advance();
        advance();
        break;
      }
      default:
        return fail(std::string("unknown escape \\") + e + " in a string");
      }
      advance();
    }
  }

  void atom(SExpr &out) {
    out.Line = Line;
    out.Column = Column;
    size_t start = Pos;
    while (Pos < Text.size() && !delimiter(Text[Pos]))
      advance();
    out.Text = std::string(Text.substr(start, Pos - start));
    char c = out.Text[0];
    if (c == '@')
      out.K = SExpr::Location;
    else if ((c >= '0' && c <= '9') ||
             (c == '-' && out.Text.size() > 1 && out.Text[1] >= '0' &&
              out.Text[1] <= '9'))
      out.K = SExpr::Number;
    else
      out.K = SExpr::Symbol;
  }
};

// -- Step 2: the AST

/// Thrown-free failure: the first error wins, and everything after it
/// unwinds by returning null.
class Builder {
public:
  Builder(ASTContext &ctx, ReadError &err) : Ctx(ctx), Err(err) {}

  TranslationUnit *top(const SExpr &form) {
    if (form.Text != "paykan-ast")
      return fail(form, "expected (paykan-ast <version> (unit ...)), not (" +
                            form.Text + " ...)"),
             nullptr;
    if (form.Items.size() != 2 || form.Items[0].K != SExpr::Number)
      return fail(form, "expected (paykan-ast <version> (unit ...))"), nullptr;
    int64_t version = 0;
    if (!integer(form.Items[0], version))
      return nullptr;
    if (version != kFormatVersion)
      return fail(form.Items[0], "unsupported AST format version " +
                                     form.Items[0].Text +
                                     " (this paykan reads version " +
                                     std::to_string(kFormatVersion) + ")"),
             nullptr;
    return unit(form.Items[1]);
  }

private:
  ASTContext &Ctx;
  ReadError &Err;

  bool failed() const { return !Err.Message.empty(); }

  void fail(const SExpr &at, const std::string &msg) {
    if (failed())
      return;
    Err.Line = at.Line;
    Err.Column = at.Column;
    Err.Message = msg;
  }

  const std::string &intern(const std::string &s) { return Ctx.intern(s); }

  /// The fields of one list, read left to right.
  class Fields {
  public:
    Fields(Builder &b, const SExpr &list) : B(b), L(list) {}

    /// The node's location, when its first field is one.
    SourceLocation location() {
      if (I < L.Items.size() && L.Items[I].K == SExpr::Location)
        return B.location(L.Items[I++]);
      return SourceLocation();
    }

    bool atEnd() const { return I >= L.Items.size(); }

    /// The next field, or null (with an error naming @p what) at the end.
    const SExpr *next(const char *what) {
      if (atEnd()) {
        B.fail(L, "(" + L.Text + " ...) is missing " + what);
        return nullptr;
      }
      return &L.Items[I++];
    }

    /// The next field if it is `_` (consumed), else false.
    bool absent() {
      if (!atEnd() && L.Items[I].K == SExpr::Symbol && L.Items[I].Text == "_") {
        ++I;
        return true;
      }
      return false;
    }

    const std::string *string(const char *what) {
      const SExpr *e = next(what);
      if (!e)
        return nullptr;
      if (e->K != SExpr::String) {
        B.fail(*e, std::string("expected a string: ") + what);
        return nullptr;
      }
      return &B.intern(e->Text);
    }

    /// A non-empty string.
    const std::string *name(const char *what) {
      const SExpr *at = atEnd() ? nullptr : &L.Items[I];
      const std::string *s = string(what);
      if (s && s->empty()) {
        B.fail(*at, std::string("empty name: ") + what);
        return nullptr;
      }
      return s;
    }

    const SExpr *symbol(const char *what) {
      const SExpr *e = next(what);
      if (e && e->K != SExpr::Symbol) {
        B.fail(*e, std::string("expected a symbol: ") + what);
        return nullptr;
      }
      return e;
    }

    bool boolean(const char *what, bool &out) {
      const SExpr *e = symbol(what);
      if (!e)
        return false;
      if (e->Text != "true" && e->Text != "false") {
        B.fail(*e, std::string("expected true or false: ") + what);
        return false;
      }
      out = e->Text == "true";
      return true;
    }

    bool integer(const char *what, int64_t &out) {
      const SExpr *e = next(what);
      if (!e)
        return false;
      if (e->K != SExpr::Number) {
        B.fail(*e, std::string("expected an integer: ") + what);
        return false;
      }
      return B.integer(*e, out);
    }

    /// The next field, which must be a list tagged @p tag.
    const SExpr *list(const char *tag) {
      const SExpr *e = next(tag);
      if (e && (e->K != SExpr::List || e->Text != tag)) {
        B.fail(*e, std::string("expected (") + tag + " ...)");
        return nullptr;
      }
      return e;
    }

    /// The next field, which must be a list (any tag).
    const SExpr *anyList(const char *what) {
      const SExpr *e = next(what);
      if (e && e->K != SExpr::List) {
        B.fail(*e, std::string("expected a list: ") + what);
        return nullptr;
      }
      return e;
    }

    /// An optional trailing `(<tag>)` item (the ownership prototype's
    /// `(let)`); true when it is there (consumed).
    bool flag(const char *tag) {
      if (atEnd() || L.Items[I].K != SExpr::List || L.Items[I].Text != tag)
        return false;
      if (!L.Items[I].Items.empty())
        B.fail(L.Items[I], std::string("(") + tag + ") takes no fields");
      ++I;
      return true;
    }

    /// An optional trailing `(<tag> view|mut|own)` item (a qualifier of the
    /// ownership prototype) into @p out; false only on a malformed one.
    bool qualifier(const char *tag, Qualifier &out, bool *present = nullptr) {
      if (atEnd() || L.Items[I].K != SExpr::List || L.Items[I].Text != tag)
        return true;
      const SExpr &q = L.Items[I++];
      if (present)
        *present = true;
      const SExpr *v = q.Items.size() == 1 ? &q.Items[0] : nullptr;
      for (Qualifier k : {Qualifier::View, Qualifier::Mut, Qualifier::Own})
        if (v && v->K == SExpr::Symbol && v->Text == qualifierName(k)) {
          out = k;
          return true;
        }
      B.fail(q, std::string("expected (") + tag + " view|mut|own)");
      return false;
    }

    /// Fail unless every field was read.
    bool done() {
      if (!atEnd()) {
        B.fail(L.Items[I], "unexpected field in (" + L.Text + " ...)");
        return false;
      }
      return !B.failed();
    }

    /// The remaining fields.
    std::vector<const SExpr *> rest() {
      std::vector<const SExpr *> out;
      while (!atEnd())
        out.push_back(&L.Items[I++]);
      return out;
    }

  private:
    Builder &B;
    const SExpr &L;
    size_t I = 0;
  };

  /// Parses all of @p text, a decimal floating-point number (the writer's
  /// "%.17g"), into @p out.  std::strtod rather than std::from_chars: Apple's
  /// libc++ (Xcode 15) has no floating-point from_chars.  paykan never sets a
  /// locale, so strtod reads '.' as the writer wrote it.  As the lexer does,
  /// a value that overflows to +-inf, or a nonzero one that underflows to 0,
  /// is rejected; a subnormal one (also ERANGE) is not.  Hexadecimal floats,
  /// which strtod would take, are not the writer's and are rejected too.
  static bool parseDouble(const std::string &text, double &out) {
    if (text.empty() || text.find_first_of("xX") != std::string::npos)
      return false;
    char *end = nullptr;
    errno = 0;
    double d = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size())
      return false;
    if (errno == ERANGE && (std::isinf(d) || d == 0.0))
      return false;
    out = d;
    return true;
  }

  bool integer(const SExpr &e, int64_t &out) {
    const char *first = e.Text.data(), *last = first + e.Text.size();
    auto [p, ec] = std::from_chars(first, last, out);
    if (ec != std::errc() || p != last) {
      fail(e, "not a 64-bit integer: " + e.Text);
      return false;
    }
    return true;
  }

  /// @L:C-L:C
  SourceLocation location(const SExpr &e) {
    size_t v[4] = {0, 0, 0, 0};
    const char *p = e.Text.data() + 1, *end = e.Text.data() + e.Text.size();
    const char seps[4] = {':', '-', ':', '\0'};
    for (int i = 0; i < 4; ++i) {
      auto [q, ec] = std::from_chars(p, end, v[i]);
      if (ec != std::errc() ||
          (i < 3 ? (q == end || *q != seps[i]) : q != end)) {
        fail(e, "malformed location '" + e.Text +
                    "' (expected @line:column-line:column)");
        return SourceLocation();
      }
      p = q + (i < 3 ? 1 : 0);
    }
    return SourceLocation(v[0], v[1], v[2], v[3]);
  }

  // -- Declarations

  TranslationUnit *unit(const SExpr &e) {
    if (e.K != SExpr::List || e.Text != "unit")
      return fail(e, "expected (unit ...)"), nullptr;
    Fields f(*this, e);
    SourceLocation loc = f.location();
    std::vector<ImportDecl *> imports;
    std::vector<ClassDecl *> classes, genericClasses;
    std::vector<FuncDecl *> funcs, genericFuncs;
    std::vector<EnumDecl *> enums;
    for (const SExpr *d : f.rest()) {
      if (d->K != SExpr::List) {
        fail(*d, "expected a declaration: (import ...), (enum ...), "
                 "(class ...) or (fn ...)");
        return nullptr;
      }
      if (d->Text == "import") {
        if (auto *i = importDecl(*d))
          imports.push_back(i);
      } else if (d->Text == "enum") {
        if (auto *en = enumDecl(*d))
          enums.push_back(en);
      } else if (d->Text == "class") {
        if (auto *c = classDecl(*d))
          (c->isGeneric() ? genericClasses : classes).push_back(c);
      } else if (d->Text == "fn") {
        if (auto *fn = funcDecl(*d))
          (fn->isGeneric() ? genericFuncs : funcs).push_back(fn);
      } else {
        fail(*d, "(" + d->Text +
                     " ...) is not a top-level declaration (import, enum, "
                     "class, fn)");
      }
      if (failed())
        return nullptr;
    }
    return Ctx.make<TranslationUnit>(
        loc, std::move(imports), std::move(classes), std::move(funcs),
        std::move(enums), std::move(genericClasses), std::move(genericFuncs));
  }

  ImportDecl *importDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    bool system = false;
    if (!f.boolean("whether the import is a system import (::name)", system))
      return nullptr;
    const std::string *base = f.string("the base path");
    if (!base)
      return nullptr;
    std::vector<ImportDecl::Module> modules;
    for (const SExpr *m : f.rest()) {
      if (m->K != SExpr::List || m->Text != "module")
        return fail(*m, "expected (module \"name\" \"alias\")"), nullptr;
      Fields mf(*this, *m);
      const std::string *name = mf.name("the module's name");
      const std::string *alias = name ? mf.string("the alias") : nullptr;
      if (!alias || !mf.done())
        return nullptr;
      modules.push_back({name, alias});
    }
    if (modules.empty())
      return fail(e, "(import ...) names no module"), nullptr;
    return Ctx.make<ImportDecl>(loc, *base, system, std::move(modules));
  }

  EnumDecl *enumDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the enum's name");
    if (!name)
      return nullptr;
    std::vector<const std::string *> variants;
    while (!f.atEnd()) {
      const std::string *v = f.name("a variant");
      if (!v)
        return nullptr;
      variants.push_back(v);
    }
    return Ctx.make<EnumDecl>(loc, *name, std::move(variants));
  }

  bool typeParams(Fields &f, std::vector<const std::string *> &out) {
    const SExpr *l = f.list("type-params");
    if (!l)
      return false;
    Fields pf(*this, *l);
    while (!pf.atEnd()) {
      const std::string *p = pf.name("a type parameter");
      if (!p)
        return false;
      out.push_back(p);
    }
    return true;
  }

  ClassDecl *classDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the class's name");
    const std::string *super = name ? f.string("the superclass") : nullptr;
    std::vector<const std::string *> params;
    if (!super || !typeParams(f, params))
      return nullptr;
    std::vector<VarDecl *> fields;
    std::vector<FuncDecl *> methods;
    const SExpr *fl = f.list("fields");
    if (!fl)
      return nullptr;
    for (const SExpr &v : fl->Items) {
      if (v.K != SExpr::List || v.Text != "var")
        return fail(v, "expected a field: (var ...)"), nullptr;
      auto *vd = varDecl(v);
      if (!vd)
        return nullptr;
      if (vd->getInitExpr() || !vd->getType())
        return fail(v, "a field has a type and no initialiser"), nullptr;
      fields.push_back(vd);
    }
    const SExpr *ml = f.list("methods");
    if (!ml)
      return nullptr;
    for (const SExpr &m : ml->Items) {
      if (m.K != SExpr::List || m.Text != "fn")
        return fail(m, "expected a method: (fn ...)"), nullptr;
      auto *fn = funcDecl(m);
      if (!fn)
        return nullptr;
      methods.push_back(fn);
    }
    if (!f.done())
      return nullptr;
    return Ctx.make<ClassDecl>(loc, *name, *super, std::move(fields),
                               std::move(methods), std::move(params));
  }

  FuncDecl *funcDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the function's name");
    std::vector<const std::string *> tparams;
    if (!name || !typeParams(f, tparams))
      return nullptr;
    const SExpr *pl = f.list("params");
    if (!pl)
      return nullptr;
    std::vector<Param> params;
    for (const SExpr &p : pl->Items) {
      if (p.K != SExpr::List || p.Text != "param")
        return fail(p, "expected (param \"name\" TYPE)"), nullptr;
      Fields pf(*this, p);
      const std::string *pname = pf.name("the parameter's name");
      Type *pty = pname ? type(pf, "the parameter's type") : nullptr;
      Qualifier pq = Qualifier::View;
      if (!pty || !pf.qualifier("qual", pq) || !pf.done())
        return nullptr;
      params.push_back({pname, pty, pq});
    }
    Type *ret = nullptr;
    if (!f.absent()) {
      ret = type(f, "the return type or _");
      if (!ret)
        return nullptr;
    }
    CompoundStmt *body = block(f, "the function's body");
    Qualifier rq = Qualifier::View, sq = Qualifier::View;
    bool explicitSelf = false;
    if (!body || !f.qualifier("qual", rq) ||
        !f.qualifier("self", sq, &explicitSelf) || !f.done())
      return nullptr;
    auto *fn = Ctx.make<FuncDecl>(loc, *name, std::move(params), ret, body,
                                  std::move(tparams));
    fn->setResultQualifier(rq);
    if (explicitSelf)
      fn->setExplicitSelf(sq);
    return fn;
  }

  VarDecl *varDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the variable's name");
    if (!name)
      return nullptr;
    Type *ty = nullptr;
    if (!f.absent()) {
      ty = type(f, "the variable's type or _");
      if (!ty)
        return nullptr;
    }
    Expr *init = nullptr;
    if (!f.absent()) {
      init = expr(f, "the initialiser or _");
      if (!init)
        return nullptr;
    }
    Qualifier q = Qualifier::View;
    if (!f.qualifier("qual", q))
      return nullptr;
    bool let = f.flag("let");
    if (!f.done())
      return nullptr;
    auto *vd = Ctx.make<VarDecl>(loc, *name, ty, init);
    vd->setQualifier(q);
    vd->setLet(let);
    return vd;
  }

  // -- Types

  Type *type(Fields &f, const char *what) {
    const SExpr *e = f.anyList(what);
    return e ? type(*e) : nullptr;
  }

  Type *type(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    if (e.Text == "named-type") {
      const std::string *name = f.name("the type's name");
      if (!name || !f.done())
        return nullptr;
      // As the recursive-descent frontend does: a builtin (int, ...) or a
      // bootstrap class (Str, Obj, ...) is the canonical type; any other
      // name, and every qualified one (module::Type), is a class type stub
      // that Sema resolves.
      if (name->find("::") == std::string::npos)
        if (Type *ty = Ctx.lookupType(*name))
          return ty;
      return Ctx.make<ClassType>(loc, *name, nullptr);
    }
    if (e.Text == "array-type") {
      Type *elem = type(f, "the element type");
      if (!elem || !f.done())
        return nullptr;
      return Ctx.make<ArrayType>(loc, elem);
    }
    if (e.Text == "optional-type") {
      Type *inner = type(f, "the inner type");
      if (!inner || !f.done())
        return nullptr;
      // The checks the grammar makes (docs/grammar.md).
      auto *bt = dyn_cast<BuiltinType>(inner);
      if (bt && bt->getTypeKind() == BuiltinType::Void)
        return fail(e, "optional type 'void?' is not supported"), nullptr;
      if (isa<OptionalType>(inner))
        return fail(e, "nested optional type '" + typeName(inner) +
                           "?' is not supported"),
               nullptr;
      return Ctx.make<OptionalType>(loc, inner);
    }
    if (e.Text == "tuple-type") {
      std::vector<Type *> elems;
      while (!f.atEnd()) {
        Type *t = type(f, "an element type");
        if (!t)
          return nullptr;
        elems.push_back(t);
      }
      if (elems.size() < 2)
        return fail(e, "a tuple type needs at least two element types"),
               nullptr;
      return Ctx.make<TupleType>(loc, std::move(elems));
    }
    if (e.Text == "generic-type") {
      const std::string *name = f.name("the generic type's name");
      if (!name)
        return nullptr;
      std::vector<Type *> args;
      while (!f.atEnd()) {
        Type *t = type(f, "a type argument");
        if (!t)
          return nullptr;
        args.push_back(t);
      }
      if (args.empty())
        return fail(e, "a generic type needs a type argument"), nullptr;
      return Ctx.make<GenericType>(loc, *name, std::move(args));
    }
    return fail(e, "(" + e.Text +
                       " ...) is not a type (named-type, array-type, "
                       "optional-type, tuple-type, generic-type)"),
           nullptr;
  }

  // -- Statements

  CompoundStmt *block(Fields &f, const char *what) {
    const SExpr *e = f.anyList(what);
    if (!e)
      return nullptr;
    if (e->Text != "block")
      return fail(*e, std::string("expected (block ...): ") + what), nullptr;
    return cast_or_null<CompoundStmt>(stmt(*e));
  }

  template <typename T> static T *cast_or_null(Stmt *s) {
    return s ? cast<T>(s) : nullptr;
  }

  Stmt *stmt(Fields &f, const char *what) {
    const SExpr *e = f.anyList(what);
    return e ? stmt(*e) : nullptr;
  }

  Stmt *stmt(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string &t = e.Text;
    Stmt *out = nullptr;
    if (t == "block") {
      std::vector<Stmt *> stmts;
      while (!f.atEnd()) {
        Stmt *s = stmt(f, "a statement");
        if (!s)
          return nullptr;
        stmts.push_back(s);
      }
      out = Ctx.make<CompoundStmt>(loc, std::move(stmts));
    } else if (t == "return") {
      Expr *v = nullptr;
      if (!f.absent()) {
        v = expr(f, "the returned value or _");
        if (!v)
          return nullptr;
      }
      out = Ctx.make<ReturnStmt>(loc, v);
    } else if (t == "assign") {
      const SExpr *target = f.list("ident");
      if (!target)
        return nullptr;
      auto *id = cast_expr<Identifier>(expr(*target));
      Expr *v = id ? expr(f, "the assigned value") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<AssignStmt>(loc, id, v);
    } else if (t == "decl") {
      const SExpr *v = f.list("var");
      VarDecl *vd = v ? varDecl(*v) : nullptr;
      if (!vd)
        return nullptr;
      out = Ctx.make<DeclStmt>(loc, vd);
    } else if (t == "expr") {
      Expr *x = expr(f, "the expression");
      if (!x)
        return nullptr;
      out = Ctx.make<ExprStmt>(loc, x);
    } else if (t == "if") {
      Expr *cond = expr(f, "the condition");
      CompoundStmt *then = cond ? block(f, "the then branch") : nullptr;
      if (!then)
        return nullptr;
      Stmt *els = nullptr;
      if (!f.absent()) {
        const SExpr *x = f.anyList("the else branch, or _");
        if (!x)
          return nullptr;
        if (x->Text != "block" && x->Text != "if")
          return fail(*x, "the else branch is a (block ...) or an (if ...)"),
                 nullptr;
        els = stmt(*x);
        if (!els)
          return nullptr;
      }
      out = Ctx.make<IfStmt>(loc, cond, then, els);
    } else if (t == "while") {
      Expr *cond = expr(f, "the condition");
      CompoundStmt *body = cond ? block(f, "the loop's body") : nullptr;
      if (!body)
        return nullptr;
      out = Ctx.make<WhileStmt>(loc, cond, body);
    } else if (t == "break") {
      out = Ctx.make<BreakStmt>(loc);
    } else if (t == "continue") {
      out = Ctx.make<ContinueStmt>(loc);
    } else if (t == "member-assign") {
      Expr *recv = expr(f, "the receiver");
      const std::string *field = recv ? f.name("the field's name") : nullptr;
      Expr *v = field ? expr(f, "the assigned value") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<MemberAssignStmt>(loc, recv, *field, v);
    } else if (t == "subscript-assign") {
      Expr *arr = expr(f, "the array");
      Expr *idx = arr ? expr(f, "the index") : nullptr;
      Expr *v = idx ? expr(f, "the assigned value") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<SubscriptAssignStmt>(loc, arr, idx, v);
    } else if (t == "match") {
      Expr *subject = expr(f, "the matched value");
      if (!subject)
        return nullptr;
      std::vector<MatchArm *> arms;
      for (const SExpr *a : f.rest()) {
        MatchArm *arm = matchArm(*a);
        if (!arm)
          return nullptr;
        arms.push_back(arm);
      }
      out = Ctx.make<MatchStmt>(loc, subject, std::move(arms));
    } else if (t == "destructure") {
      const SExpr *tl = f.list("targets");
      if (!tl)
        return nullptr;
      std::vector<DestructureStmt::Target> targets;
      for (const SExpr &te : tl->Items) {
        if (te.K != SExpr::List || te.Text != "target")
          return fail(te, "expected (target \"name\" TYPE-or-_)"), nullptr;
        Fields tf(*this, te);
        SourceLocation tloc = tf.location();
        const std::string *name = tf.string("the target's name (\"\" for _)");
        if (!name)
          return nullptr;
        Type *ty = nullptr;
        if (!tf.absent()) {
          ty = type(tf, "the target's type or _");
          if (!ty)
            return nullptr;
        }
        if (!tf.done())
          return nullptr;
        targets.push_back({name, ty, tloc});
      }
      if (targets.size() < 2)
        return fail(*tl, "a destructuring needs at least two targets"), nullptr;
      Expr *v = expr(f, "the destructured value");
      if (!v)
        return nullptr;
      out = Ctx.make<DestructureStmt>(loc, std::move(targets), v);
    } else {
      return fail(e, "(" + t + " ...) is not a statement"), nullptr;
    }
    return f.done() ? out : nullptr;
  }

  MatchArm *matchArm(const SExpr &e) {
    if (e.K != SExpr::List)
      return fail(e, "expected a match arm"), nullptr;
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *binding = f.string("the arm's binding (\"\" for none)");
    if (!binding)
      return nullptr;
    MatchArm *arm = nullptr;
    if (e.Text == "type-arm") {
      Type *ty = type(f, "the matched type");
      CompoundStmt *body = ty ? block(f, "the arm's body") : nullptr;
      if (body)
        arm = Ctx.make<MatchArm>(loc, *binding, ty, body);
    } else if (e.Text == "value-arm") {
      Expr *pattern = expr(f, "the matched value");
      CompoundStmt *body = pattern ? block(f, "the arm's body") : nullptr;
      if (body)
        arm = Ctx.make<MatchArm>(loc, *binding, pattern, body);
    } else if (e.Text == "wildcard-arm") {
      CompoundStmt *body = block(f, "the arm's body");
      if (body)
        arm = Ctx.make<MatchArm>(loc, *binding, static_cast<Type *>(nullptr),
                                 body);
    } else {
      return fail(e, "(" + e.Text +
                         " ...) is not a match arm (type-arm, value-arm, "
                         "wildcard-arm)"),
             nullptr;
    }
    return arm && f.done() ? arm : nullptr;
  }

  // -- Expressions

  template <typename T> static T *cast_expr(Expr *e) {
    return e ? cast<T>(e) : nullptr;
  }

  Expr *expr(Fields &f, const char *what) {
    const SExpr *e = f.anyList(what);
    return e ? expr(*e) : nullptr;
  }

  bool exprs(Fields &f, std::vector<Expr *> &out) {
    while (!f.atEnd()) {
      Expr *x = expr(f, "an expression");
      if (!x)
        return false;
      out.push_back(x);
    }
    return true;
  }

  static bool binaryOp(const std::string &s, BinaryOpcode &op) {
    static const std::pair<const char *, BinaryOpcode> ops[] = {
        {"add", BinaryOpcode::Add}, {"sub", BinaryOpcode::Sub},
        {"mul", BinaryOpcode::Mul}, {"div", BinaryOpcode::Div},
        {"mod", BinaryOpcode::Mod}, {"lt", BinaryOpcode::Lt},
        {"gt", BinaryOpcode::Gt},   {"le", BinaryOpcode::Le},
        {"ge", BinaryOpcode::Ge},   {"eq", BinaryOpcode::Eq},
        {"ne", BinaryOpcode::Ne},   {"and", BinaryOpcode::And},
        {"or", BinaryOpcode::Or}};
    for (const auto &[name, o] : ops)
      if (s == name) {
        op = o;
        return true;
      }
    return false;
  }

  Expr *expr(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string &t = e.Text;
    Expr *out = nullptr;
    if (t == "int") {
      int64_t v = 0;
      if (!f.integer("the value", v))
        return nullptr;
      out = Ctx.make<IntegerLiteral>(loc, v);
    } else if (t == "float") {
      const SExpr *v = f.next("the value");
      if (!v)
        return nullptr;
      double d = 0;
      if (v->K == SExpr::Symbol &&
          (v->Text == "inf" || v->Text == "-inf" || v->Text == "nan")) {
        d = v->Text == "nan"
                ? std::numeric_limits<double>::quiet_NaN()
                : (v->Text == "inf" ? std::numeric_limits<double>::infinity()
                                    : -std::numeric_limits<double>::infinity());
      } else {
        if (v->K != SExpr::Number || !parseDouble(v->Text, d))
          return fail(*v, "not a floating-point number: " + v->Text), nullptr;
      }
      out = Ctx.make<FloatLiteral>(loc, d);
    } else if (t == "bool") {
      bool v = false;
      if (!f.boolean("the value", v))
        return nullptr;
      out = Ctx.make<BoolLiteral>(loc, v);
    } else if (t == "char") {
      int64_t v = 0;
      if (!f.integer("the character's byte value", v))
        return nullptr;
      if (v < 0 || v > 255)
        return fail(e, "a char is a byte value, 0 to 255"), nullptr;
      out = Ctx.make<CharLiteral>(
          loc, static_cast<char>(static_cast<unsigned char>(v)));
    } else if (t == "none") {
      out = Ctx.make<NoneLiteral>(loc);
    } else if (t == "string") {
      const std::string *v = f.string("the value");
      if (!v)
        return nullptr;
      out = Ctx.make<StringLiteral>(loc, *v);
    } else if (t == "cp" || t == "mv") {
      Expr *x = expr(f, "the operand");
      if (!x)
        return nullptr;
      out = t == "cp" ? static_cast<Expr *>(Ctx.make<CopyExpr>(loc, x))
                      : Ctx.make<MoveExpr>(loc, x);
    } else if (t == "unary") {
      const SExpr *op = f.symbol("the operator (neg or not)");
      if (!op)
        return nullptr;
      if (op->Text != "neg" && op->Text != "not")
        return fail(*op,
                    "unknown unary operator '" + op->Text + "' (neg, not)"),
               nullptr;
      Expr *x = expr(f, "the operand");
      if (!x)
        return nullptr;
      out = Ctx.make<UnaryExpr>(
          loc, op->Text == "neg" ? UnaryOpcode::Neg : UnaryOpcode::Not, x);
    } else if (t == "binary") {
      const SExpr *op = f.symbol("the operator");
      BinaryOpcode bop{};
      if (!op)
        return nullptr;
      if (!binaryOp(op->Text, bop))
        return fail(*op, "unknown binary operator '" + op->Text + "'"), nullptr;
      Expr *lhs = expr(f, "the left operand");
      Expr *rhs = lhs ? expr(f, "the right operand") : nullptr;
      if (!rhs)
        return nullptr;
      out = Ctx.make<BinaryExpr>(loc, bop, lhs, rhs);
    } else if (t == "ident") {
      const std::string *name = f.name("the name");
      if (!name)
        return nullptr;
      out = Ctx.make<Identifier>(loc, *name);
    } else if (t == "call") {
      const std::string *callee = f.name("the callee's name");
      const SExpr *tl = callee ? f.list("type-args") : nullptr;
      if (!tl)
        return nullptr;
      std::vector<Type *> targs;
      for (const SExpr &ta : tl->Items) {
        if (ta.K != SExpr::List)
          return fail(ta, "expected a type"), nullptr;
        Type *ty = type(ta);
        if (!ty)
          return nullptr;
        targs.push_back(ty);
      }
      std::vector<Expr *> args;
      if (!exprs(f, args))
        return nullptr;
      out = Ctx.make<CallExpr>(loc, *callee, std::move(args), std::move(targs));
    } else if (t == "method-call") {
      Expr *recv = expr(f, "the receiver");
      const std::string *name = recv ? f.name("the method's name") : nullptr;
      std::vector<Expr *> args;
      if (!name || !exprs(f, args))
        return nullptr;
      out = Ctx.make<MethodCallExpr>(loc, recv, *name, std::move(args));
    } else if (t == "ternary") {
      Expr *c = expr(f, "the condition");
      Expr *a = c ? expr(f, "the value if true") : nullptr;
      Expr *b = a ? expr(f, "the value if false") : nullptr;
      if (!b)
        return nullptr;
      out = Ctx.make<TernaryExpr>(loc, c, a, b);
    } else if (t == "member") {
      Expr *recv = expr(f, "the receiver");
      const std::string *name = recv ? f.name("the field's name") : nullptr;
      if (!name)
        return nullptr;
      out = Ctx.make<MemberAccessExpr>(loc, recv, *name);
    } else if (t == "array") {
      std::vector<Expr *> elems;
      if (!exprs(f, elems))
        return nullptr;
      out = Ctx.make<ArrayLiteralExpr>(loc, std::move(elems));
    } else if (t == "subscript") {
      Expr *arr = expr(f, "the array");
      Expr *idx = arr ? expr(f, "the index") : nullptr;
      if (!idx)
        return nullptr;
      out = Ctx.make<SubscriptExpr>(loc, arr, idx);
    } else if (t == "enum-value") {
      const std::string *en = f.name("the enum's name");
      const std::string *v = en ? f.name("the variant's name") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<EnumValueExpr>(loc, *en, *v);
    } else if (t == "mov") {
      return fail(e, "(mov ...): " + std::string(frontend::kMovRemoved)),
             nullptr;
    } else if (t == "tuple") {
      std::vector<Expr *> elems;
      if (!exprs(f, elems))
        return nullptr;
      if (elems.size() < 2)
        return fail(e, "a tuple needs at least two elements"), nullptr;
      out = Ctx.make<TupleLiteralExpr>(loc, std::move(elems));
    } else if (t == "tuple-index") {
      Expr *tup = expr(f, "the tuple");
      int64_t idx = 0;
      if (!tup || !f.integer("the index", idx))
        return nullptr;
      if (idx < 0)
        return fail(e, "a tuple index is not negative"), nullptr;
      out = Ctx.make<TupleIndexExpr>(loc, tup, static_cast<size_t>(idx));
    } else {
      return fail(e, "(" + t + " ...) is not an expression"), nullptr;
    }
    return f.done() ? out : nullptr;
  }
};

} // namespace

TranslationUnit *read(std::string_view text, ASTContext &ctx,
                      ReadError &error) {
  error = ReadError();
  Lexer lexer(text);
  SExpr form;
  if (!lexer.top(form)) {
    error = lexer.Err;
    return nullptr;
  }
  Builder b(ctx, error);
  TranslationUnit *tu = b.top(form);
  if (!error.Message.empty())
    return nullptr;
  return tu;
}

} // namespace paykan::ast::interchange
