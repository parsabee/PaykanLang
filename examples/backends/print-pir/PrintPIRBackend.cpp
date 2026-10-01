// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The `print-pir` backend: the smallest possible backend, as a template for
// writing your own (docs/writing-a-backend.md).  Its "source output" is the
// PIR text of the program, so
//
//   paykan-print-pir --backend=print-pir --emit-source program.pkn
//
// prints what every backend receives after the lowering and the verifier.

#include "paykan/Backend.h"
#include "paykan/pir/Printer.h"

#include <memory>
#include <ostream>

namespace {

using namespace paykan;

class PrintPIRBackend : public backend::Backend {
public:
  // The name users select the backend by: --backend=print-pir.
  std::string_view name() const override { return "print-pir"; }

  // What the driver may ask for.  This backend only emits "source"; it
  // cannot build an executable or run the program, so `paykan build` and
  // `paykan run` are refused by the driver before reaching it.
  backend::Capabilities capabilities() const override {
    backend::Capabilities c;
    c.EmitSource = true;
    c.SourceExtension = ".pir";
    return c;
  }

  // Shown by --version and --list-backends.
  std::string describe() const override { return "prints the Paykan IR"; }

  // The program arrives in `in.Program`, lowered and verified: every module
  // of the program, main module first (see docs/pir.md).  A backend walks
  // the modules, classes and functions and translates each PIR instruction
  // one-to-one; here the PIR printer does the walking.
  Status emit(const backend::Input &in, backend::EmitKind kind,
              const backend::EmitOptions &, std::ostream &out) override {
    if (kind != backend::EmitKind::Source)
      return Status::error("print-pir only emits source");
    if (!in.Program)
      return Status::error("print-pir needs the PIR program");
    pir::print(*in.Program, out);
    return Status::ok();
  }

  // run() keeps the base class's default (an error): capabilities().Run is
  // false, so the driver never calls it.
};

std::unique_ptr<backend::Backend> createPrintPIRBackend() {
  return std::make_unique<PrintPIRBackend>();
}

} // namespace

// Static registration.  The library is linked whole into the driver
// (paykan_add_driver / paykan_add_plugin), so this runs before main().
PAYKAN_REGISTER_BACKEND(print_pir, "print-pir", &createPrintPIRBackend);
