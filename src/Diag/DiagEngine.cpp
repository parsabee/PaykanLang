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
// one as the kSnippetWidth characters around the caret, with kEllipsis
// where it is cut, so that a diagnostic's size does not grow with its line.
// A character is a UTF-8 code point, a tab included (printed as a tab under
// itself in the caret line too, so the caret stays under its character).
constexpr size_t kSnippetWidth = 120;
constexpr std::string_view kEllipsis = "...";

// The byte offset of each character of @p line, then line.size().
std::vector<size_t> charStarts(const std::string &line) {
  std::vector<size_t> starts;
  for (size_t i = 0; i < line.size(); ++i)
    if ((static_cast<unsigned char>(line[i]) & 0xC0) != 0x80)
      starts.push_back(i);
  starts.push_back(line.size());
  return starts;
}

// The index of the character the byte @p offset of a line is in, given its
// charStarts; past the end, the character count plus the bytes after it.
size_t charAt(const std::vector<size_t> &starts, size_t offset) {
  if (offset >= starts.back())
    return starts.size() - 1 + (offset - starts.back());
  return static_cast<size_t>(
             std::upper_bound(starts.begin(), starts.end(), offset) -
             starts.begin()) -
         1;
}

// Characters [first, first + kSnippetWidth) of @p line, an ellipsis where
// characters before or after them are left out.
void printWindow(std::ostream &os, const std::string &line, size_t first) {
  const std::vector<size_t> starts = charStarts(line);
  const size_t count = starts.size() - 1;
  const size_t from = std::min(first, count);
  const size_t to = std::min(first + kSnippetWidth, count);
  if (from > 0)
    os << kEllipsis;
  os << std::string_view(line).substr(starts[from], starts[to] - starts[from]);
  if (to < count)
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
         << " errors, the error limit (--error-limit=N sets it, and "
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

  // The characters of the caret's line around it, the same for the lines
  // around it.
  const std::string &line = (*SourceLines)[lineNo - 1];
  const std::vector<size_t> starts = charStarts(line);
  const size_t count = starts.size() - 1;
  const size_t startByte = std::max<size_t>(1, loc.getColumnStart()) - 1;
  const size_t caret = charAt(starts, startByte);
  size_t first = 0;
  if (count > kSnippetWidth && caret > kSnippetWidth / 2)
    first = std::min(caret - kSnippetWidth / 2, count - kSnippetWidth);

  auto printLine = [&](size_t no) {
    const std::string noStr = std::to_string(no);
    OS << std::string(gutterWidth - noStr.size(), ' ') << noStr << " | ";
    printWindow(OS, (*SourceLines)[no - 1], first);
    OS << "\n";
  };

  if (prevLineNo < lineNo)
    printLine(prevLineNo);
  printLine(lineNo);

  // The underline covers the location's characters on this line, up to the
  // window's end.
  size_t width = 1;
  if (loc.getLineEnd() == loc.getLineStart() &&
      loc.getColumnEnd() > loc.getColumnStart())
    width = charAt(starts, loc.getColumnEnd() - 1) - caret;
  const size_t room =
      first + kSnippetWidth > caret ? first + kSnippetWidth - caret : 1;
  width = std::clamp<size_t>(width, 1, room);

  std::string marker = std::string(gutterWidth, ' ') + " | ";
  if (first > 0)
    marker.append(kEllipsis.size(), ' ');
  for (size_t c = first; c < caret; ++c)
    marker.push_back(c < count && line[starts[c]] == '\t' ? '\t' : ' ');
  marker.push_back('^');
  if (width > 1)
    marker.append(width - 1, '~');
  OS << marker << "\n";

  if (nextLineNo > lineNo)
    printLine(nextLineNo);
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
