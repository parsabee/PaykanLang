// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The handwritten frontend plugin: adapts parseSource/dumpTokens to the
// frontend interface and registers the plugin as "handwritten".

#include "Frontends/Handwritten.h"
#include "paykan/Frontend.h"

#include <memory>

namespace paykan::frontend::handwritten {

namespace {

class HandwrittenFrontend : public Frontend {
public:
  std::string_view name() const override { return "handwritten"; }

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
    handwritten::dumpTokens(source, os, nullptr);
    return true;
  }
};

std::unique_ptr<Frontend> createHandwrittenFrontend() {
  return std::make_unique<HandwrittenFrontend>();
}

} // namespace

} // namespace paykan::frontend::handwritten

PAYKAN_REGISTER_FRONTEND(
    handwritten, "handwritten",
    &paykan::frontend::handwritten::createHandwrittenFrontend);
