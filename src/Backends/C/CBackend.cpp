// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
//
// The C backend as a backend plugin: `--backend=c`, `--emit-c`, `build`,
// `run`.

#include "paykan/backends/c/CBackend.h"
#include "Names.h"
#include "paykan/Backend.h"

#include <filesystem>
#include <sstream>

namespace paykan::backend_c {

namespace {

class CBackend final : public backend::Backend {
public:
  std::string_view name() const override { return "c"; }

  backend::Capabilities capabilities() const override {
    backend::Capabilities caps;
    caps.EmitSource = true;
    caps.EmitObject = false;
    caps.EmitExecutable = true;
    caps.Run = true;
    caps.SourceExtension = ".c";
    return caps;
  }

  Status emit(const backend::Input &in, backend::EmitKind kind,
              const backend::EmitOptions &opts, std::ostream &out) override {
    if (!in.Program)
      return Status::error("the C backend needs a PIR program");
    std::ostringstream errs;
    switch (kind) {
    case backend::EmitKind::Source:
      if (!emitC(*in.Program, out, errs))
        return Status::error(errs.str());
      return Status::ok();
    case backend::EmitKind::Executable: {
      std::string output = opts.OutputPath;
      if (output.empty())
        output = std::filesystem::path(in.InputFilename).stem().string();
      if (output.empty())
        output = "a.out";
      if (!buildExecutable(*in.Program, output, toolchain(in), errs))
        return Status::error(errs.str());
      return Status::ok();
    }
    case backend::EmitKind::Object:
      break;
    }
    return Status::error("the C backend cannot emit object files");
  }

  StatusOr<int> run(const backend::Input &in, std::span<const std::string> args,
                    const backend::RunOptions &opts) override {
    if (!in.Program)
      return Status::error("the C backend needs a PIR program");
    std::ostringstream errs;
    std::vector<std::string> argv(args.begin(), args.end());
    int rc =
        buildAndRun(*in.Program, argv, opts.TrackHeap, toolchain(in), errs);
    if (rc < 0)
      return Status::error(errs.str());
    return rc;
  }

private:
  static Toolchain toolchain(const backend::Input &in) {
    Toolchain tc;
    tc.ExtraFlags.push_back("-O" +
                            std::to_string(in.OptLevel > 3 ? 3u : in.OptLevel));
    // The object cache lives next to the LLVM backend's bitcode cache.
    tc.CacheDir =
        (std::filesystem::path(in.ProjectRoot) / names::kCacheDir).string();
    return tc;
  }
};

std::unique_ptr<backend::Backend> create() {
  return std::make_unique<CBackend>();
}

} // namespace

} // namespace paykan::backend_c

PAYKAN_REGISTER_BACKEND(c, "c", &paykan::backend_c::create);
