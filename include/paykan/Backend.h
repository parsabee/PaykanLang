// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
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
//
// Migration note: the legacy LLVM backend (src/Backends/LLVM) still generates
// code straight from the typed AST; it reports consumesPIR() == false and the
// driver hands it the AST through Input::Sema / Input::TU.  Once the
// PIR-based LLVM translation lands, those two fields go away and every
// backend reads Input::Program.

#pragma once

#include "AST.h"
#include "Sema.h"
#include "paykan/Registry.h"
#include "paykan/Status.h"
#include "paykan/pir/PIR.h"

#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace paykan::backend {

/// What a backend can produce.  The driver refuses a request the backend
/// does not support before calling it.
struct Capabilities {
  bool EmitSource = false;     // emit(EmitKind::Source): C, LLVM IR, ...
  bool EmitObject = false;     // emit(EmitKind::Object)
  bool EmitExecutable = false; // emit(EmitKind::Executable): `paykan build`
  bool Run = false;            // run(): `paykan run`, in-process or not
  /// File extension of the backend's source output (".c", ".ll"), used for
  /// default output names and --help.
  std::string_view SourceExtension;
};

enum class EmitKind { Source, Object, Executable };

/// The program a backend works on.
struct Input {
  /// The main source file, as given on the command line.
  std::string InputFilename;
  /// The project root imports were resolved against (the main file's
  /// directory; "" for the current directory).
  std::string ProjectRoot;
  /// The verified PIR of the whole program (main module first).  Null only
  /// for a backend that does not consume PIR (see Backend::consumesPIR).
  const pir::Program *Program = nullptr;
  /// The typed AST, for the legacy LLVM backend only (see the migration note
  /// above).  Null for backends that consume PIR.
  const sema::SemaContext *Sema = nullptr;
  ast::TranslationUnit *TU = nullptr;
  /// -O<n>, 0..3.
  unsigned OptLevel = 0;
};

struct EmitOptions {
  /// Output file for Object / Executable; "" means a default next to the
  /// input.  Source output goes to the stream emit() is given.
  std::string OutputPath;
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

  /// The name the backend is registered under ("c", "llvm", ...).
  virtual std::string_view name() const = 0;

  virtual Capabilities capabilities() const = 0;

  /// Whether the backend reads Input::Program.  The driver runs the lowering
  /// (and the verifier) only for backends that do.  Legacy AST backends
  /// return false; everything else keeps the default.
  virtual bool consumesPIR() const { return true; }

  /// Translate the program into the requested artifact.  Source output is
  /// written to @p out; Object / Executable output to opts.OutputPath.
  virtual Status emit(const Input &in, EmitKind kind, const EmitOptions &opts,
                      std::ostream &out) = 0;

  /// Execute the program and return its exit code.  @p args are the
  /// program's arguments, args[0] being the script path.  Only called when
  /// capabilities().Run is set.
  virtual StatusOr<int> run(const Input &in, std::span<const std::string> args,
                            const RunOptions &opts) = 0;
};

using Registry = plugin::Registry<Backend>;

/// The backend used when none is named: PAYKAN_DEFAULT_BACKEND from the build
/// configuration ("" when the build has no backend).
std::string_view defaultBackend();

} // namespace paykan::backend

/// Registers a backend plugin.  @p ID is an identifier unique within the
/// plugin library, @p NAME the name users select it by, @p FACTORY a
/// `std::unique_ptr<paykan::backend::Backend> (*)()`.
#define PAYKAN_REGISTER_BACKEND(ID, NAME, FACTORY)                             \
  static const ::paykan::plugin::Registration<::paykan::backend::Backend>      \
      paykanBackendRegistration_##ID(NAME, FACTORY)
