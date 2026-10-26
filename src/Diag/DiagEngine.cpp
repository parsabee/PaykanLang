// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
//
// The diagnostic engine (DiagEngine.h): formatting with the source snippet
// (a window of a long line), the error limit, and the debug-build guard
// against reporting one diagnostic twice.

#include "DiagEngine.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace paykan {
namespace sema {

void DiagEngine::closeGroup() {
#ifndef NDEBUG
  if (!CurrentGroup.empty() && !SeenGroups.insert(CurrentGroup).second) {
    std::cerr << "internal compiler error: the same diagnostic was reported "
                 "twice (a construct was resolved and reported more than "
                 "once):\n"
              << CurrentGroup;
    std::abort();
  }
#endif
  CurrentGroup.clear();
}

DiagEngine::~DiagEngine() { closeGroup(); }

namespace {

// A source line of up to kSnippetWidth characters is shown whole; a longer
// one as kSnippetWidth characters around the caret, a kEllipsis among them
// standing for the characters cut before or after, so that a diagnostic's
// size does not grow with its line.  Only the window and the few
// characters around it are looked at, never the rest of the line, so the
// cost does not grow with it either.  A character is a UTF-8 code point, a
// tab one too (printed as a tab under itself in the caret line, so the
// caret stays under its character), and so is each byte that is in no code
// point (invalid UTF-8, which a terminal shows as one character a byte).
constexpr size_t kSnippetWidth = 120;
constexpr std::string_view kEllipsis = "...";

// The length in bytes of the code point the byte @p lead starts: 1 for an
// ASCII byte and for a byte that starts none.
size_t sequenceLength(unsigned char lead) {
  if ((lead & 0xE0) == 0xC0)
    return 2;
  if ((lead & 0xF0) == 0xE0)
    return 3;
  if ((lead & 0xF8) == 0xF0)
    return 4;
  return 1;
}

// Whether byte @p i of @p line starts a character: any byte but a
// continuation byte (10xxxxxx) of the code point a lead byte before it
// starts.  It is decided by the three bytes before it, so the line can be
// walked either way from any byte.
bool startsChar(std::string_view line, size_t i) {
  if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80)
    return true;
  for (size_t back = 1; back <= 3 && back <= i; ++back) {
    const auto b = static_cast<unsigned char>(line[i - back]);
    if ((b & 0xC0) != 0x80)
      return sequenceLength(b) <= back;
  }
  return true;
}

// The character start @p n characters after the one at @p at, or the
// line's end.
size_t forward(std::string_view line, size_t at, size_t n) {
  for (; n > 0 && at < line.size(); --n) {
    ++at;
    while (at < line.size() && !startsChar(line, at))
      ++at;
  }
  return at;
}

// The character start @p n characters before the one at @p at, or 0.
size_t backward(std::string_view line, size_t at, size_t n) {
  for (; n > 0 && at > 0; --n) {
    --at;
    while (at > 0 && !startsChar(line, at))
      --at;
  }
  return at;
}

// What a snippet shows of a line: its bytes [From, To), each at a
// character's start (or the end), after an ellipsis when characters come
// before From and before one when characters come after To.
struct Window {
  size_t From = 0;
  size_t To = 0;
};

// The window of @p line from the character at byte @p first: as many
// characters as fit in kSnippetWidth with the ellipses.
Window windowFrom(std::string_view line, size_t first) {
  const size_t room = kSnippetWidth - (first > 0 ? kEllipsis.size() : 0);
  const size_t to = forward(line, first, room);
  if (to == line.size())
    return {.From = first, .To = to};
  return {.From = first, .To = forward(line, first, room - kEllipsis.size())};
}

// Whether @p line has at most kSnippetWidth characters.
bool fits(std::string_view line) {
  return line.size() <= kSnippetWidth ||
         forward(line, 0, kSnippetWidth) == line.size();
}

// The window of @p line around the character at byte @p caret: the whole
// line if it fits, else kSnippetWidth characters with the ellipses, the
// caret at most half of them from the start.
Window around(std::string_view line, size_t caret) {
  if (fits(line))
    return {.From = 0, .To = line.size()};
  constexpr size_t half = kSnippetWidth / 2;
  const size_t first = backward(line, caret, half);
  // Near the end: the last characters, and the cut before them.
  if (first > 0 &&
      forward(line, caret, kSnippetWidth - kEllipsis.size() - half) ==
          line.size())
    return {.From =
                backward(line, line.size(), kSnippetWidth - kEllipsis.size()),
            .To = line.size()};
  return windowFrom(line, first);
}

// The window of @p line, above or below the caret's line, beside @p w, the
// caret line's: the whole line if it fits, else the characters from w's
// first byte on (the line's last ones if it ends before that byte).
Window beside(std::string_view line, const Window &w) {
  if (fits(line))
    return {.From = 0, .To = line.size()};
  if (w.From >= line.size())
    return around(line, line.size());
  size_t first = w.From;
  while (first < line.size() && !startsChar(line, first))
    ++first;
  return windowFrom(line, first);
}

void printWindow(std::ostream &os, std::string_view line, const Window &w) {
  if (w.From > 0)
    os << kEllipsis;
  os << line.substr(w.From, w.To - w.From);
  if (w.To < line.size())
    os << kEllipsis;
}

} // namespace

