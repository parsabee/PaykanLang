// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// A loaded plugin's C backend (PaykanBackend, paykan/plugin_api.h) as a
// backend::Backend: the driver selects and drives it exactly like a
// built-in one.  The program crosses the boundary as PIR text
// (docs/pir.md), printed from the verified pir::Program.

#include "PluginHost.h"

#include "paykan/pir/Printer.h"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace paykan::plugin::host {

namespace {

class BackendAdapter : public backend::Backend {
public:
  explicit BackendAdapter(const PaykanBackend *b) : B(b) {}

  std::string_view name() const override { return B->name; }

  backend::Capabilities capabilities() const override {
    backend::Capabilities c;
    c.EmitSource = B->capabilities & PAYKAN_BACKEND_EMIT_SOURCE;
    c.EmitObject = B->capabilities & PAYKAN_BACKEND_EMIT_OBJECT;
    c.EmitExecutable = B->capabilities & PAYKAN_BACKEND_EMIT_EXECUTABLE;
    c.Run = (B->capabilities & PAYKAN_BACKEND_RUN) && B->run;
    c.SourceExtension = B->source_extension ? B->source_extension : "";
    return c;
  }

  std::string describe() const override {
    return B->description ? B->description : "";
  }

  Status emit(const backend::Input &in, backend::EmitKind kind,
              const backend::EmitOptions &opts, std::ostream &out) override {
    Call call(*this, in, out);
    if (!call.Ok)
      return call.failure(PAYKAN_ERROR);
    uint32_t k = PAYKAN_EMIT_SOURCE;
    std::string output;
    switch (kind) {
    case backend::EmitKind::Source:
      break;
    case backend::EmitKind::Object:
      k = PAYKAN_EMIT_OBJECT;
      output = defaultOutput(in, opts, ".o");
      break;
    case backend::EmitKind::Executable:
      k = PAYKAN_EMIT_EXECUTABLE;
      output = defaultOutput(in, opts, "");
      break;
    }
    int rc = B->emit(B->data, &call.Session, &call.Input, k,
                     output.empty() ? nullptr : output.c_str());
    out.flush();
    return rc == PAYKAN_OK ? Status::ok() : call.failure(rc);
  }

  StatusOr<int> run(const backend::Input &in, std::span<const std::string> args,
                    const backend::RunOptions &opts) override {
    if (!B->run)
      return Backend::run(in, args, opts);
    Call call(*this, in, std::cout);
    if (!call.Ok)
      return call.failure(PAYKAN_ERROR);
    std::vector<const char *> argv;
    argv.reserve(args.size());
    for (const std::string &a : args)
      argv.push_back(a.c_str());
    PaykanRunRequest req{};
    req.struct_size = sizeof(PaykanRunRequest);
    req.args = argv.data();
    req.num_args = argv.size();
    req.track_heap = opts.TrackHeap ? 1u : 0u;
    int exitCode = 0;
    int rc = B->run(B->data, &call.Session, &call.Input, &req, &exitCode);
    std::cout.flush();
    if (rc != PAYKAN_OK)
      return call.failure(rc);
    return exitCode;
  }

private:
  /// The state of one callback: the PIR text, the input struct and the
  /// session.
  struct Call {
    Call(const BackendAdapter &self, const backend::Input &in,
         std::ostream &out) {
      Session.Out = &out;
      Session.InputFile = in.InputFilename;
      Session.Owner = "backend '" + std::string(self.B->name) + "'";
      Session.OptLevel = in.OptLevel;
      if (!in.Program) {
        Session.Errors.push_back(Session.Owner + ": error: no PIR program");
        Ok = false;
        return;
      }
      std::ostringstream pir;
      pir::print(*in.Program, pir);
      Pir = pir.str();
      Input.struct_size = sizeof(PaykanBackendInput);
      Input.input_filename = in.InputFilename.c_str();
      ProjectRoot = in.ProjectRoot;
      Input.project_root = ProjectRoot.c_str();
      Input.pir = Pir.c_str();
      Input.pir_size = Pir.size();
      Input.pir_text_version = PAYKAN_PIR_TEXT_VERSION;
      Input.opt_level = in.OptLevel;
    }

    /// The failure of a call that returned @p rc: its error diagnostics,
    /// or a generic message when it reported none.
    Status failure(int rc) const {
      if (Session.Errors.empty())
        return Status::error(Session.Owner + " failed (status " +
                             std::to_string(rc) + ")");
      std::string msg;
      for (const std::string &e : Session.Errors) {
        if (!msg.empty())
          msg += "\n";
        msg += e;
      }
      return Status::error(msg);
    }

    PaykanSession Session;
    PaykanBackendInput Input{};
    std::string Pir;
    std::string ProjectRoot;
    bool Ok = true;
  };

  /// -o, or what the built-in backends default to: the input's stem (plus
  /// @p ext), "a.out" for an executable without one.
  static std::string defaultOutput(const backend::Input &in,
                                   const backend::EmitOptions &opts,
                                   const char *ext) {
    if (!opts.OutputPath.empty())
      return opts.OutputPath;
    std::string stem = std::filesystem::path(in.InputFilename).stem().string();
    if (stem.empty())
      return *ext ? std::string("a") + ext : std::string("a.out");
    return stem + ext;
  }

  const PaykanBackend *B;
};

} // namespace

std::unique_ptr<backend::Backend> makeBackendAdapter(const PaykanBackend *b) {
  return std::make_unique<BackendAdapter>(b);
}

} // namespace paykan::plugin::host
