// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The backend interface: a compiled program -> source / object / executable,
// or an in-process run.
//
// A backend translates one program into some artifact.  Everything in front
// of it is shared: the frontends produce the AST, Sema types it, and the
// lowering turns it into PIR (paykan/pir/PIR.h, docs/pir.md), which is what a
// backend consumes.  Backends are plugins (paykan/Registry.h): a backend
// library implements Backend and registers a factory with
// PAYKAN_REGISTER_BACKEND; the driver selects one with --backend=<name>.
//
// The interface uses Status / StatusOr (paykan/Status.h), never exceptions
// and never llvm::Expected: the core is standard C++ only.

#pragma once

#include "paykan/Registry.h"
#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paykan::backend {

/// What a backend can produce.  The driver refuses a request the backend
/// does not support before calling it.
struct Capabilities {
  bool EmitSource = false;     // emit(EmitKind::Source): C, LLVM IR, ...
  bool EmitObject = false;     // emit(EmitKind::Object)
  bool EmitExecutable = false; // emit(EmitKind::Executable): `paykan build`
  bool Run = false;            // run(): `paykan run`, in-process or not
  /// compileModule(): a module's code compiled when its `.pkm` is written,
  /// and linked from Input::Compiled.
  bool CompileModules = false;
  /// File extension of the backend's source output (".c", ".ll"), used for
  /// default output names and --help.
  std::string_view SourceExtension;
};

enum class EmitKind { Source, Object, Executable };

/// A module's code compiled by a backend when the module's `.pkm` was
/// written (its OBJ section, docs/design/pkm.md §5.13).  The module's PIR
/// then declares it: compiled declarations, each with what its code uses
/// (docs/pir.md §4).
struct ModuleObject {
  /// What made the bytes: the backend's registered name, their format
  /// ("llvm-bc"), the target they are for and the backend's version (its
  /// describe()).  A backend links only what it made (objectKind()).
  std::string Backend, Format, Target, Producer;
  std::vector<uint8_t> Bytes;

  bool sameKind(const ModuleObject &o) const {
    return Backend == o.Backend && Format == o.Format && Target == o.Target &&
           Producer == o.Producer;
  }
};

/// The program a backend works on.
struct Input {
  /// The main source file, as given on the command line.
  std::string InputFilename;
  /// The project root imports were resolved against (the main file's
  /// directory; "" for the current directory).
  std::string ProjectRoot;
  /// The verified PIR of the whole program (main module first).
  const pir::Program *Program = nullptr;
  /// -O<n>, 0..3.
  unsigned OptLevel = 0;
  /// --object=<files>: object files (or archives) that define the
  /// program's native functions (#198).  Every native backend links them;
  /// the user builds them however they like.
  std::vector<std::string> Objects;
  /// The compiled code of the program's modules whose PIR declares it, by
  /// module name.  A backend with CompileModules links what the program
  /// uses of it; any other gets none (the driver gives it modules lowered
  /// from source or loaded from a `.pkm` without compiled code).
  std::map<std::string, const ModuleObject *> Compiled;
};

struct EmitOptions {
  /// Output file for Object / Executable; "" means a default next to the
  /// input.  Source output goes to the stream emit() is given.
  std::string OutputPath;
  /// For Source: which of the backend's source forms to write ("" for its
  /// default), for a backend with several (the mlir backend's stages,
  /// --emit-mlir=<stage>).
  std::string SourceStage;
};

struct RunOptions {
  /// Track runtime heap allocations and dump statistics (incl. leaks) at
  /// exit (--track-heap).  An in-process backend switches the runtime's
  /// tracking allocator on before the program allocates; a backend that runs
  /// a separate process passes the request on to it.
  bool TrackHeap = false;
};

class Backend {
public:
  virtual ~Backend() = default;

  /// The name the backend is registered under ("mlir", ...).
  virtual std::string_view name() const = 0;

  virtual Capabilities capabilities() const = 0;

  /// A one-line description for --version / --list-backends, typically the
  /// toolchain the backend is built on ("LLVM 22.1.8"); "" for none.
  virtual std::string describe() const { return {}; }

  /// Translate the program into the requested artifact.  Source output is
  /// written to @p out; Object / Executable output to opts.OutputPath.
  virtual Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
                      std::ostream &out) = 0;

  /// Execute the program and return its exit code.  @p args are the
  /// program's arguments, args[0] being the script path.  Only called when
  /// capabilities().Run is set; the default fails.
  virtual StatusOr<int> run(const Input &in, std::span<const std::string> args,
                            const RunOptions &opts);

  /// What compileModule makes (no bytes): the only compiled code this
  /// backend links.  Only called when capabilities().CompileModules is set.
  virtual ModuleObject objectKind() const { return {}; }

  /// The code of @p module, one verified PIR module whose other modules'
  /// items (and its own shared copies of instantiations) are declared, for
  /// a program that has the module to link.  Only called when
  /// capabilities().CompileModules is set; the default fails.
  virtual StatusOr<ModuleObject> compileModule(const pir::Module &module);

  /// Write @p code, one module's compiled code, as a native object at
  /// @p path, optimised at -O@p optLevel: what a program's C (--emit-c) is
  /// linked with (`paykan pkm extract --object`).  Only called when
  /// capabilities().CompileModules is set; the default fails.
  virtual Status emitModuleObject(const ModuleObject &code, unsigned optLevel,
                                  const std::string &path);
};

using Registry = plugin::Registry<Backend>;

/// The backend used when none is named: PAYKAN_DEFAULT_BACKEND from the build
/// configuration ("" when the build has no backend).
std::string_view defaultBackend();

} // namespace paykan::backend

/// Registers a backend plugin.  @p ID is an identifier unique within the
/// plugin library, @p NAME the name users select it by, @p FACTORY a
/// `std::unique_ptr<paykan::backend::Backend> (*)()`.  The registration
/// records PAYKAN_PLUGIN_BUILD_VERSION, the PaykanLang version of the headers
/// the plugin is compiled against, and the registry accepts the plugin only
/// if the running paykan lists that version as compatible (paykan/Registry.h).
#define PAYKAN_REGISTER_BACKEND(ID, NAME, FACTORY)                             \
  static const ::paykan::plugin::Registration<::paykan::backend::Backend>      \
      paykanBackendRegistration_##ID(                                          \
          ::paykan::plugin::PluginInfo<::paykan::backend::Backend>{            \
              NAME, FACTORY, PAYKAN_PLUGIN_BUILD_VERSION})
