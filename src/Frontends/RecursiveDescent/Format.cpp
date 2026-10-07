// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The Paykan source formatter (paykan/format/Format.h).  It works on the
// recursive-descent frontend's token stream, so it reads exactly the tokens
// the compiler reads; comments are found in the gaps between tokens.

#include "paykan/format/Format.h"

#include "Lexer.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::format {

namespace {

using frontend::recursive_descent::Lexer;
using frontend::recursive_descent::Tok;
using frontend::recursive_descent::Token;

struct Comment {
  size_t Offset = 0;
  size_t Length = 0;
  size_t Line = 0;
};

struct Lexed {
  std::vector<Token> Tokens;
  std::vector<Comment> Comments;
  std::string Error; // the first lexical error, `line:col: message`
};

/// Display width: the number of UTF-8 code points.
size_t width(std::string_view s) {
  size_t n = 0;
  for (char c : s)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  return n;
}

std::string_view rtrim(std::string_view s) {
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

size_t tokenEnd(const Token &t) { return t.Offset + t.Text.size(); }

/// The tokens of @p src and the `//` comments between them.
Lexed lex(std::string_view src) {
  Lexed out;
  Lexer lexer(src, [&](ast::SourceLocation loc, const std::string &msg) {
    if (out.Error.empty())
      out.Error = std::to_string(loc.getLineStart()) + ":" +
                  std::to_string(loc.getColumnStart()) + ": " + msg;
  });
  for (Token t = lexer.next(); t.Kind != Tok::Eof; t = lexer.next())
    out.Tokens.push_back(t);
  if (!out.Error.empty())
    return out;

  // Without lexical errors a gap holds only whitespace and comments, and a
  // comment runs to the end of its line.
  size_t pos = 0;
  size_t line = 1;
  auto scanGap = [&](size_t end) {
    while (pos < end) {
      if (src[pos] == '\n') {
        ++line;
        ++pos;
      } else if (src[pos] == '/' && pos + 1 < end && src[pos + 1] == '/') {
        size_t e = src.find('\n', pos);
        if (e == std::string_view::npos || e > end)
          e = end;
        out.Comments.push_back({pos, e - pos, line});
        pos = e;
      } else {
        ++pos;
      }
    }
  };
  for (const Token &t : out.Tokens) {
    scanGap(t.Offset);
    pos = tokenEnd(t);
    line = t.Loc.getLineEnd();
  }
  scanGap(src.size());
  return out;
}

class Formatter {
public:
  Formatter(std::string_view src, const Style &style, const Lexed &lexed)
      : Src(src), S(style), Tokens(lexed.Tokens), Comments(lexed.Comments) {
    for (size_t start = 0;;) {
      size_t nl = Src.find('\n', start);
      LineStart.push_back(start);
      Lines.push_back(Src.substr(start, nl == std::string_view::npos
                                            ? std::string_view::npos
                                            : nl - start));
      if (nl == std::string_view::npos)
        break;
      start = nl + 1;
    }
    // Lines are 1-based; index 0 is unused.
    LineStart.insert(LineStart.begin(), 0);
    Lines.insert(Lines.begin(), std::string_view());
    FirstTok.assign(Lines.size(), -1);
    LastTok.assign(Lines.size(), -1);
    CommentOn.assign(Lines.size(), -1);
    for (size_t i = 0; i < Tokens.size(); ++i) {
      size_t l = lineOf(i);
      if (FirstTok[l] < 0)
        FirstTok[l] = static_cast<long>(i);
      LastTok[l] = static_cast<long>(i);
    }
    for (size_t c = 0; c < Comments.size(); ++c)
      CommentOn[Comments[c].Line] = static_cast<long>(c);
  }

  std::string run() {
    findCollapsibleFunctions();
    std::vector<std::string> out;
    Continuation cont;
    for (size_t l = 1; l < Lines.size(); ++l) {
      if (auto r = Replacements.find(l); r != Replacements.end()) {
        for (const std::string &s : r->second.Lines)
          out.push_back(s);
        l = r->second.Last;
        cont = {};
        continue;
      }
      out.push_back(formatLine(l, cont));
    }
    while (!out.empty() && out.back().empty())
      out.pop_back();
    std::string text;
    for (const std::string &s : out) {
      text += s;
      text += '\n';
    }
    return text;
  }

private:
  std::string_view Src;
  const Style &S;
  const std::vector<Token> &Tokens;
  const std::vector<Comment> &Comments;
  std::vector<size_t> LineStart;
  std::vector<std::string_view> Lines;
  std::vector<long> FirstTok, LastTok, CommentOn;

  struct Replacement {
    size_t Last = 0;
    std::vector<std::string> Lines;
  };
  std::map<size_t, Replacement> Replacements; // by first line

  /// The comment above (or the one before it in a chain) trailed code:
  /// where it started in the source and where it was put.
  struct Continuation {
    bool Valid = false;
    size_t SourceColumn = 0;
    size_t OutputColumn = 0;
  };

  size_t lineOf(size_t tok) const { return Tokens[tok].Loc.getLineStart(); }

  std::string_view commentText(const Comment &c) const {
    return Src.substr(c.Offset, c.Length);
  }

  /// 1-based display column of byte @p offset on line @p line.
  size_t columnOf(size_t line, size_t offset) const {
    return width(Src.substr(LineStart[line], offset - LineStart[line])) + 1;
  }

  /// @p left padded to the right column (at least two spaces), then @p right.
  /// Sets @p column to where @p right starts.
  std::string twoColumns(std::string_view left, std::string_view right,
                         size_t &column) const {
    size_t w = width(left);
    size_t pad = w + 3 <= S.RightColumn ? S.RightColumn - 1 - w : 2;
    column = w + pad + 1;
    std::string s(left);
    s.append(pad, ' ');
    s += right;
    return s;
  }

  std::string formatLine(size_t l, Continuation &cont) const {
    std::string_view text = Lines[l];
    if (CommentOn[l] < 0) {
      cont = {};
      return std::string(rtrim(text));
    }
    const Comment &c = Comments[static_cast<size_t>(CommentOn[l])];
    size_t sourceColumn = columnOf(l, c.Offset);
    if (FirstTok[l] >= 0) {
      std::string_view code =
          rtrim(Src.substr(LineStart[l], c.Offset - LineStart[l]));
      size_t column = 0;
      std::string s = twoColumns(code, commentText(c), column);
      cont = {true, sourceColumn, column};
      return s;
    }
    if (cont.Valid && cont.SourceColumn == sourceColumn) {
      std::string s(cont.OutputColumn - 1, ' ');
      s += commentText(c);
      return s;
    }
    cont = {};
    return std::string(rtrim(text));
  }

  /// Record a Replacement for every function or method that collapses onto
  /// one line (see Format.h).
  void findCollapsibleFunctions() {
    for (size_t i = 0; i < Tokens.size(); ++i) {
      if (Tokens[i].Kind != Tok::KwFn)
        continue;
      size_t fnLine = lineOf(i);
      if (FirstTok[fnLine] != static_cast<long>(i))
        continue;
      // The body's `{`: the first one outside parentheses and brackets, on
      // the line of `fn`.
      size_t open = i + 1;
      int depth = 0;
      for (; open < Tokens.size(); ++open) {
        Tok k = Tokens[open].Kind;
        if (k == Tok::LParen || k == Tok::LBracket)
          ++depth;
        else if (k == Tok::RParen || k == Tok::RBracket)
          --depth;
        else if ((k == Tok::LBrace && depth == 0) || k == Tok::Semi ||
                 k == Tok::RBrace || k == Tok::KwFn)
          break;
      }
      if (open >= Tokens.size() || Tokens[open].Kind != Tok::LBrace ||
          lineOf(open) != fnLine)
        continue;
      // The matching `}`, with no block nested in the body.
      size_t close = open + 1;
      while (close < Tokens.size() && Tokens[close].Kind != Tok::RBrace &&
             Tokens[close].Kind != Tok::LBrace)
        ++close;
      if (close >= Tokens.size() || Tokens[close].Kind != Tok::RBrace)
        continue;
      size_t closeLine = lineOf(close);
      if (LastTok[closeLine] != static_cast<long>(close))
        continue;
      collapse(i, open, close, fnLine, closeLine);
      i = close;
    }
  }

  /// The block form of a one-line function: `left {` (with the comment that
  /// trailed the line), the statements of its body indented two spaces, and
  /// `}`.  Spacing inside a statement is kept.
  std::vector<std::string> expand(std::string_view indent,
                                  const std::string &left, size_t open,
                                  size_t close,
                                  const std::vector<std::string_view> &moved) {
    std::vector<std::string> lines;
    std::string header = left + " {";
    if (moved.empty()) {
      lines.push_back(header);
    } else {
      size_t column = 0;
      lines.push_back(twoColumns(header, moved.front(), column));
    }
    std::string stmt;
    int depth = 0;
    for (size_t b = open + 1; b < close; ++b) {
      if (!stmt.empty())
        stmt += Src.substr(tokenEnd(Tokens[b - 1]),
                           Tokens[b].Offset - tokenEnd(Tokens[b - 1]));
      stmt += Tokens[b].Text;
      Tok k = Tokens[b].Kind;
      if (k == Tok::LParen || k == Tok::LBracket)
        ++depth;
      else if (k == Tok::RParen || k == Tok::RBracket)
        --depth;
      if ((k == Tok::Semi && depth == 0) || b + 1 == close) {
        lines.push_back(std::string(indent) + "  " + stmt);
        stmt.clear();
      }
    }
    lines.push_back(std::string(indent) + "}");
    return lines;
  }

  void collapse(size_t fn, size_t open, size_t close, size_t fnLine,
                size_t closeLine) {
    // A comment inside the body keeps the function as it is; one after the
    // header or after the closing brace moves above it.
    std::vector<std::string_view> moved;
    for (size_t l = fnLine; l <= closeLine; ++l) {
      if (CommentOn[l] < 0)
        continue;
      if (l != fnLine && l != closeLine)
        return;
      moved.push_back(commentText(Comments[static_cast<size_t>(CommentOn[l])]));
    }

    std::string body;
    for (size_t b = open + 1; b < close; ++b) {
      if (b > open + 1) {
        if (lineOf(b) == lineOf(b - 1))
          body += Src.substr(tokenEnd(Tokens[b - 1]),
                             Tokens[b].Offset - tokenEnd(Tokens[b - 1]));
        else
          body += ' ';
      }
      body += Tokens[b].Text;
    }

    std::string_view indent =
        Src.substr(LineStart[fnLine], Tokens[fn].Offset - LineStart[fnLine]);
    std::string left(indent);
    left += Src.substr(Tokens[fn].Offset,
                       tokenEnd(Tokens[open - 1]) - Tokens[fn].Offset);
    size_t column = 0;
    std::string line =
        twoColumns(left, body.empty() ? "{}" : "{ " + body + " }", column);
    Replacement r;
    r.Last = closeLine;
    if (width(line) > S.MaxWidth) {
      // Too wide: a function written on one line becomes a block, one
      // statement per line; one already written as a block stays as it is.
      if (fnLine == closeLine)
        r.Lines = expand(indent, left, open, close, moved);
      if (!r.Lines.empty())
        Replacements[fnLine] = std::move(r);
      return;
    }
    for (std::string_view c : moved)
      r.Lines.push_back(std::string(indent) + std::string(c));
    r.Lines.push_back(std::move(line));
    Replacements[fnLine] = std::move(r);
  }
};

/// Whether @p a and @p b have the same tokens and the same comments, in
/// order.
bool sameContent(std::string_view srcA, const Lexed &a, std::string_view srcB,
                 const Lexed &b) {
  if (a.Tokens.size() != b.Tokens.size() ||
      a.Comments.size() != b.Comments.size())
    return false;
  for (size_t i = 0; i < a.Tokens.size(); ++i)
    if (a.Tokens[i].Kind != b.Tokens[i].Kind ||
        a.Tokens[i].Text != b.Tokens[i].Text)
      return false;
  for (size_t i = 0; i < a.Comments.size(); ++i)
    if (srcA.substr(a.Comments[i].Offset, a.Comments[i].Length) !=
        srcB.substr(b.Comments[i].Offset, b.Comments[i].Length))
      return false;
  return true;
}

} // namespace

Result format(std::string_view source, const Style &style) {
  Result r;
  Lexed lexed = lex(source);
  if (!lexed.Error.empty()) {
    r.Error = lexed.Error;
    return r;
  }
  std::string text = Formatter(source, style, lexed).run();
  Lexed again = lex(text);
  if (!again.Error.empty() || !sameContent(source, lexed, text, again)) {
    r.Error = "internal error: formatting would change the program; the "
              "file is left as it is";
    return r;
  }
  r.Ok = true;
  r.Text = std::move(text);
  return r;
}

} // namespace paykan::format
