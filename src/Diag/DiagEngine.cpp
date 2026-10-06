// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The diagnostic engine (DiagEngine.h): formatting with the source snippet,
// and the debug-build guard against reporting one diagnostic twice.

#include "DiagEngine.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <ostream>
#include <string>

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

void DiagEngine::emit(Diagnostic::Severity level, ast::SourceLocation loc,
                      const std::string &msg) {
#ifndef NDEBUG
  if (level != Diagnostic::Note)
    closeGroup();
  CurrentGroup += SourceName + ":" + std::to_string(loc.getLineStart()) + ":" +
                  std::to_string(loc.getColumnStart()) + "-" +
                  std::to_string(loc.getLineEnd()) + ":" +
                  std::to_string(loc.getColumnEnd()) + ": " +
                  std::to_string(static_cast<int>(level)) + ": " + msg + "\n";
#endif
  Diagnostics.push_back({level, loc, msg});
  if (level == Diagnostic::Error)
    ++ErrorCount;

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

  if (prevLineNo < lineNo) {
    const std::string &prevLine = (*SourceLines)[prevLineNo - 1];
    std::string prevNoStr = std::to_string(prevLineNo);
    OS << std::string(gutterWidth - prevNoStr.size(), ' ') << prevNoStr << " | "
       << prevLine << "\n";
  }

  const std::string &line = (*SourceLines)[lineNo - 1];
  std::string lineNoStr = std::to_string(lineNo);
  OS << std::string(gutterWidth - lineNoStr.size(), ' ') << lineNoStr << " | "
     << line << "\n";

  size_t startCol = std::max<size_t>(1, loc.getColumnStart());
  size_t endCol = std::max(startCol, loc.getColumnEnd());
  size_t width = 1;
  if (loc.getLineEnd() == loc.getLineStart() && endCol > startCol)
    width = endCol - startCol;

  std::string marker = std::string(gutterWidth, ' ') + " | ";
  for (size_t i = 1; i < startCol; ++i) {
    if (i - 1 < line.size() && line[i - 1] == '\t')
      marker.push_back('\t');
    else
      marker.push_back(' ');
  }
  marker.push_back('^');
  if (width > 1)
    marker.append(width - 1, '~');
  OS << marker << "\n";

  if (nextLineNo > lineNo) {
    const std::string &nextLine = (*SourceLines)[nextLineNo - 1];
    std::string nextNoStr = std::to_string(nextLineNo);
    OS << std::string(gutterWidth - nextNoStr.size(), ' ') << nextNoStr << " | "
       << nextLine << "\n";
  }
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
