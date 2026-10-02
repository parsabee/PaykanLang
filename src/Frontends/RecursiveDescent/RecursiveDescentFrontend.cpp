// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The recursive-descent frontend plugin: adapts parseSource/dumpTokens to the
// frontend interface and registers the plugin as "recursive-descent".

#include "Frontends/RecursiveDescent.h"
#include "paykan/Frontend.h"

#include <memory>

namespace paykan::frontend::recursive_descent {

namespace {

class RecursiveDescentFrontend : public Frontend {
public:
  std::string_view name() const override { return "recursive-descent"; }

  ParseResult parse(std::string_view /*filename*/, std::string_view source,
                    ast::ASTContext &ctx, sema::DiagEngine &diag,
                    const Options & /*opts*/) override {
    ParseOutput out = parseSource(ctx, source, &diag);
    ParseResult r;
    r.Root = out.Root;
    r.ErrorCount = out.ErrorCount;
    return r;
  }

  bool dumpTokens(std::string_view /*filename*/, std::string_view source,
                  std::ostream &os) override {
    // Lexical errors are printed to stderr by the fallback engine; the dump
    // itself is still complete (bad bytes are skipped), so it succeeds.
    recursive_descent::dumpTokens(source, os, nullptr);
    return true;
  }
};

std::unique_ptr<Frontend> createRecursiveDescentFrontend() {
  return std::make_unique<RecursiveDescentFrontend>();
}

} // namespace

} // namespace paykan::frontend::recursive_descent

PAYKAN_REGISTER_FRONTEND(
    recursive_descent, "recursive-descent",
    &paykan::frontend::recursive_descent::createRecursiveDescentFrontend);