void DiagEngine::emit(Diagnostic::Severity level, ast::SourceLocation loc,
                      const std::string &msg) {
#ifndef NDEBUG
  // Only a group whose error or warning has a location is checked, and only
  // while the locations are exact (DiagEngine.h).
  if (level != Diagnostic::Note) {
    closeGroup();
    CheckGroup = loc.isValid();
  }
  if (CheckGroup && ExactLocations)
    CurrentGroup += SourceName + ":" + std::to_string(loc.getLineStart()) +
                    ":" + std::to_string(loc.getColumnStart()) + "-" +
                    std::to_string(loc.getLineEnd()) + ":" +
                    std::to_string(loc.getColumnEnd()) + ": " +
                    std::to_string(static_cast<int>(level)) + ": " + msg + "\n";
#endif
  Diagnostics.push_back({level, loc, msg});
  if (level == Diagnostic::Error)
    ++ErrorCount;

  // Past the error limit nothing is printed; the error after the last one
  // it lets out says so instead.
  if (Limit->Reached)
    return;
  if (level == Diagnostic::Error) {
    if (Limit->Max != 0 && Limit->Printed == Limit->Max) {
      Limit->Reached = true;
      OS << "note: stopping after " << Limit->Max
         << (Limit->Max == 1 ? " error" : " errors")
         << ", the error limit (--error-limit=N sets it, and "
            "--error-limit=0 shows every error)\n";
      return;
    }
    ++Limit->Printed;
  }

  if (loc.isValid()) {
    if (!SourceName.empty())
      OS << SourceName << ":";
    OS << loc.getLineStart() << ":" << loc.getColumnStart() << ": ";
  }

  switch (level) {
  case Diagnostic::Error:
    OS << "error: ";
    break;
  case Diagnostic::Warning:
    OS << "warning: ";
    break;
  case Diagnostic::Note:
    OS << "note: ";
    break;
  }
  OS << msg << "\n";

  if (!loc.isValid() || !SourceLines)
    return;

  size_t lineNo = loc.getLineStart();
  if (lineNo == 0 || lineNo > SourceLines->size())
    return;

  size_t prevLineNo = lineNo > 1 ? lineNo - 1 : lineNo;
  size_t nextLineNo = std::min(lineNo + 1, SourceLines->size());
  size_t gutterWidth = std::max<size_t>(3, std::to_string(nextLineNo).size());

  // The window of the caret's line around the caret, and the lines around
  // it beside it.  A caret past the line's end (a column of another file
  // than the line's, as a frontend plugin may give) stands after its last
  // character.
  const std::string_view line = (*SourceLines)[lineNo - 1];
  size_t caret = std::min<size_t>(std::max<size_t>(1, loc.getColumnStart()) - 1,
                                  line.size());
  while (caret > 0 && caret < line.size() && !startsChar(line, caret))
    --caret;
  const Window window = around(line, caret);

  auto printLine = [&](size_t no, const Window &w) {
    const std::string noStr = std::to_string(no);
    OS << std::string(gutterWidth - noStr.size(), ' ') << noStr << " | ";
    printWindow(OS, (*SourceLines)[no - 1], w);
    OS << "\n";
  };
  auto printNeighbour = [&](size_t no) {
    printLine(no, beside((*SourceLines)[no - 1], window));
  };

  if (prevLineNo < lineNo)
    printNeighbour(prevLineNo);
  printLine(lineNo, window);

  // The caret under its character, and the underline over the location's
  // characters on this line, up to the window's end.
  std::string marker = std::string(gutterWidth, ' ') + " | ";
  if (window.From > 0)
    marker.append(kEllipsis.size(), ' ');
  for (size_t at = window.From; at < caret; at = forward(line, at, 1))
    marker.push_back(line[at] == '\t' ? '\t' : ' ');
  marker.push_back('^');
  if (loc.getLineEnd() == loc.getLineStart() &&
      loc.getColumnEnd() > loc.getColumnStart()) {
    const size_t end = std::min<size_t>(loc.getColumnEnd() - 1, window.To);
    for (size_t at = forward(line, caret, 1); at < end;
         at = forward(line, at, 1))
      marker.push_back('~');
  }
  OS << marker << "\n";

  if (nextLineNo > lineNo)
    printNeighbour(nextLineNo);
}

void DiagEngine::error(ast::SourceLocation loc, const std::string &msg) {
  emit(Diagnostic::Error, loc, msg);
}

void DiagEngine::warning(ast::SourceLocation loc, const std::string &msg) {
  emit(Diagnostic::Warning, loc, msg);
}

void DiagEngine::note(ast::SourceLocation loc, const std::string &msg) {
  emit(Diagnostic::Note, loc, msg);
}

} // namespace sema
} // namespace paykan
