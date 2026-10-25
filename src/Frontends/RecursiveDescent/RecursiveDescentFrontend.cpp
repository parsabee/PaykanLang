// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The recursive-descent frontend plugin: adapts parseSource/dumpTokens to the
// frontend interface and registers the plugin as "recursive-descent".

#include "paykan/Frontend.h"
#include "paykan/frontends/RecursiveDescent.h"

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
                  std::ostream &os, sema::DiagEngine &diag) override {
    // The listing is complete whatever the lexical errors (the bad bytes are
    // skipped); they are reported through diag.
    recursive_descent::dumpTokens(source, os, &diag);
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
