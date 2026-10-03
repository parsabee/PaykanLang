// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// AST -> PIR lowering: the single home of Paykan's ownership semantics
// (ARC placement, scope cleanup, `mov`, `match` dispatch, vtables).  Every
// backend consumes the pir::Program this produces; none re-derives ownership.
//
// Standard C++ only.

#pragma once

#include "Sema.h"
#include "paykan/pir/PIR.h"

#include <ostream>
#include <string>

namespace paykan::lowering {

/// Lower the analysed translation unit @p tu (whose SemaContext is @p ctx)
/// and every module it transitively imports into @p out.  The main module,
/// read from @p mainFile, is first.  Every module is named by its canonical
/// module name (ModuleName.h): the main module by @p mainFile's stem, an
/// import by its module path -- never by a file path, so the program is the
/// same wherever its sources live.  Diagnostics for internal
/// errors (states Sema should have ruled out) are written to @p errs; returns
/// false when any occurred.
bool lowerProgram(const sema::SemaContext &ctx, ast::TranslationUnit *tu,
                  const std::string &mainFile, const std::string &projectRoot,
                  pir::Program &out, std::ostream &errs);

} // namespace paykan::lowering
