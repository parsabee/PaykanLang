// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: MIT
// The paykan driver: frontend -> Sema -> backend, selected by name.

#include "ASTPrinter.h"
#include "DiagEngine.h"
#include "Options.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "Version.h"
#include "paykan/Backend.h"
#include "paykan/Frontend.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/Verifier.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using paykan::driver::Command;
using paykan::driver::Options;

namespace {

/// " (built with PaykanLang <v>, compatible)" or " (incompatible: <why>)"
/// for --version.
template <typename Entry> std::string compatibility(const Entry &e) {
  if (!e.Compatible)
    return " (incompatible: " + e.Incompatibility + ")";
  return " (built with PaykanLang " + e.BuildVersion + ", compatible)";
}

/// The version, the plugin build versions it accepts, and every registered
/// plugin with the version it was built with and whether it is compatible.
/// An incompatible plugin is never instantiated, so it has no description.
void printVersion() {
  std::cout << "PaykanLang " << paykan::kVersion << "\n";
  std::cout << "accepts plugins built with PaykanLang";
  const char *sep = " ";
  for (const char *v : paykan::plugin::compatibleBuildVersions()) {
    std::cout << sep << v;
    sep = ", ";
  }
  std::cout << "\n";
  for (const auto &e : paykan::frontend::Registry::get().entries())
    std::cout << "frontend " << e.Name << compatibility(e) << "\n";
  for (const auto &e : paykan::backend::Registry::get().entries()) {
    std::cout << "backend " << e.Name << compatibility(e);
    if (e.Compatible)
      if (auto be = paykan::backend::Registry::get().create(e.Name))
        if (std::string d = be->describe(); !d.empty())
          std::cout << ": " << d;
    std::cout << "\n";
  }
}

// --list-frontends / --list-backends: one line per plugin,
// "<name>[ (default)][: <description>]", or
// "<name> (incompatible: <why>)" for a plugin this paykan does not accept.
void listFrontends() {
  for (const auto &e : paykan::frontend::Registry::get().entries()) {
    std::cout << e.Name;
    if (!e.Compatible)
      std::cout << " (incompatible: " << e.Incompatibility << ")";
    else if (e.Name == paykan::frontend::defaultFrontend())
      std::cout << " (default)";
    std::cout << "\n";
  }
}

void listBackends() {
  for (const auto &e : paykan::backend::Registry::get().entries()) {
    std::cout << e.Name;
    if (!e.Compatible) {
      std::cout << " (incompatible: " << e.Incompatibility << ")\n";
      continue;
    }
    if (e.Name == paykan::backend::defaultBackend())
      std::cout << " (default)";
    if (auto be = paykan::backend::Registry::get().create(e.Name))
      if (std::string d = be->describe(); !d.empty())
        std::cout << ": " << d;
    std::cout << "\n";
  }
}

/// A driver (command line / configuration) error.
int fail(const std::string &msg) {
  std::cerr << "paykan: " << msg << "\n";
  return EXIT_FAILURE;
}

/// A backend failure: its message is already complete.
int failBackend(const paykan::Status &s) {
  std::cerr << s.message() << "\n";
  return EXIT_FAILURE;
}

/// Selecting a plugin this paykan does not accept (#103): exit status 2.
constexpr int kExitIncompatiblePlugin = 2;

/// Check that the plugin named @p name in @p registry, if there is one, is
/// compatible.  Returns the failure message ("" when it is fine or unknown).
template <typename Registry>
std::string checkCompatible(const Registry &registry, const char *kind,
                            const std::string &name) {
  const auto *e = registry.find(name);
  if (!e || e->Compatible)
    return {};
  return std::string("cannot use ") + kind + " '" + name +
         "' (incompatible: " + e->Incompatibility + ")";
}

} // namespace

