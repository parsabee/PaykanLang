// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The `rd-plugin` frontend: the installation's recursive-descent parser as
// a loadable frontend plugin, for the exported test support's own test
// (tests/OutOfTree/FrontendTests.cmake).  It shows the pattern for a C++
// frontend that reuses existing code: internally it is C++ and uses the
// installed static libraries (the parser, the AST, and the AST interchange
// writer, linked into the module); across the boundary there is only the C
// interface of paykan/plugin_api.h and the AST text.

#include "DiagEngine.h"
#include "paykan/ast/Interchange.h"
#include "paykan/frontends/RecursiveDescent.h"
#include "paykan/plugin_api.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using namespace paykan;

const PaykanHost *host;

/// The diagnostics the parser reported, through the host.
void forward(PaykanSession *s, const sema::DiagEngine &diag) {
  for (const sema::Diagnostic &d : diag.getDiagnostics()) {
    uint32_t level = d.Level == sema::Diagnostic::Error ? PAYKAN_DIAG_ERROR
                     : d.Level == sema::Diagnostic::Warning
                         ? PAYKAN_DIAG_WARNING
                         : PAYKAN_DIAG_NOTE;
    host->diagnostic(s, level, nullptr,
                     static_cast<uint32_t>(d.Loc.getLineStart()),
                     static_cast<uint32_t>(d.Loc.getColumnStart()),
                     d.Message.data(), d.Message.size());
  }
}

/// @p text in memory the host releases with free_memory (std::free).
char *copy(const std::string &text) {
  auto *buf = static_cast<char *>(std::malloc(text.size() + 1));
  if (buf)
    std::memcpy(buf, text.c_str(), text.size() + 1);
  return buf;
}

int parse(void *, PaykanSession *s, const PaykanFrontendInput *in,
          PaykanFrontendOutput *out) {
  ast::ASTContext ctx;
  std::ostringstream quiet;
  sema::DiagEngine diag(quiet);
  auto r = frontend::recursive_descent::parseSource(
      ctx, std::string_view(in->source, in->source_size), &diag);
  forward(s, diag);
  out->error_count = r.ErrorCount;
  if (r.ErrorCount)
    return PAYKAN_OK;
  std::ostringstream text;
  std::string error;
  if (!ast::interchange::write(*r.Root, text, error)) {
    host->diagnostic(s, PAYKAN_DIAG_ERROR, nullptr, 0, 0, error.data(),
                     error.size());
    return PAYKAN_ERROR;
  }
  std::string ast = text.str();
  out->ast = copy(ast);
  out->ast_size = ast.size();
  return out->ast ? PAYKAN_OK : PAYKAN_ERROR;
}

int dumpTokens(void *, PaykanSession *s, const PaykanFrontendInput *in) {
  std::ostringstream os, quiet;
  sema::DiagEngine diag(quiet);
  frontend::recursive_descent::dumpTokens(
      std::string_view(in->source, in->source_size), os, &diag);
  forward(s, diag);
  std::string tokens = os.str();
  return host->write_output(s, tokens.data(), tokens.size());
}

void freeMemory(void *ptr) { std::free(ptr); }

const PaykanFrontend frontends[] = {{
    sizeof(PaykanFrontend),
    "rd-plugin",
    "the recursive-descent parser as a plugin",
    nullptr,
    &parse,
    &dumpTokens,
}};

const PaykanPlugin plugin = {
    sizeof(PaykanPlugin),
    PAYKAN_PLUGIN_API_VERSION,
    PAYKAN_PLUGIN_BUILD_VERSION,
    "rd-plugin",
    "1.0",
    &freeMemory,
    0,
    nullptr,
    1,
    frontends,
};

} // namespace

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *
paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}
