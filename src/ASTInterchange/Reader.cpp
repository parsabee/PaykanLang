// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
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
// (kMaxDepth lists), and so is every recursion here.  The AST is held to the
// frontends' own limits too, nesting (frontend::kMaxNesting) and chains
// (frontend::kMaxChain), counted as the recursive-descent frontend counts
// them, so that an AST from a plugin can make the passes after it recurse no
// deeper than a parsed program can.

#include "Names.h"
#include "paykan/Frontend.h"
#include "paykan/ast/Interchange.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paykan::ast::interchange {

static_assert(kMaxDepth == 4 * frontend::kMaxNesting + frontend::kMaxChain + 16,
              "kMaxDepth fits every AST within the frontends' limits");

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

  SExpr() = default;
  SExpr(SExpr &&) = default;
  SExpr &operator=(SExpr &&) = default;
  SExpr(const SExpr &) = delete;
  SExpr &operator=(const SExpr &) = delete;
  /// The lists in the list are taken apart in a loop, not by a recursion a
  /// level per list: they nest up to kMaxDepth deep.
  ~SExpr() {
    if (Items.empty())
      return;
    std::vector<std::vector<SExpr>> pending;
    pending.push_back(std::move(Items));
    while (!pending.empty()) {
      std::vector<SExpr> items = std::move(pending.back());
      pending.pop_back();
      for (SExpr &item : items)
        if (!item.Items.empty())
          pending.push_back(std::move(item.Items));
    }
  }
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
    if (!list(out))
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

  /// The list at Pos, '(' on: its tag, then its items.
  bool openList(SExpr &out) {
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
    return true;
  }

  /// The list at Pos into @p out, and the lists in it: in a loop, the
  /// lists open innermost last in a stack of their own (each one's address
  /// holds until it is closed, as only the innermost gets items), not by a
  /// recursion a level per list.
  bool list(SExpr &out) {
    if (!openList(out))
      return false;
    std::vector<SExpr *> open{&out};
    while (!open.empty()) {
      SExpr &list = *open.back();
      skipSpace();
      if (Pos >= Text.size())
        return fail("unterminated list (" + list.Text + " ...) from " +
                    std::to_string(list.Line) + ":" +
                    std::to_string(list.Column));
      char c = Text[Pos];
      if (c == ')') {
        advance();
        open.pop_back();
        continue;
      }
      list.Items.emplace_back();
      SExpr &item = list.Items.back();
      if (c == '(') {
        if (open.size() >= kMaxDepth)
          return fail("nesting too deep (more than " +
                      std::to_string(kMaxDepth) + " levels)");
        if (!openList(item))
          return false;
        open.push_back(&item);
      } else if (c == '"') {
        if (!string(item))
          return false;
      } else {
        atom(item);
      }
    }
    return true;
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
  Builder(ASTContext &ctx, ReadError &err, Origin origin)
      : Ctx(ctx), Err(err), From(origin) {}

  TranslationUnit *top(const SExpr &form) {
    if (form.Text != "paykan-ast")
      return fail(form, "expected (paykan-ast <version> (unit ...)), not (" +
                            form.Text + " ...)"),
             nullptr;
    if (form.Items.size() != 2 || form.Items[0].K != SExpr::Number)
      return fail(form, "expected (paykan-ast <version> (unit ...))"), nullptr;
    if (form.Items[0].Text != versionString(kFormatVersion))
      return fail(form.Items[0], "unsupported AST format version " +
                                     form.Items[0].Text +
                                     " (this paykan reads version " +
                                     versionString(kFormatVersion) + ")"),
             nullptr;
    return unit(form.Items[1]);
  }

private:
  ASTContext &Ctx;
  ReadError &Err;
  /// Who wrote the text: a frontend's names are the grammar's.
  Origin From;
  /// The levels of nesting (frontend::kMaxNesting) around what is built.
  unsigned Depth = 0;
  /// The chain links (frontend::kMaxChain) on the deepest path down what
  /// was built since the innermost Link began.
  unsigned ChainHeight = 0;

  bool failed() const { return !Err.Message.empty(); }

  /// One level of nesting, as the frontends count them (docs/grammar.md
  /// section 9), around the fields read while it lives: a block's
  /// statements, a declaration body's members, a match's arms, an operand
  /// of a prefix operator or a conditional, the arguments of a call, an
  /// array's or a tuple's elements, a subscript's index, a lambda's result
  /// type, a type's arguments, elements, parameters, result or the type a
  /// suffix wraps.  The AST has no parentheses, so a parenthesised
  /// expression's level is not there to count: what a frontend parsed
  /// within the limit is within it here.
  class Nested {
  public:
    Nested(Builder &b, const SExpr &at) : B(b) {
      if (++B.Depth > frontend::kMaxNesting)
        B.fail(at, "nesting too deep (more than " +
                       std::to_string(frontend::kMaxNesting) + " levels)");
    }
    ~Nested() { --B.Depth; }
    Nested(const Nested &) = delete;
    Nested &operator=(const Nested &) = delete;
    /// False when this level is one too many.
    bool ok() const { return B.Depth <= frontend::kMaxNesting; }

  private:
    Builder &B;
  };

  /// A link of a chain, as the frontends count them (docs/grammar.md
  /// section 9): a `binary`, and a `call-value`, `member`, `method-call`,
  /// `tuple-index` or `subscript` (also as the target of a
  /// `member-assign` or a `subscript-assign`), one level of the chain over
  /// the deepest of the fields built while it lives.
  class Link {
  public:
    Link(Builder &b, const SExpr &at)
        : B(b), At(at), Outer(std::exchange(b.ChainHeight, 0)) {}
    ~Link() { B.ChainHeight = std::max(Outer, B.ChainHeight); }
    Link(const Link &) = delete;
    Link &operator=(const Link &) = delete;
    /// Count the link, once its fields are built: false (failed) when it is
    /// one too many.
    bool ok() {
      if (++B.ChainHeight <= frontend::kMaxChain)
        return true;
      B.fail(At, "nesting too deep (more than " +
                     std::to_string(frontend::kMaxChain) + " levels)");
      return false;
    }

  private:
    Builder &B;
    const SExpr &At;
    unsigned Outer;
  };

  void fail(const SExpr &at, const std::string &msg) {
    if (failed())
      return;
    Err.Line = at.Line;
    Err.Column = at.Column;
    Err.Message = msg;
  }
  /// fail() with a literal: the string is made here, not in the frame of a
  /// builder that a deep AST may hold hundreds of times over.
  void fail(const SExpr &at, const char *msg) { fail(at, std::string(msg)); }

  const std::string &intern(const std::string &s) { return Ctx.intern(s); }

  /// What a NAME may be where it stands, as the grammar writes it there
  /// (docs/grammar.md sections 1 and 5).
  enum class NameKind {
    Ident,     ///< an identifier: anything declared, a member, a local
    Path,      ///< identifiers joined by `::`: a callee, an enum, a type
    TypeParam, ///< an identifier, or an untyped parameter's `typeof(x)`
    TypeName,  ///< a Path, or an untyped parameter's `typeof(x)`
    Member,    ///< an identifier, or a tuple index (`t.0 = e`)
  };

  static bool isPath(std::string_view s) {
    for (size_t at = 0;;) {
      size_t sep = s.find("::", at);
      if (!frontend::isIdentifier(s.substr(at, sep - at)))
        return false;
      if (sep == std::string_view::npos)
        return true;
      at = sep + 2;
    }
  }

  static bool isName(std::string_view s, NameKind kind) {
    auto untyped = [&] {
      return frontend::isIdentifier(names::untypedParamOf(s));
    };
    switch (kind) {
    case NameKind::Ident:
      return frontend::isIdentifier(s);
    case NameKind::Path:
      return isPath(s);
    case NameKind::TypeParam:
      return frontend::isIdentifier(s) || untyped();
    case NameKind::TypeName:
      return isPath(s) || untyped();
    case NameKind::Member:
      // A tuple index as the parser reads it: digits without a leading
      // zero, at most INT64_MAX.
      return frontend::isIdentifier(s) ||
             (!s.empty() &&
              s.find_first_not_of("0123456789") == std::string_view::npos &&
              (s.size() == 1 || s[0] != '0') &&
              (s.size() < 19 ||
               (s.size() == 19 && s <= "9223372036854775807")));
    }
    return false;
  }

  static const char *nameKindText(NameKind kind) {
    switch (kind) {
    case NameKind::Ident:
      return "an identifier";
    case NameKind::Path:
      return "an identifier or a qualified name (a::b)";
    case NameKind::TypeParam:
      return "an identifier or an untyped parameter's typeof(x)";
    case NameKind::TypeName:
      return "an identifier, a qualified name (a::b) or an untyped "
             "parameter's typeof(x)";
    case NameKind::Member:
      return "an identifier or a tuple index";
    }
    return "";
  }

  /// Fail at @p at unless @p s is a NAME of @p kind (@p what is the
  /// field's role, for the message), in a frontend's text.
  bool checkName(const SExpr &at, const std::string &s, NameKind kind,
                 const std::string &what) {
    if (From != Origin::Frontend || isName(s, kind))
      return true;
    // Quoted as the format writes it: a plugin's name can hold any byte.
    fail(at, "expected " + std::string(nameKindText(kind)) + ": " + what +
                 ", not " + quote(s));
    return false;
  }

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

    /// The next field if it is a list tagged @p tag (consumed), else null:
    /// an optional trailing item.
    const SExpr *optionalList(const char *tag) {
      if (!nextIsList(tag))
        return nullptr;
      return &L.Items[I++];
    }
    /// Whether the next field is the list @p tag (not consumed).
    bool nextIsList(const char *tag) const {
      return !atEnd() && L.Items[I].K == SExpr::List && L.Items[I].Text == tag;
    }
    /// The next field, not consumed (the caller checked there is one).
    const SExpr *peekList() const { return &L.Items[I]; }

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

    /// A NAME of @p kind.
    const std::string *name(const char *what, NameKind kind = NameKind::Ident) {
      const SExpr *at = atEnd() ? nullptr : &L.Items[I];
      const std::string *s = string(what);
      if (s && s->empty()) {
        B.fail(*at, std::string("empty name: ") + what);
        return nullptr;
      }
      return s && B.checkName(*at, *s, kind, what) ? s : nullptr;
    }

    /// A NAME of @p kind, or "" for none.
    const std::string *optionalName(const char *what,
                                    NameKind kind = NameKind::Ident) {
      const SExpr *at = atEnd() ? nullptr : &L.Items[I];
      const std::string *s = string(what);
      return s && (s->empty() || B.checkName(*at, *s, kind, what)) ? s
                                                                   : nullptr;
    }

    /// The field read last (the caller read one).
    const SExpr &last() const { return L.Items[I - 1]; }

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

    /// The value of an `(int ...)`: B.literalInteger of the next field.
    bool literalInteger(const char *what, int64_t &bits, bool &above) {
      const SExpr *e = next(what);
      if (!e)
        return false;
      if (e->K != SExpr::Number) {
        B.fail(*e, std::string("expected an integer: ") + what);
        return false;
      }
      return B.literalInteger(*e, bits, above);
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

  /// An integer literal's value (docs/plugins/ast-format.md): an int64, or a
  /// value without a sign up to 2^64-1, whose bits go to @p bits with
  /// @p above set (a literal above INT64_MAX, docs/design/bitwise.md).
  bool literalInteger(const SExpr &e, int64_t &bits, bool &above) {
    const char *first = e.Text.data(), *last = first + e.Text.size();
    above = false;
    if (auto [p, ec] = std::from_chars(first, last, bits);
        ec == std::errc() && p == last)
      return true;
    uint64_t value = 0;
    if (auto [p, ec] = std::from_chars(first, last, value);
        ec == std::errc() && p == last) {
      bits = static_cast<int64_t>(value);
      above = true;
      return true;
    }
    fail(e, "not a 64-bit integer: " + e.Text);
    return false;
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
    std::vector<EnumDecl *> enums, genericEnums;
    std::vector<ImplDecl *> impls;
    std::vector<TraitDecl *> traits;
    std::vector<StructDecl *> structs, genericStructs;
    for (const SExpr *d : f.rest()) {
      if (d->K != SExpr::List) {
        fail(*d, "expected a declaration: (import ...), (enum ...), "
                 "(class ...), (struct ...), (fn ...), (trait ...) or "
                 "(impl ...)");
        return nullptr;
      }
      if (d->Text == "import") {
        if (auto *i = importDecl(*d))
          imports.push_back(i);
      } else if (d->Text == "enum") {
        if (auto *en = enumDecl(*d))
          (en->isGeneric() ? genericEnums : enums).push_back(en);
      } else if (d->Text == "class") {
        if (auto *c = classDecl(*d))
          (c->isGeneric() ? genericClasses : classes).push_back(c);
      } else if (d->Text == "struct") {
        if (auto *s = structDecl(*d))
          (s->isGeneric() ? genericStructs : structs).push_back(s);
      } else if (d->Text == "fn") {
        if (auto *fn = funcDecl(*d))
          (fn->isGeneric() ? genericFuncs : funcs).push_back(fn);
      } else if (d->Text == "impl") {
        if (auto *im = implDecl(*d))
          impls.push_back(im);
      } else if (d->Text == "trait") {
        if (auto *t = traitDecl(*d))
          traits.push_back(t);
      } else {
        fail(*d, "(" + d->Text +
                     " ...) is not a top-level declaration (import, enum, "
                     "class, struct, fn, trait, impl)");
      }
      if (failed())
        return nullptr;
    }
    return Ctx.make<TranslationUnit>(
        loc, std::move(imports), std::move(classes), std::move(funcs),
        std::move(enums), std::move(genericClasses), std::move(genericFuncs),
        std::move(impls), std::move(traits), std::move(structs),
        std::move(genericStructs), std::move(genericEnums));
  }

  StructDecl *structDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the struct's name");
    std::vector<const std::string *> params;
    TypeParamBounds bounds;
    if (!name || !typeParams(f, params, bounds))
      return nullptr;
    std::vector<VarDecl *> fields;
    const SExpr *fl = f.list("fields");
    if (!fl)
      return nullptr;
    Nested body(*this, *fl);
    if (!body.ok())
      return nullptr;
    for (const SExpr &v : fl->Items) {
      if (v.K != SExpr::List || v.Text != "var")
        return fail(v, "expected a field: (var ...)"), nullptr;
      auto *vd = varDecl(v);
      if (!vd)
        return nullptr;
      if (vd->getInitExpr() || !vd->getType())
        return fail(v, "a field has a type and no initialiser"), nullptr;
      if (vd->isLet())
        return fail(v, "a field cannot be declared with let"), nullptr;
      if (vd->isWeak())
        return fail(v, "a struct field cannot be weak"), nullptr;
      fields.push_back(vd);
    }
    // The check the grammar makes (docs/grammar.md).
    if (fields.empty())
      return fail(e, "a struct has at least one field"), nullptr;
    if (!f.done())
      return nullptr;
    auto *sd =
        Ctx.make<StructDecl>(loc, *name, std::move(fields), std::move(params));
    sd->setTypeParamBounds(std::move(bounds));
    return sd;
  }

  ImplDecl *implDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *typeName = f.name("the type's name", NameKind::Path);
    if (!typeName)
      return nullptr;
    // `impl Box<T>`: (type-params NAME...) after the type's name.
    std::vector<const std::string *> params;
    if (const SExpr *tl = f.optionalList("type-params")) {
      for (const SExpr &item : tl->Items) {
        if (item.K != SExpr::String || item.Text.empty())
          return fail(item, "expected a type parameter name"), nullptr;
        if (!checkName(item, item.Text, NameKind::Ident, "a type parameter"))
          return nullptr;
        params.push_back(&intern(item.Text));
      }
      if (params.empty())
        return fail(*tl, "(type-params) of an impl needs a name"), nullptr;
    }
    // `impl Show for A`: (for TRAIT) after the type's name.
    Type *trait = nullptr;
    if (const SExpr *fl = f.optionalList("for")) {
      Fields tf(*this, *fl);
      trait = type(tf, "the trait");
      if (!trait || !tf.done())
        return nullptr;
    }
    std::vector<FuncDecl *> methods;
    {
      Nested body(*this, e);
      if (!body.ok() || !methodList(f, methods, FuncKind::ImplMethod))
        return nullptr;
    }
    if (!f.done())
      return nullptr;
    return Ctx.make<ImplDecl>(loc, *typeName, std::move(methods), trait,
                              std::move(params));
  }

  TraitDecl *traitDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the trait's name");
    std::vector<const std::string *> tparams;
    TypeParamBounds bounds;
    if (!name || !typeParams(f, tparams, bounds))
      return nullptr;
    if (!bounds.empty())
      return fail(e, "a trait's type parameters cannot have bounds"), nullptr;
    std::vector<FuncDecl *> methods;
    {
      Nested body(*this, e);
      if (!body.ok() || !methodList(f, methods, FuncKind::Requirement))
        return nullptr;
    }
    if (!f.done())
      return nullptr;
    return Ctx.make<TraitDecl>(loc, *name, std::move(tparams),
                               std::move(methods));
  }

  /// Where a function is declared, which decides what may stand where its
  /// body goes and whether it may be a `static fn`.
  enum class FuncKind {
    Free,        ///< a top-level function: a block, or a native's C symbol
    ImplMethod,  ///< an `impl` block's method or `static fn`: a block
    Requirement, ///< a trait's method: `_`
    ClassMethod, ///< a class body's: a block, or `_` for a pure virtual one
  };

  /// The `(methods FN...)` field into @p out, each declared as @p kind
  /// says.
  bool methodList(Fields &f, std::vector<FuncDecl *> &out, FuncKind kind) {
    const SExpr *ml = f.list("methods");
    if (!ml)
      return false;
    for (const SExpr &m : ml->Items) {
      if (m.K != SExpr::List || m.Text != "fn")
        return fail(m, "expected a method: (fn ...)"), false;
      auto *fn = funcDecl(m, kind);
      if (!fn)
        return false;
      out.push_back(fn);
    }
    return true;
  }

  ImportDecl *importDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    bool system = false;
    if (!f.boolean("whether the import is a system import (::name)", system))
      return nullptr;
    const std::string *base = f.optionalName("the base path", NameKind::Path);
    if (!base)
      return nullptr;
    std::vector<ImportDecl::Module> modules;
    for (const SExpr *m : f.rest()) {
      if (m->K != SExpr::List || m->Text != "module")
        return fail(*m, "expected (module \"name\" \"alias\")"), nullptr;
      Fields mf(*this, *m);
      const std::string *name = mf.name("the module's name");
      const std::string *alias = name ? mf.optionalName("the alias") : nullptr;
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
    // A generic enum's (type-params ...) after its name.
    std::vector<const std::string *> params;
    TypeParamBounds bounds;
    if (f.nextIsList("type-params")) {
      const SExpr &tl = *f.peekList();
      if (!typeParams(f, params, bounds))
        return nullptr;
      if (params.empty())
        return fail(tl, "(type-params) of an enum needs a name"), nullptr;
    }
    std::vector<const std::string *> variants;
    std::vector<std::vector<VarDecl *>> payloads;
    while (!f.atEnd()) {
      // A plain variant is its name; one with fields is
      // (variant "Name" (var "f" TYPE _)...).
      if (const SExpr *vl = f.optionalList("variant")) {
        Fields vf(*this, *vl);
        const std::string *v = vf.name("a variant");
        if (!v)
          return nullptr;
        std::vector<VarDecl *> fields;
        while (!vf.atEnd()) {
          const SExpr *fe = vf.list("var");
          VarDecl *field = fe ? varDecl(*fe) : nullptr;
          if (!field)
            return nullptr;
          if (!field->getType() || field->getInitExpr() || field->isLet() ||
              field->isWeak() || field->getMode() != ParamMode::Value)
            return fail(*fe, "a variant's field is a name and a type only"),
                   nullptr;
          fields.push_back(field);
        }
        if (fields.empty())
          return fail(*vl, "(variant) needs at least one field"), nullptr;
        variants.push_back(v);
        payloads.push_back(std::move(fields));
        continue;
      }
      const std::string *v = f.name("a variant");
      if (!v)
        return nullptr;
      variants.push_back(v);
      payloads.emplace_back();
    }
    // The check the grammar makes (docs/grammar.md).
    if (variants.empty())
      return fail(e, "an enum needs at least one variant"), nullptr;
    auto *ed = Ctx.make<EnumDecl>(loc, *name, std::move(variants),
                                  std::move(payloads), std::move(params));
    ed->setTypeParamBounds(std::move(bounds));
    return ed;
  }

  /// `(type-params "T" (bound "U" TRAIT...))` into @p out and @p bounds
  /// (left empty when no parameter has a bound).
  bool typeParams(Fields &f, std::vector<const std::string *> &out,
                  TypeParamBounds &bounds) {
    const SExpr *l = f.list("type-params");
    if (!l)
      return false;
    bool anyBound = false;
    for (const SExpr &item : l->Items) {
      std::vector<Type *> traits;
      const std::string *p = nullptr;
      if (item.K == SExpr::List && item.Text == "bound") {
        Fields bf(*this, item);
        p = bf.name("a type parameter", NameKind::TypeParam);
        if (!p)
          return false;
        while (!bf.atEnd()) {
          Type *t = type(bf, "a bound");
          if (!t)
            return false;
          traits.push_back(t);
        }
        if (traits.empty())
          return fail(item, "(bound ...) needs at least one trait"), false;
        anyBound = true;
      } else if (item.K == SExpr::String && !item.Text.empty()) {
        if (!checkName(item, item.Text, NameKind::TypeParam,
                       "a type parameter"))
          return false;
        p = &intern(item.Text);
      } else {
        return fail(item, "expected a type parameter: \"T\" or "
                          "(bound \"T\" TRAIT...)"),
               false;
      }
      out.push_back(p);
      bounds.push_back(std::move(traits));
    }
    if (!anyBound)
      bounds.clear();
    return true;
  }

  ClassDecl *classDecl(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the class's name");
    const std::string *super =
        name ? f.optionalName("the superclass", NameKind::Path) : nullptr;
    std::vector<const std::string *> params;
    TypeParamBounds bounds;
    if (!super || !typeParams(f, params, bounds))
      return nullptr;
    std::vector<VarDecl *> fields;
    std::vector<FuncDecl *> methods;
    const SExpr *fl = f.list("fields");
    if (!fl)
      return nullptr;
    Nested body(*this, *fl);
    if (!body.ok())
      return nullptr;
    for (const SExpr &v : fl->Items) {
      if (v.K != SExpr::List || v.Text != "var")
        return fail(v, "expected a field: (var ...)"), nullptr;
      auto *vd = varDecl(v);
      if (!vd)
        return nullptr;
      if (vd->getInitExpr() || !vd->getType())
        return fail(v, "a field has a type and no initialiser"), nullptr;
      if (vd->isLet())
        return fail(v, "a field cannot be declared with let"), nullptr;
      fields.push_back(vd);
    }
    if (!methodList(f, methods, FuncKind::ClassMethod) || !f.done())
      return nullptr;
    auto *cd = Ctx.make<ClassDecl>(loc, *name, *super, std::move(fields),
                                   std::move(methods), std::move(params));
    cd->setTypeParamBounds(std::move(bounds));
    return cd;
  }

  /// A function's or a lambda's parameters, @p pl's `(param LOC NAME TYPE
  /// QUAL?)` each.  A parameter without a location is at @p loc, its
  /// function's or its lambda's (a location is optional).
  bool paramList(const SExpr &pl, SourceLocation loc, std::vector<Param> &out) {
    for (const SExpr &p : pl.Items) {
      if (p.K != SExpr::List || p.Text != "param")
        return fail(p, "expected (param \"name\" TYPE)"), false;
      Fields pf(*this, p);
      SourceLocation ploc = pf.location();
      if (!ploc.isValid())
        ploc = loc;
      const std::string *pname = pf.name("the parameter's name");
      Type *pty = pname ? type(pf, "the parameter's type") : nullptr;
      ParamMode mode = ParamMode::Value;
      if (!pty || !paramMode(pf, mode) || !pf.done())
        return false;
      out.push_back({pname, pty, mode, ploc});
    }
    return true;
  }

  /// A function declared as @p kind says.  Its body, and its `(qual
  /// static)`, are checked as the grammar's are (docs/grammar.md): only a
  /// top-level function is native, only a class's or a trait's method has
  /// no body and only an `impl` block's method is a `static fn`.
  FuncDecl *funcDecl(const SExpr &e, FuncKind kind = FuncKind::Free) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name = f.name("the function's name");
    std::vector<const std::string *> tparams;
    TypeParamBounds bounds;
    if (!name || !typeParams(f, tparams, bounds))
      return nullptr;
    // `___init__` is the initializer `__init__`, private to its module.
    const bool modulePrivateInit = *name == names::kMethodInitModulePrivate;
    if (modulePrivateInit)
      name = &Ctx.intern(names::kMethodInit);
    const SExpr *pl = f.list("params");
    std::vector<Param> params;
    if (!pl || !paramList(*pl, loc, params))
      return nullptr;
    Type *ret = nullptr;
    if (!f.absent()) {
      ret = type(f, "the return type or _");
      if (!ret)
        return nullptr;
    }
    // A borrowed result (`-> view T`) is a `(qual ...)` after its type.
    ParamMode resultMode = ParamMode::Value;
    if (!paramMode(f, resultMode))
      return nullptr;
    if (resultMode != ParamMode::Value && !ret)
      return fail(e, "a borrowed result needs a type"), nullptr;
    // A `native fn` has (native "symbol") where the body goes (#198).
    if (const SExpr *nl = f.optionalList("native")) {
      if (kind != FuncKind::Free)
        return fail(e, "function '" + *name +
                           "' has (native ...) for its body: only a "
                           "top-level function can (a 'native fn')"),
               nullptr;
      Fields nf(*this, *nl);
      // Any C symbol, which is no NAME of the grammar's.
      const std::string *symbol = nf.string("the C symbol");
      if (symbol && symbol->empty())
        return fail(nf.last(), "empty name: the C symbol"), nullptr;
      if (!symbol || !nf.done() || !f.done())
        return nullptr;
      if (std::string_view why = names::cSymbolProblem(*symbol); !why.empty())
        return fail(*nl, "the C symbol '" + *symbol + "' of native function '" +
                             *name + "' " + std::string(why)),
               nullptr;
      auto *fn = Ctx.make<FuncDecl>(loc, *name, std::move(params), ret, nullptr,
                                    std::move(tparams));
      fn->setNativeSymbol(*symbol);
      fn->setResultMode(resultMode);
      fn->setModulePrivateInit(modulePrivateInit);
      return fn;
    }
    CompoundStmt *body = nullptr;
    if (kind == FuncKind::Requirement) {
      if (!tparams.empty())
        return fail(e, "a trait method cannot be generic"), nullptr;
      if (!f.absent())
        return fail(e, "a trait method has `_` for its body"), nullptr;
    } else if (f.absent()) {
      // Only a class's method is pure virtual.
      if (kind != FuncKind::ClassMethod)
        return fail(e, "function '" + *name +
                           "' has `_` for its body: only a class's method "
                           "can (a pure virtual method)"),
               nullptr;
    } else {
      body = block(f, "the function's body");
      if (!body)
        return nullptr;
    }
    // A `view fn` ends with `(qual view)`, a `static fn` with
    // `(qual static)`.
    bool view = false, isStatic = false;
    if (const SExpr *q = f.optionalList("qual")) {
      const SExpr *v = q->Items.size() == 1 ? &q->Items[0] : nullptr;
      if (!v || v->K != SExpr::Symbol ||
          (v->Text != "view" && v->Text != "static"))
        return fail(*q, "expected (qual view) or (qual static)"), nullptr;
      view = v->Text == "view";
      isStatic = !view;
    }
    if (isStatic && kind != FuncKind::ImplMethod)
      return fail(e, "function '" + *name +
                         "' has (qual static): only an 'impl' block's "
                         "method can (a 'static fn')"),
             nullptr;
    // Only a free function is generic, as the grammar has it.
    if (!tparams.empty() &&
        (kind == FuncKind::ClassMethod || kind == FuncKind::ImplMethod))
      return fail(e, std::string(isStatic ? frontend::kGenericStaticFn
                                          : frontend::kGenericMethod)),
             nullptr;
    // `fn double(x: int) => x * 2;`: a trailing (inferred), as a lambda's.
    const SExpr *inferred = f.optionalList("inferred");
    if (inferred) {
      const auto *stmts = body ? &body->getStatements() : nullptr;
      const auto *r = stmts && stmts->size() == 1
                          ? dyn_cast<ReturnStmt>((*stmts)[0])
                          : nullptr;
      if (!inferred->Items.empty())
        return fail(*inferred, "(inferred) takes no fields"), nullptr;
      if (ret || !r || !r->getReturnValue())
        return fail(*inferred,
                    "an inferred result needs no result type and a body "
                    "that is one return of a value"),
               nullptr;
    }
    if (!f.done())
      return nullptr;
    auto *fn = Ctx.make<FuncDecl>(loc, *name, std::move(params), ret, body,
                                  std::move(tparams));
    fn->setInferredResult(inferred != nullptr);
    fn->setView(view);
    fn->setStatic(isStatic);
    fn->setResultMode(resultMode);
    fn->setModulePrivateInit(modulePrivateInit);
    fn->setTypeParamBounds(std::move(bounds));
    return fn;
  }

  /// A parameter's or a local borrow's optional `(qual view)` /
  /// `(qual inout)` into @p out; false (after failing) for any other
  /// `(qual ...)`.
  bool paramMode(Fields &f, ParamMode &out) {
    const SExpr *q = f.optionalList("qual");
    if (!q)
      return true;
    const SExpr *v = q->Items.size() == 1 ? &q->Items[0] : nullptr;
    for (ParamMode m : {ParamMode::View, ParamMode::Inout})
      if (v && v->K == SExpr::Symbol && v->Text == paramModeName(m)) {
        out = m;
        return true;
      }
    fail(*q, "expected (qual view) or (qual inout)");
    return false;
  }

  /// @p discardable: the name may be "" (`_`), as a for loop's may.
  VarDecl *varDecl(const SExpr &e, bool discardable = false) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *name =
        discardable ? f.optionalName("the variable's name (\"\" for _)")
                    : f.name("the variable's name");
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
    // A local borrow (`x: view = e;`) carries (qual view) or (qual inout),
    // `let x = e;` a trailing (let) and `weak y: T? = e;` (or a class's
    // `weak` field) a trailing (weak); a borrow or a let has an initialiser.
    ParamMode mode = ParamMode::Value;
    if (!paramMode(f, mode))
      return nullptr;
    const bool borrow = mode != ParamMode::Value;
    if (borrow && !init)
      return fail(e, "a local borrow has an initialiser"), nullptr;
    const SExpr *let = f.optionalList("let");
    if (let && !let->Items.empty())
      return fail(*let, "(let) takes no fields"), nullptr;
    if (let && !init)
      return fail(*let, "a let declaration has an initialiser"), nullptr;
    if (let && borrow)
      return fail(*let, "a local borrow cannot be let"), nullptr;
    const SExpr *weak = f.optionalList("weak");
    if (weak && !weak->Items.empty())
      return fail(*weak, "(weak) takes no fields"), nullptr;
    if (weak && (let || borrow))
      return fail(*weak, "a weak declaration is neither let nor a borrow"),
             nullptr;
    if (weak && !ty && !init)
      return fail(*weak, "a weak declaration has a type or an initialiser"),
             nullptr;
    if (!f.done())
      return nullptr;
    auto *vd = Ctx.make<VarDecl>(loc, *name, ty, init);
    vd->setLet(let != nullptr);
    vd->setWeak(weak != nullptr);
    vd->setMode(mode);
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
      const std::string *name = f.name("the type's name", NameKind::TypeName);
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
      Nested inner(*this, e);
      Type *elem = inner.ok() ? type(f, "the element type") : nullptr;
      if (!elem || !f.done())
        return nullptr;
      return Ctx.make<ArrayType>(loc, elem);
    }
    if (e.Text == "fixed-array-type") {
      Type *elem = nullptr;
      {
        Nested inner(*this, e);
        elem = inner.ok() ? type(f, "the element type") : nullptr;
      }
      if (!elem)
        return nullptr;
      // `_`: a length the declaration infers (docs/grammar.md).
      int64_t n = 0;
      const SExpr *len = f.next("the length");
      if (!len)
        return nullptr;
      if (len->K == SExpr::Symbol && len->Text == "_")
        n = FixedArrayType::kInferredLength;
      else if (len->K != SExpr::Number)
        return fail(*len, "expected an integer or _: the length"), nullptr;
      else if (!integer(*len, n))
        return nullptr;
      if (!f.done())
        return nullptr;
      // The check the grammar makes (docs/grammar.md).
      if (n < 1 && n != FixedArrayType::kInferredLength)
        return fail(e, "a fixed-size array has at least one element: its "
                       "length must be 1 or more"),
               nullptr;
      return Ctx.make<FixedArrayType>(loc, elem, n);
    }
    if (e.Text == "optional-type") {
      Type *inner = nullptr;
      {
        Nested level(*this, e);
        inner = level.ok() ? type(f, "the inner type") : nullptr;
      }
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
      Nested inner(*this, e);
      if (!inner.ok())
        return nullptr;
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
    if (e.Text == "fn-type") {
      // The parameters and the result, one level in.
      Nested inner(*this, e);
      const SExpr *pl = inner.ok() ? f.list("params") : nullptr;
      if (!pl)
        return nullptr;
      std::vector<Type *> params;
      std::vector<ParamMode> modes;
      for (const SExpr &p : pl->Items) {
        if (p.K != SExpr::List || p.Text != "param")
          return fail(p, "expected a parameter: (param TYPE)"), nullptr;
        Fields pf(*this, p);
        Type *pty = type(pf, "the parameter's type");
        ParamMode mode = ParamMode::Value;
        if (!pty || !paramMode(pf, mode) || !pf.done())
          return nullptr;
        params.push_back(pty);
        modes.push_back(mode);
      }
      Type *ret = type(f, "the result type");
      if (!ret || !f.done())
        return nullptr;
      return Ctx.make<FunctionType>(loc, std::move(params), std::move(modes),
                                    ret);
    }
    if (e.Text == "generic-type") {
      const std::string *name =
          f.name("the generic type's name", NameKind::Path);
      Nested inner(*this, e);
      if (!name || !inner.ok())
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
                       "fixed-array-type, optional-type, tuple-type, "
                       "fn-type, generic-type)"),
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
      Nested inner(*this, e);
      if (!inner.ok())
        return nullptr;
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
      // The check the grammar makes (docs/grammar.md): a local starts with
      // its initialiser's value, a `weak` one without one as None.
      if (!vd->getInitExpr() && !vd->isWeak())
        return fail(*v, "a local declaration has an initialiser (only a "
                        "(weak) one may leave it out)"),
               nullptr;
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
        if (x->Text == "if") {
          // An `elif`: a level of nesting, as the frontends count it.
          Nested level(*this, *x);
          els = level.ok() ? stmt(*x) : nullptr;
        } else {
          els = stmt(*x);
        }
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
    } else if (t == "for") {
      // (for (var NAME TYPE-or-_ _) START END STEP-or-_ BLOCK (inclusive)?),
      // NAME "" for `for _`.
      const SExpr *v = f.list("var");
      VarDecl *var = v ? varDecl(*v, true) : nullptr;
      if (!var)
        return nullptr;
      if (var->getInitExpr() || var->isLet() || var->isWeak() ||
          var->getMode() != ParamMode::Value)
        return fail(*v, "a for loop's variable is a name and a type or _ "
                        "only"),
               nullptr;
      Expr *start = expr(f, "the start of the range");
      Expr *end = start ? expr(f, "the end of the range") : nullptr;
      if (!end)
        return nullptr;
      Expr *step = nullptr;
      if (!f.absent()) {
        step = expr(f, "the step or _");
        if (!step)
          return nullptr;
      }
      CompoundStmt *body = block(f, "the loop's body");
      if (!body)
        return nullptr;
      const SExpr *inclusive = f.optionalList("inclusive");
      if (inclusive && !inclusive->Items.empty())
        return fail(*inclusive, "(inclusive) takes no fields"), nullptr;
      out = Ctx.make<ForStmt>(loc, var, start, end, step, inclusive != nullptr,
                              body);
    } else if (t == "break") {
      out = Ctx.make<BreakStmt>(loc);
    } else if (t == "continue") {
      out = Ctx.make<ContinueStmt>(loc);
    } else if (t == "member-assign") {
      Expr *recv = nullptr;
      {
        Link target(*this, e); // the `recv.field` assigned to
        recv = operand(f, e, "the receiver", Operand::Receiver);
        if (recv && !target.ok())
          return nullptr;
      }
      const std::string *field =
          recv ? f.name("the field's name", NameKind::Member) : nullptr;
      Expr *v = field ? expr(f, "the assigned value") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<MemberAssignStmt>(loc, recv, *field, v);
    } else if (t == "subscript-assign") {
      Expr *arr = nullptr, *idx = nullptr;
      {
        Link target(*this, e); // the `arr[idx]` assigned to
        arr = operand(f, e, "the array", Operand::Receiver);
        idx = arr ? nestedExpr(f, e, "the index") : nullptr;
        if (idx && !target.ok())
          return nullptr;
      }
      Expr *v = idx ? expr(f, "the assigned value") : nullptr;
      if (!v)
        return nullptr;
      out = Ctx.make<SubscriptAssignStmt>(loc, arr, idx, v);
    } else if (t == "match") {
      Expr *subject = expr(f, "the matched value");
      if (!subject)
        return nullptr;
      std::vector<MatchArm *> arms;
      Nested inner(*this, e);
      if (!inner.ok())
        return nullptr;
      for (const SExpr *a : f.rest()) {
        MatchArm *arm = matchArm(*a);
        if (!arm)
          return nullptr;
        arms.push_back(arm);
      }
      // The check the grammar makes (docs/grammar.md).
      if (arms.empty())
        return fail(e, "a match needs at least one arm"), nullptr;
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
        const std::string *name =
            tf.optionalName("the target's name (\"\" for _)");
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

  /// Whether @p tag is a literal's, the nodes a value arm's pattern is.
  static bool literalTag(const std::string &tag) {
    for (const char *t : {"int", "float", "bool", "char", "string", "none"})
      if (tag == t)
        return true;
    return false;
  }

  MatchArm *matchArm(const SExpr &e) {
    if (e.K != SExpr::List)
      return fail(e, "expected a match arm"), nullptr;
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string *binding =
        f.optionalName("the arm's binding (\"\" for none)");
    if (!binding)
      return nullptr;
    // The checks the grammar makes (docs/grammar.md): only a type arm binds
    // the subject.
    if (!binding->empty() && e.Text != "type-arm")
      return fail(f.last(), "(" + e.Text +
                                " ...) has no binding: its "
                                "binding is \"\""),
             nullptr;
    MatchArm *arm = nullptr;
    if (e.Text == "type-arm") {
      Type *ty = type(f, "the matched type");
      ParamMode mode = ParamMode::Value;
      if (ty && !paramMode(f, mode))
        return nullptr;
      if (mode != ParamMode::Value && binding->empty())
        return fail(e, "an arm without a binding has no mode"), nullptr;
      CompoundStmt *body = ty ? block(f, "the arm's body") : nullptr;
      if (body) {
        arm = Ctx.make<MatchArm>(loc, *binding, ty, body);
        arm->setMode(mode);
      }
    } else if (e.Text == "value-arm") {
      // The check the grammar makes (docs/grammar.md): the pattern is a
      // literal, a negative number's (`(int -1)`) included.
      const SExpr *pe = f.anyList("the matched value");
      if (pe && !literalTag(pe->Text))
        return fail(*pe, "a value arm's pattern is a literal (int, float, "
                         "bool, char, string or none)"),
               nullptr;
      Expr *pattern = pe ? expr(*pe) : nullptr;
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

  /// expr(@p f, @p what) one level of nesting inside @p parent.
  Expr *nestedExpr(Fields &f, const SExpr &parent, const char *what) {
    Nested inner(*this, parent);
    return inner.ok() ? expr(f, what) : nullptr;
  }

  /// Where an operand stands: a binary operator's left or right operand, a
  /// prefix operator's, the expression a suffix applies to (a call of a
  /// function value, a field, a method call, a tuple index, a subscript, or
  /// the target of an assignment to one), or a conditional's condition.
  enum class Operand { Left, Right, Prefix, Receiver, Condition };

  /// The precedence the grammar gives @p op (docs/grammar.md section 6):
  /// 1 for `||` up to 5 for `*`, `/` and `%`; the relational operators'
  /// (kRelational) do not chain.
  static int precedence(BinaryOpcode op) {
    switch (op) {
    case BinaryOpcode::Or:
      return 1;
    case BinaryOpcode::And:
      return 2;
    case BinaryOpcode::Lt:
    case BinaryOpcode::Gt:
    case BinaryOpcode::Le:
    case BinaryOpcode::Ge:
    case BinaryOpcode::Eq:
    case BinaryOpcode::Ne:
      return kRelational;
    case BinaryOpcode::Add:
    case BinaryOpcode::Sub:
      return 4;
    case BinaryOpcode::Mul:
    case BinaryOpcode::Div:
    case BinaryOpcode::Mod:
      return 5;
    case BinaryOpcode::Count:
      break;
    }
    return 0;
  }
  static constexpr int kRelational = 3;

  /// Whether the operand @p e, at @p where of a binary operator of
  /// precedence @p prec, is one the grammar writes in parentheses there:
  /// a conditional anywhere; a binary operator's of lower precedence on the
  /// left (or a comparison's of a comparison), of the same or lower on the
  /// right, and any as the operand of a prefix operator or what a suffix
  /// applies to; a prefix operator's as what a suffix applies to; and a
  /// lambda with an expression body (`fn() => e`, (inferred)) on the left
  /// or before a suffix, where its body would take what follows.  The AST
  /// has no parentheses, but such an operand is a level of nesting, as its
  /// parenthesis is for the frontends (docs/grammar.md section 9): the AST
  /// of a program is held to the nesting its source has.
  static bool operandNests(const SExpr &e, Operand where, int prec) {
    if (e.K != SExpr::List)
      return false;
    if (e.Text == "ternary")
      return true;
    if (e.Text == "lambda")
      return (where == Operand::Left || where == Operand::Receiver) &&
             !e.Items.empty() && e.Items.back().K == SExpr::List &&
             e.Items.back().Text == "inferred";
    if (e.Text == "unary")
      return where == Operand::Receiver;
    const int q = binaryPrecedenceOf(e);
    if (!q)
      return false;
    switch (where) {
    case Operand::Left:
      return q < prec || (q == prec && prec == kRelational);
    case Operand::Right:
      return q <= prec;
    case Operand::Prefix:
    case Operand::Receiver:
      return true;
    case Operand::Condition:
      return false;
    }
    return false;
  }

  /// The precedence of the operator of the `binary` @p e, or 0 when @p e is
  /// not one with an operator the format names.
  static int binaryPrecedenceOf(const SExpr &e) {
    if (e.K != SExpr::List || e.Text != "binary")
      return 0;
    size_t at = !e.Items.empty() && e.Items[0].K == SExpr::Location ? 1 : 0;
    BinaryOpcode op{};
    if (at >= e.Items.size() || e.Items[at].K != SExpr::Symbol ||
        !binaryOp(e.Items[at].Text, op))
      return 0;
    return precedence(op);
  }

  /// expr(@p f, @p what), the operand of @p parent at @p where (of a binary
  /// operator of precedence @p prec): one level of nesting inside
  /// @p parent when the grammar writes it in parentheses (operandNests).
  Expr *operand(Fields &f, const SExpr &parent, const char *what, Operand where,
                int prec = 0) {
    if (!f.atEnd() && operandNests(*f.peekList(), where, prec))
      return nestedExpr(f, parent, what);
    return expr(f, what);
  }

  /// The remaining fields, expressions (the arguments of a call, the
  /// elements of an array or a tuple), one level of nesting inside
  /// @p parent when there are any.
  bool exprs(Fields &f, const SExpr &parent, std::vector<Expr *> &out) {
    if (f.atEnd())
      return true;
    Nested inner(*this, parent);
    if (!inner.ok())
      return false;
    while (!f.atEnd()) {
      Expr *x = expr(f, "an expression");
      if (!x)
        return false;
      out.push_back(x);
    }
    return true;
  }

  /// The type arguments in @p list into @p out, one level of nesting
  /// inside it.
  bool typeArgs(const SExpr &list, std::vector<Type *> &out) {
    Nested inner(*this, list);
    if (!inner.ok())
      return false;
    for (const SExpr &ta : list.Items) {
      if (ta.K != SExpr::List)
        return fail(ta, "expected a type"), false;
      Type *ty = type(ta);
      if (!ty)
        return false;
      out.push_back(ty);
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

  /// The list a link of a chain builds first, its operand (a `binary`'s
  /// left operand; the function value, receiver, tuple or array of a
  /// suffix), when @p e is a link with one; else null.
  /// An operand written in parentheses (operandNests) is not one: it is
  /// built inside its level of nesting, by its link.
  static const SExpr *chainOperand(const SExpr &e) {
    if (e.K != SExpr::List)
      return nullptr;
    size_t at = !e.Items.empty() && e.Items[0].K == SExpr::Location ? 1 : 0;
    const int prec = binaryPrecedenceOf(e);
    if (e.Text == "binary")
      ++at; // the operator
    else if (e.Text != "call-value" && e.Text != "member" &&
             e.Text != "method-call" && e.Text != "tuple-index" &&
             e.Text != "subscript")
      return nullptr;
    if (at >= e.Items.size() || e.Items[at].K != SExpr::List ||
        operandNests(e.Items[at], prec ? Operand::Left : Operand::Receiver,
                     prec))
      return nullptr;
    return &e.Items[at];
  }

  /// The expressions, and their chain heights, of the links of a chain
  /// below the one being built, built innermost first (buildChainBelow)
  /// and not taken yet.
  std::unordered_map<const SExpr *, std::pair<Expr *, unsigned>> ChainBuilt;

  /// When @p e is a link of a chain whose operand is a link too: build the
  /// links below @p e, innermost first, so that reading a chain of any
  /// length recurses no deeper than a few links.  Each link builds its
  /// operand first, so that is the order they are built in anyway; a link
  /// counts in the chain height of the link above it when that takes it.
  void buildChainBelow(const SExpr &e) {
    const SExpr *operand = chainOperand(e);
    if (!operand || !chainOperand(*operand) || ChainBuilt.count(operand))
      return;
    std::vector<const SExpr *> links;
    for (const SExpr *x = operand; chainOperand(*x); x = chainOperand(*x))
      links.push_back(x);
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
      const unsigned outer = std::exchange(ChainHeight, 0);
      Expr *built = node(**it);
      ChainBuilt[*it] = {built, ChainHeight};
      ChainHeight = outer;
    }
  }

  Expr *expr(const SExpr &e) {
    if (failed())
      return nullptr;
    if (!ChainBuilt.empty()) {
      auto built = ChainBuilt.find(&e);
      if (built != ChainBuilt.end()) {
        auto [x, height] = built->second;
        ChainBuilt.erase(built);
        ChainHeight = std::max(ChainHeight, height);
        return x;
      }
    }
    buildChainBelow(e);
    return node(e);
  }

  /// expr(@p e) of a link whose operand was built already, and of
  /// everything else.
  Expr *node(const SExpr &e) {
    Fields f(*this, e);
    SourceLocation loc = f.location();
    const std::string &t = e.Text;
    Expr *out = nullptr;
    if (t == "int") {
      int64_t v = 0;
      bool above = false;
      if (!f.literalInteger("the value", v, above))
        return nullptr;
      out = Ctx.make<IntegerLiteral>(loc, v, above);
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
    } else if (t == "unary") {
      const SExpr *op = f.symbol("the operator (neg or not)");
      if (!op)
        return nullptr;
      if (op->Text != "neg" && op->Text != "not")
        return fail(*op,
                    "unknown unary operator '" + op->Text + "' (neg, not)"),
               nullptr;
      Nested inner(*this, e);
      Expr *x =
          inner.ok() ? operand(f, e, "the operand", Operand::Prefix) : nullptr;
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
      Link link(*this, e);
      const int prec = precedence(bop);
      Expr *lhs = operand(f, e, "the left operand", Operand::Left, prec);
      Expr *rhs = lhs ? operand(f, e, "the right operand", Operand::Right, prec)
                      : nullptr;
      if (!rhs || !link.ok())
        return nullptr;
      out = Ctx.make<BinaryExpr>(loc, bop, lhs, rhs);
    } else if (t == "ident") {
      const std::string *name = f.name("the name");
      if (!name)
        return nullptr;
      out = Ctx.make<Identifier>(loc, *name);
    } else if (t == "call") {
      const std::string *callee = f.name("the callee's name", NameKind::Path);
      const SExpr *tl = callee ? f.list("type-args") : nullptr;
      if (!tl)
        return nullptr;
      std::vector<Type *> targs;
      if (!tl->Items.empty() && !typeArgs(*tl, targs))
        return nullptr;
      std::vector<Expr *> args;
      if (!exprs(f, e, args))
        return nullptr;
      out = Ctx.make<CallExpr>(loc, *callee, std::move(args), std::move(targs));
    } else if (t == "method-call") {
      Link link(*this, e);
      Expr *recv = operand(f, e, "the receiver", Operand::Receiver);
      const std::string *name = recv ? f.name("the method's name") : nullptr;
      std::vector<Expr *> args;
      if (!name || !exprs(f, e, args) || !link.ok())
        return nullptr;
      out = Ctx.make<MethodCallExpr>(loc, recv, *name, std::move(args));
    } else if (t == "ternary") {
      Nested inner(*this, e);
      Expr *c = inner.ok() ? operand(f, e, "the condition", Operand::Condition)
                           : nullptr;
      Expr *a = c ? expr(f, "the value if true") : nullptr;
      Expr *b = a ? expr(f, "the value if false") : nullptr;
      if (!b)
        return nullptr;
      out = Ctx.make<TernaryExpr>(loc, c, a, b);
    } else if (t == "member") {
      Link link(*this, e);
      Expr *recv = operand(f, e, "the receiver", Operand::Receiver);
      const std::string *name = recv ? f.name("the field's name") : nullptr;
      if (!name || !link.ok())
        return nullptr;
      out = Ctx.make<MemberAccessExpr>(loc, recv, *name);
    } else if (t == "array") {
      std::vector<Expr *> elems;
      if (!exprs(f, e, elems))
        return nullptr;
      out = Ctx.make<ArrayLiteralExpr>(loc, std::move(elems));
    } else if (t == "array-repeat") {
      Expr *v = nestedExpr(f, e, "the repeated value");
      int64_t n = 0;
      if (!v || !f.integer("the count", n))
        return nullptr;
      if (n < 1)
        return fail(e, "an array has at least one element"), nullptr;
      out = Ctx.make<ArrayLiteralExpr>(loc, std::vector<Expr *>{v}, n);
    } else if (t == "subscript") {
      Link link(*this, e);
      Expr *arr = operand(f, e, "the array", Operand::Receiver);
      Expr *idx = arr ? nestedExpr(f, e, "the index") : nullptr;
      if (!idx || !link.ok())
        return nullptr;
      out = Ctx.make<SubscriptExpr>(loc, arr, idx);
    } else if (t == "enum-value") {
      const std::string *en = f.name("the enum's name", NameKind::Path);
      const std::string *v = en ? f.name("the variant's name") : nullptr;
      if (!v)
        return nullptr;
      // A generic enum's (type-args TYPE...), `Option<int>::Nothing`.
      std::vector<Type *> targs;
      if (const SExpr *tl = f.optionalList("type-args")) {
        if (!typeArgs(*tl, targs))
          return nullptr;
        if (targs.empty())
          return fail(*tl, "(type-args) of an enum value needs a type"),
                 nullptr;
      }
      out = Ctx.make<EnumValueExpr>(loc, *en, *v, std::move(targs));
    } else if (t == "mov") {
      return fail(e, "(mov ...): " + std::string(frontend::kMovRemoved)),
             nullptr;
    } else if (t == "call-value") {
      Link link(*this, e);
      Expr *callee = operand(f, e, "the function value", Operand::Receiver);
      if (!callee)
        return nullptr;
      std::vector<Expr *> args;
      if (!exprs(f, e, args) || !link.ok())
        return nullptr;
      return Ctx.make<CallValueExpr>(loc, callee, std::move(args));
    } else if (t == "lambda") {
      const SExpr *pl = f.list("params");
      std::vector<Param> params;
      if (!pl || !paramList(*pl, loc, params))
        return nullptr;
      Type *ret = nullptr;
      if (!f.absent()) {
        Nested inner(*this, e);
        ret = inner.ok() ? type(f, "the result type or _") : nullptr;
        if (!ret)
          return nullptr;
      }
      CompoundStmt *body = block(f, "the lambda's body");
      if (!body)
        return nullptr;
      // `fn(x: int) => x * 2`: a trailing (inferred), no result type, and a
      // body that is one `return` of a value.
      const SExpr *inferred = f.optionalList("inferred");
      if (inferred) {
        const auto &stmts = body->getStatements();
        const auto *r =
            stmts.size() == 1 ? dyn_cast<ReturnStmt>(stmts[0]) : nullptr;
        if (!inferred->Items.empty())
          return fail(*inferred, "(inferred) takes no fields"), nullptr;
        if (ret || !r || !r->getReturnValue())
          return fail(*inferred,
                      "an inferred result needs no result type and a body "
                      "that is one return of a value"),
                 nullptr;
      }
      if (!f.done())
        return nullptr;
      auto *lambda = Ctx.make<LambdaExpr>(loc, std::move(params), ret, body);
      lambda->setInferredResult(inferred != nullptr);
      return lambda;
    } else if (t == "tuple") {
      std::vector<Expr *> elems;
      if (!exprs(f, e, elems))
        return nullptr;
      if (elems.size() < 2)
        return fail(e, "a tuple needs at least two elements"), nullptr;
      out = Ctx.make<TupleLiteralExpr>(loc, std::move(elems));
    } else if (t == "tuple-index") {
      Link link(*this, e);
      Expr *tup = operand(f, e, "the tuple", Operand::Receiver);
      int64_t idx = 0;
      if (!tup || !f.integer("the index", idx) || !link.ok())
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

TranslationUnit *read(std::string_view text, ASTContext &ctx, ReadError &error,
                      Origin origin) {
  error = ReadError();
  Lexer lexer(text);
  SExpr form;
  if (!lexer.top(form)) {
    error = lexer.Err;
    return nullptr;
  }
  Builder b(ctx, error, origin);
  TranslationUnit *tu = b.top(form);
  if (!error.Message.empty())
    return nullptr;
  return tu;
}

} // namespace paykan::ast::interchange