int main(int argc, char *argv[]) {
  auto parsed = paykan::driver::parseCommandLine(argc, argv);
  const Options &opts = parsed.Opts;
  if (opts.ShowVersion) {
    printVersion();
    return EXIT_SUCCESS;
  }
  if (opts.ShowHelp) {
    paykan::driver::printUsage(std::cout, argv[0]);
    return EXIT_SUCCESS;
  }
  if (opts.ListFrontends) {
    listFrontends();
    return EXIT_SUCCESS;
  }
  if (opts.ListBackends) {
    listBackends();
    return EXIT_SUCCESS;
  }
  if (!parsed.Error.empty())
    return fail(parsed.Error + ". Try: '" + argv[0] + " --help'");
  if (!opts.Frontend.empty() &&
      !paykan::frontend::Registry::get().find(opts.Frontend))
    return fail("unknown frontend '" + opts.Frontend +
                "' (see --list-frontends)");
  if (!opts.Backend.empty() &&
      !paykan::backend::Registry::get().find(opts.Backend))
    return fail("unknown backend '" + opts.Backend + "' (see --list-backends)");
  // A plugin built with a version this paykan does not accept is listed but
  // can't be selected.  The defaults are built-in plugins, always compatible,
  // but they go through the same check.
  {
    std::string feName = opts.Frontend.empty()
                             ? std::string(paykan::frontend::defaultFrontend())
                             : opts.Frontend;
    std::string beName = opts.Backend.empty()
                             ? std::string(paykan::backend::defaultBackend())
                             : opts.Backend;
    for (const std::string &msg :
         {checkCompatible(paykan::frontend::Registry::get(), "frontend",
                          feName),
          checkCompatible(paykan::backend::Registry::get(), "backend", beName)})
      if (!msg.empty()) {
        std::cerr << "paykan: " << msg << "\n";
        return kExitIncompatiblePlugin;
      }
  }

  // -- Frontend -------------------------------------------------------------
  // One DiagEngine shared by every pass, wired into the parser up front so
  // syntax errors come out in the same rich source-located format as sema
  // errors (file:line:col + snippet + caret).  SourceLines lives inside the
  // driver and is filled by parseFile before the parser runs, so handing its
  // address over now is safe -- the vector itself never moves.
  paykan::sema::DiagEngine diag(std::cerr);
  paykan::frontend::Options feOpts;
  feOpts.TraceParsing = opts.TraceParsing;
  feOpts.TraceScanning = opts.TraceScanning;
  paykan::parser::ParserDriver driver(opts.Frontend, feOpts);
  diag.setSourceInfo(opts.InputFilename, &driver.getSourceLines());
  driver.setDiagEngine(&diag);

  if (opts.DumpTokens)
    return driver.dumpTokens(opts.InputFilename, std::cout) == 0 ? EXIT_SUCCESS
                                                                 : EXIT_FAILURE;

  if (driver.parseFile(opts.InputFilename) != 0) {
    std::cerr << "parsing failed with " << driver.getErrorCount()
              << " error(s)\n";
    return EXIT_FAILURE;
  }
  auto *root = driver.getRoot();

  if (opts.DumpAST) {
    paykan::ast::ASTPrinter printer(std::cout);
    printer.visit(root);
    return EXIT_SUCCESS;
  }

  // -- Semantic analysis ----------------------------------------------------
  std::string projectRoot =
      std::filesystem::path(opts.InputFilename).parent_path().string();
  // Sema reuses the DiagEngine constructed above (already carrying the
  // source info for the parsed file), so error counts accumulate across
  // passes and all diagnostics share one output stream.
  paykan::sema::Sema sema(driver.getASTContext(), diag, projectRoot,
                          driver.getFrontendName());
  // A program that is built or run needs an entry point; --check-only also
  // accepts a module without one, but still checks a declared `main`.
  sema.setEntryPointCheck(opts.CheckOnly
                              ? paykan::sema::Sema::EntryPoint::IfDeclared
                              : paykan::sema::Sema::EntryPoint::Required);
  auto semaCtx = sema.run(root);
  if (!semaCtx)
    return EXIT_FAILURE;
  if (opts.CheckOnly)
    return EXIT_SUCCESS;

  // -- Backend --------------------------------------------------------------
  std::string backendName = opts.Backend.empty()
                                ? std::string(paykan::backend::defaultBackend())
                                : opts.Backend;
  if (backendName.empty())
    return fail("this build has no backend (PAYKAN_BACKENDS was empty); only "
                "--check-only and --dump-ast are available");
  auto backend = paykan::backend::Registry::get().create(backendName);
  if (!backend)
    return fail("unknown backend '" + backendName + "' (see --list-backends)");
  const auto caps = backend->capabilities();

  paykan::backend::Input in;
  in.InputFilename = opts.InputFilename;
  in.ProjectRoot = projectRoot;
  in.OptLevel = opts.optLevel();
  // -- Lowering -------------------------------------------------------------
  // Every backend reads PIR: lower the program (the single home of the
  // ownership semantics), verify it, and hand it over.  --emit-pir prints it
  // instead.
  paykan::pir::Program program;
  if (!paykan::lowering::lowerProgram(semaCtx, root, opts.InputFilename,
                                      projectRoot, program, std::cerr))
    return fail("lowering to PIR failed");
  auto errors = paykan::pir::verify(program);
  if (!errors.empty())
    return fail("PIR verification failed:\n" +
                paykan::pir::formatErrors(errors));
  if (opts.EmitPIR) {
    paykan::pir::print(program, std::cout);
    std::cout.flush();
    return EXIT_SUCCESS;
  }
  in.Program = &program;

  if (opts.EmitSource) {
    if (!caps.EmitSource)
      return fail("backend '" + backendName + "' has no source output");
    paykan::Status s =
        backend->emit(in, paykan::backend::EmitKind::Source, {}, std::cout);
    std::cout.flush();
    return s ? EXIT_SUCCESS : failBackend(s);
  }

  if (opts.Cmd == Command::Build) {
    if (!caps.EmitExecutable)
      return fail("backend '" + backendName + "' cannot build executables");
    paykan::backend::EmitOptions eo;
    eo.OutputPath = opts.OutputPath;
    paykan::Status s =
        backend->emit(in, paykan::backend::EmitKind::Executable, eo, std::cout);
    return s ? EXIT_SUCCESS : failBackend(s);
  }

  if (!caps.Run)
    return fail("backend '" + backendName + "' cannot run programs");
  // args[0] = script path, args[1..] = program args.
  std::vector<std::string> progArgs;
  progArgs.push_back(opts.InputFilename);
  progArgs.insert(progArgs.end(), opts.ProgramArgs.begin(),
                  opts.ProgramArgs.end());
  paykan::backend::RunOptions ro;
  ro.TrackHeap = opts.TrackHeap;
  auto result = backend->run(in, progArgs, ro);
  if (!result)
    return failBackend(result.status());
  return *result;
}
