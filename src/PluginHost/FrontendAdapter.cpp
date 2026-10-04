// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// A loaded plugin's C frontend (PaykanFrontend, paykan/plugin_api.h) as a
// frontend::Frontend.  The plugin returns the program as text in the AST
// interchange format (docs/plugins/ast-format.md); the adapter reads it into
// the parse's ASTContext, checking it as it goes, so what reaches Sema is an
// AST like the built-in frontend's.

#include "PluginHost.h"

#include "paykan/ast/Interchange.h"

#include <iostream>
#include <string>

static_assert(PAYKAN_AST_FORMAT_VERSION ==
                  paykan::ast::interchange::kFormatVersion,
              "plugin_api.h and paykan/ast/Interchange.h disagree on the AST "
              "format version");

namespace paykan::plugin::host {

namespace {

class FrontendAdapter : public frontend::Frontend {
public:
  FrontendAdapter(const PaykanPlugin *plugin, const PaykanFrontend *f)
      : Plugin(plugin), F(f) {}

  std::string_view name() const override { return F->name; }

  frontend::ParseResult parse(std::string_view filename,
                              std::string_view source, ast::ASTContext &ctx,
                              sema::DiagEngine &diag,
                              const frontend::Options &opts) override {
    std::string file(filename), text(source);
    PaykanSession s;
    s.Out = &std::cerr; // debug traces
    s.InputFile = file;
    s.Owner = owner();
    s.Diag = &diag;
    PaykanFrontendInput in = input(file, text, opts);
    PaykanFrontendOutput out{};
    out.struct_size = sizeof(PaykanFrontendOutput);
    int rc = F->parse(F->data, &s, &in, &out);
    std::cerr.flush();

    frontend::ParseResult result;
    std::string ast(out.ast ? std::string(out.ast, out.ast_size) : "");
    if (out.ast)
      Plugin->free_memory(out.ast);
    unsigned errors =
        out.error_count > s.DiagErrors ? out.error_count : s.DiagErrors;
    if (rc != PAYKAN_OK || errors) {
      if (!errors) {
        diag.error(ast::SourceLocation(),
                   owner() + " failed (status " + std::to_string(rc) + ")");
        errors = 1;
      }
      result.ErrorCount = errors;
      return result;
    }
    if (!out.ast) {
      diag.error(ast::SourceLocation(),
                 owner() + " returned no AST and reported no error");
      result.ErrorCount = 1;
      return result;
    }
    ast::interchange::ReadError err;
    result.Root = ast::interchange::read(ast, ctx, err);
    if (!result.Root) {
      diag.error(ast::SourceLocation(),
                 owner() + " returned an invalid AST: " + err.str());
      result.ErrorCount = 1;
    }
    return result;
  }

  bool dumpTokens(std::string_view filename, std::string_view source,
                  std::ostream &os) override {
    if (!F->dump_tokens)
      return false;
    std::string file(filename), text(source);
    sema::DiagEngine diag(std::cerr);
    PaykanSession s;
    s.Out = &os;
    s.InputFile = file;
    s.Owner = owner();
    s.Diag = &diag;
    PaykanFrontendInput in = input(file, text, frontend::Options());
    int rc = F->dump_tokens(F->data, &s, &in);
    os.flush();
    if (rc != PAYKAN_OK && !s.DiagErrors)
      diag.error(ast::SourceLocation(),
                 owner() + " failed (status " + std::to_string(rc) + ")");
    // Supported: the listing (and any errors) were written.
    return true;
  }

private:
  std::string owner() const {
    return "frontend '" + std::string(F->name) + "'";
  }

  static PaykanFrontendInput input(const std::string &file,
                                   const std::string &text,
                                   const frontend::Options &opts) {
    PaykanFrontendInput in{};
    in.struct_size = sizeof(PaykanFrontendInput);
    in.filename = file.c_str();
    in.source = text.c_str();
    in.source_size = text.size();
    in.ast_format_version = PAYKAN_AST_FORMAT_VERSION;
    in.max_nesting = frontend::kMaxNesting;
    in.trace_parsing = opts.TraceParsing ? 1u : 0u;
    in.trace_scanning = opts.TraceScanning ? 1u : 0u;
    return in;
  }

  const PaykanPlugin *Plugin;
  const PaykanFrontend *F;
};

} // namespace

std::unique_ptr<frontend::Frontend>
makeFrontendAdapter(const PaykanPlugin *plugin, const PaykanFrontend *f) {
  return std::make_unique<FrontendAdapter>(plugin, f);
}

} // namespace paykan::plugin::host
