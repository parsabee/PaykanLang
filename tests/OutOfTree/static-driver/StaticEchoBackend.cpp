// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// A static C++ backend for a driver of its own (paykan_add_driver): prints
// the PIR it receives, through the in-process C++ interface.

#include "paykan/Backend.h"
#include "paykan/pir/Printer.h"

#include <memory>
#include <ostream>

namespace {

using namespace paykan;

class StaticEchoBackend : public backend::Backend {
public:
  std::string_view name() const override { return "static-echo"; }
  backend::Capabilities capabilities() const override {
    backend::Capabilities c;
    c.EmitSource = true;
    c.SourceExtension = ".pir";
    return c;
  }
  std::string describe() const override { return "linked in statically"; }
  Status emit(const backend::Input &in, backend::EmitKind,
              const backend::EmitOptions &, std::ostream &out) override {
    pir::print(*in.Program, out);
    return Status::ok();
  }
};

std::unique_ptr<backend::Backend> create() {
  return std::make_unique<StaticEchoBackend>();
}

} // namespace

PAYKAN_REGISTER_BACKEND(static_echo, "static-echo", &create);
