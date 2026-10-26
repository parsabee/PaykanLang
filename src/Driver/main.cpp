// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The paykan driver: frontend -> Sema -> backend, selected by name.

#include "ASTPrinter.h"
#include "DiagEngine.h"
#include "ModuleName.h"
#include "ModuleUtils.h"
#include "Names.h"
#include "Options.h"
#include "ParserDriver.h"
#include "Sema.h"
#include "Version.h"
#include "paykan/Backend.h"
#include "paykan/Frontend.h"
#include "paykan/PluginLoader.h"
#include "paykan/ast/Interchange.h"
#include "paykan/lowering/Lowering.h"
#include "paykan/modules/ModuleCode.h"
#include "paykan/modules/ModuleResolver.h"
#include "paykan/pir/Binary.h"
#include "paykan/pir/DeadCode.h"
#include "paykan/pir/Lend.h"
#include "paykan/pir/MoveAtLastUse.h"
#include "paykan/pir/Pass.h"
#include "paykan/pir/Printer.h"
#include "paykan/pir/RCElimination.h"
#include "paykan/pir/Verifier.h"
#include "paykan/pkm/Dump.h"
#include "paykan/pkm/File.h"
#include "paykan/pkm/Object.h"
#include "paykan/plugin_api.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using paykan::driver::Command;
using paykan::driver::Options;
using paykan::driver::PkmVerb;

namespace {

namespace pkm = paykan::pkm;

/// A module's compiled code as the backend interface has it, and as a
/// `.pkm`'s OBJ section does.
paykan::backend::ModuleObject toBackend(const pkm::Object &o) {
  return {o.Backend, o.Format, o.Target, o.Producer, o.Bytes};
}
pkm::Object toPkm(const paykan::backend::ModuleObject &o) {
  return {o.Backend, o.Format, o.Target, o.Producer, o.Bytes};
}

/// A copy of @p m (a module holds its blocks by unique_ptr): the binary
/// form round-trips every module the verifier accepts.
paykan::pir::Module copyOf(const paykan::pir::Module &m) {
  auto copy = paykan::pir::binary::decode(paykan::pir::binary::encode(m));
  return copy ? std::move(*copy) : paykan::pir::Module();
}

/// Add @p pass to @p passes unless @p skip names it (--pir-skip).
void addPass(paykan::pir::PassManager &passes,
             std::unique_ptr<paykan::pir::Pass> pass,
             const std::vector<std::string> &skip) {
  if (std::find(skip.begin(), skip.end(), pass->name()) == skip.end())
    passes.add(std::move(pass));
}

/// The passes that make code better, run on every program before a backend
/// sees it and on every module's code before its `.pkm` is written
/// (docs/pir.md §12), in this order, but those @p skip names: the
/// standard library's compiled code is made by them too.
void addCodePasses(paykan::pir::PassManager &passes,
                   const std::vector<std::string> &skip) {
  addPass(passes, paykan::pir::createMovesAtLastUsePass(), skip);
  addPass(passes, paykan::pir::createLendingPass(), skip);
  addPass(passes, paykan::pir::createRCEliminationPass(), skip);
}

/// Whether the program is verified after each PIR pass, so that a pass
/// that breaks it is the one the error names: in a build with assertions.
/// A module's code is verified after its passes in every build, since its
/// `.pkm` keeps it.
#ifndef NDEBUG
constexpr bool kVerifyPasses = true;
#else
constexpr bool kVerifyPasses = false;
#endif

/// What a module's `.pkm` holds of its code (docs/design/pkm.md §5.13).
struct WrittenCode {
  paykan::pir::Module Code;
  std::optional<paykan::backend::ModuleObject> Object;
};

/// @p module (as lowered) trimmed to what it and its importers (whose view
/// of it is @p iface) reach, through the code passes but those @p skip
/// names (what they did added to @p stats) and verified, and with a backend
/// that compiles modules, split into its CODE and the code @p backend
/// compiles.
paykan::StatusOr<WrittenCode>
moduleCode(paykan::pir::Module module, const pkm::Interface &iface,
           paykan::backend::Backend *backend,
           const paykan::backend::Capabilities &caps,
           const std::vector<std::string> &skip,
           paykan::pir::Statistics &stats) {
  paykan::modules::trimModuleCode(module, iface);
  {
    paykan::pir::Program code;
    code.Modules.push_back(std::move(module));
    paykan::pir::PassManager passes;
    addCodePasses(passes, skip);
    if (paykan::Status s = passes.run(code, stats); !s)
      return s;
    module = std::move(code.Modules.front());
  }
  if (auto errors = paykan::pir::verify(module); !errors.empty())
    return paykan::Status::error(
        "internal compiler error: the module's code does not verify after "
        "the PIR passes:\n" +
        paykan::pir::formatErrors(errors));
  WrittenCode out;
  if (!backend || !caps.CompileModules) {
    out.Code = std::move(module);
    return out;
  }
  paykan::modules::ModuleCode parts =
      paykan::modules::splitModuleCode(std::move(module));
  for (const paykan::pir::Module *m : {&parts.Code, &parts.Compiled})
    if (auto errors = paykan::pir::verify(*m); !errors.empty())
      return paykan::Status::error(
          "internal compiler error: the split module does not verify:\n" +
          paykan::pir::formatErrors(errors));
  auto object = backend->compileModule(parts.Compiled);
  if (!object)
    return object.status();
  out.Code = std::move(parts.Code);
  out.Object = std::move(*object);
  return out;
}

/// " [<file>]" for a loaded plugin, "" for a built-in one.
template <typename Entry> std::string origin(const Entry &e) {
  return e.Path.empty() ? std::string() : " [" + e.Path + "]";
}

/// " (built with PaykanLang <v>, compatible)", " (incompatible: <why>)" or
/// " (ambiguous: provided by both ...)" for --version.  A name two files
/// provide is ambiguous whether or not the first of them is compatible, so
/// the ambiguity, which names both files, is reported first.
template <typename Entry> std::string compatibility(const Entry &e) {
  if (!e.Conflict.empty())
    return " (ambiguous: " + e.Conflict + ")";
  if (!e.Compatible)
    return " (incompatible: " + e.Incompatibility + ")";
  return " (built with PaykanLang " + e.BuildVersion + ", compatible)";
}

/// The rejected plugin files, one "rejected plugin <file>: <why>" line each.
void printRejected(const paykan::plugin::LoadReport &plugins) {
  for (const auto &f : plugins.Files)
    if (!f.Error.empty())
      std::cout << "rejected plugin " << f.Path << ": " << f.Error << "\n";
}

/// The version, the plugin build versions it accepts, every registered
/// plugin with the version it was built with and whether it is compatible,
/// and where plugins are loaded from.  An incompatible plugin is never
/// instantiated, so it has no description.
void printVersion(const paykan::plugin::LoadReport &plugins) {
  std::cout << "PaykanLang " << paykan::kVersion << "\n";
  std::cout << "accepts plugins built with PaykanLang";
  const char *sep = " ";
  for (const char *v : paykan::plugin::compatibleBuildVersions()) {
    std::cout << sep << v;
    sep = ", ";
  }
  std::cout << "\n";
  for (const auto &e : paykan::frontend::Registry::get().entries())
    std::cout << "frontend " << e.Name << compatibility(e) << origin(e) << "\n";
  for (const auto &e : paykan::backend::Registry::get().entries()) {
    std::cout << "backend " << e.Name << compatibility(e);
    if (e.Compatible)
      if (auto be = paykan::backend::Registry::get().create(e.Name))
        if (std::string d = be->describe(); !d.empty())
          std::cout << ": " << d;
    std::cout << origin(e) << "\n";
  }
  std::cout << "plugin API " << paykan::versionString(PAYKAN_PLUGIN_API_VERSION)
            << "\n";
  std::cout << "standard library: "
            << paykan::module_utils::stdlibDir("").string() << "\n";
  std::cout << "plugin directories:";
  if (plugins.DiscoveryDisabled)
    std::cout << " none searched (--no-plugins or PAYKAN_NO_PLUGINS)";
  for (const std::string &d : plugins.SearchDirs)
    std::cout << " " << d;
  std::cout << "\n";
  for (const auto &f : plugins.Files) {
    if (!f.Error.empty())
      continue;
    std::cout << "plugin " << f.Path;
    if (!f.Name.empty())
      std::cout << " (" << f.Name << (f.Version.empty() ? "" : " ") << f.Version
                << ")";
    std::cout << ":";
    const char *psep = " ";
    for (const std::string &what : f.Provides) {
      std::cout << psep << what;
      psep = ", ";
    }
    std::cout << "\n";
  }
  printRejected(plugins);
}

/// Record in @p opts the frontend @p name that parses the program, as its
/// module files keep it (docs/design/pkm.md §2.2, tag 0x20): a built-in
/// frontend has this paykan's version and no plugin; a plugin's frontend
/// has the plugin's own version, and the plugin is named by its descriptor
/// (else by its file's name).
void recordFrontend(const std::string &name,
                    const paykan::plugin::LoadReport &plugins,
                    paykan::modules::ResolverOptions &opts) {
  opts.FrontendName = name;
  const auto *e = paykan::frontend::Registry::get().find(name);
  if (!e || e->Path.empty()) {
    opts.FrontendVersion = paykan::kVersion;
    return;
  }
  for (const auto &f : plugins.Files)
    if (f.Path == e->Path) {
      opts.FrontendVersion = f.Version;
      opts.FrontendPlugin =
          f.Name.empty() ? std::filesystem::path(f.Path).filename().string()
                         : f.Name;
    }
}

// --list-frontends / --list-backends: one line per plugin,
// "<name>[ (default)][: <description>][ [<file>]]",
// "<name> (incompatible: <why>)[ [<file>]]" for a plugin this paykan does
// not accept, or "<name> (ambiguous: <why>)[ [<file>]]" for a name two
// plugins provide; then a "rejected plugin <file>: <why>" line for every
// plugin file that could not be loaded.
void listFrontends(const paykan::plugin::LoadReport &plugins) {
  for (const auto &e : paykan::frontend::Registry::get().entries()) {
    std::cout << e.Name;
    if (!e.Conflict.empty())
      std::cout << " (ambiguous: " << e.Conflict << ")";
    else if (!e.Compatible)
      std::cout << " (incompatible: " << e.Incompatibility << ")";
    else if (e.Name == paykan::frontend::defaultFrontend())
      std::cout << " (default)";
    if (e.Compatible && e.Conflict.empty() && !e.Description.empty())
      std::cout << ": " << e.Description;
    std::cout << origin(e) << "\n";
  }
  printRejected(plugins);
}

void listBackends(const paykan::plugin::LoadReport &plugins) {
  for (const auto &e : paykan::backend::Registry::get().entries()) {
    std::cout << e.Name;
    if (!e.Conflict.empty()) {
      std::cout << " (ambiguous: " << e.Conflict << ")" << origin(e) << "\n";
      continue;
    }
    if (!e.Compatible) {
      std::cout << " (incompatible: " << e.Incompatibility << ")" << origin(e)
                << "\n";
      continue;
    }
    if (e.Name == paykan::backend::defaultBackend())
      std::cout << " (default)";
    if (auto be = paykan::backend::Registry::get().create(e.Name))
      if (std::string d = be->describe(); !d.empty())
        std::cout << ": " << d;
    std::cout << origin(e) << "\n";
  }
  printRejected(plugins);
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

/// @p status for a command that wrote to standard output, once what it
/// wrote is flushed, or EXIT_FAILURE with a message when some of it could
/// not be written (a full disk, say): a stream that failed takes no more
/// output, so the error is reported here, at the end.  The message gives
/// the reason when the final flush is the write that failed; that of an
/// earlier write may since have been overwritten (coreutils' close_stdout
/// does the same).
int finishStdout(int status) {
  bool failed = !std::cout;
  std::cout.clear();
  errno = 0;
  std::cout.flush();
  int error = errno;
  if (!failed && std::cout && !std::ferror(stdout))
    return status;
  std::cerr << "paykan: cannot write to standard output";
  if (error != 0)
    std::cerr << ": " << std::strerror(error);
  std::cerr << "\n";
  return EXIT_FAILURE;
}

/// Whether @p a and @p b are one file, however each is spelled: through
/// `.`, `..` or repeated slashes, a symbolic or a hard link, or in another
/// case on a case-insensitive file system.  A path that names no file is no
/// other file.
bool sameFile(const std::filesystem::path &a, const std::filesystem::path &b) {
  std::error_code ec;
  return std::filesystem::equivalent(a, b, ec) && !ec;
}

/// Why the driver must not write @p output: it is a file the command reads
/// (the source file, or the module file of `pkm`, an --object file, or a
/// file @p resolver, when given, read for the imports: a source or a
/// module file); "" when it is none of them.
std::string
overwritesInput(const std::string &output, const Options &opts,
                const paykan::modules::ModuleResolver *resolver = nullptr) {
  std::vector<std::string> inputs{opts.InputFilename};
  inputs.insert(inputs.end(), opts.Objects.begin(), opts.Objects.end());
  if (resolver)
    for (const std::filesystem::path &file : resolver->filesRead())
      inputs.push_back(file.string());
  for (const std::string &input : inputs) {
    if (!sameFile(output, input))
      continue;
    std::string why = "the output '";
    why += output;
    why += "' is the input file '";
    why += input;
    why += "' (choose another with -o)";
    return why;
  }
  return {};
}

/// The executable `build` writes without -o, in the current directory.  A
/// source file named with a `.pkn` (or `.pk`) extension gives its name
/// without it (`prog.pkn` gives `prog`), unless what is left still has
/// one, in either case for a file system that ignores case; that name and
/// any other get `.out` added (`prog` gives `prog.out`, `prog.pkn.bak`
/// `prog.pkn.bak.out` and `main.pkn.pkn` `main.pkn.pkn.out`), so the
/// executable never takes the name of another source.
std::string defaultExecutable(const std::string &input) {
  std::filesystem::path name = std::filesystem::path(input).filename();
  auto sourceExtension = [](const std::filesystem::path &p, bool anyCase) {
    std::string ext = p.extension().string();
    if (anyCase)
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
    return ext == ".pkn" || ext == ".pk";
  };
  if (sourceExtension(name, false) && !sourceExtension(name.stem(), true))
    return name.stem().string();
  return name.string() + ".out";
}

/// What @p print returns (EXIT_SUCCESS, or the status of a failure it
/// reported), run on where an --emit-* option prints: -o's file, or
/// standard output without -o.  The file is never one the command reads
/// (overwritesInput, with @p resolver when the imports were resolved), and
/// is written once all of it was printed, as a module file is
/// (pkm::writeFileAtomically): a failure leaves what the path names as it
/// was.
template <typename Print>
int printOutput(const Options &opts,
                const paykan::modules::ModuleResolver *resolver,
                Print &&print) {
  if (opts.OutputPath.empty()) {
    int status = print(std::cout);
    return status == EXIT_SUCCESS ? finishStdout(status) : status;
  }
  const std::string &path = opts.OutputPath;
  if (std::string why = overwritesInput(path, opts, resolver); !why.empty())
    return fail(why);
  std::ostringstream text;
  if (int status = print(text); status != EXIT_SUCCESS)
    return status;
  const std::string bytes = std::move(text).str();
  paykan::Status s = pkm::writeFileAtomically(
      path,
      std::span(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
  return s ? EXIT_SUCCESS : fail("cannot write '" + path + "': " + s.message());
}

/// Selecting a plugin this paykan does not accept (#103), an ambiguous
/// one, or naming a --plugin file that is rejected: exit status 2.
constexpr int kExitIncompatiblePlugin = 2;

/// Check that the plugin named @p name in @p registry, if there is one, is
/// compatible and unambiguous.  Returns the failure message ("" when it is
/// fine or unknown).
template <typename Registry>
std::string checkCompatible(const Registry &registry, const char *kind,
                            const std::string &name) {
  const auto *e = registry.find(name);
  if (!e)
    return {};
  std::string head = std::string("cannot use ") + kind + " '" + name + "'";
  if (!e->Conflict.empty())
    return head + " (ambiguous: " + e->Conflict + ")";
  if (!e->Compatible)
    return head + " (incompatible: " + e->Incompatibility + ")" + origin(*e);
  return {};
}

/// "unknown <kind> '<name>' (see --list-<kind>s)", noting rejected plugin
/// files, which may be where the name was meant to come from.
std::string unknownPlugin(const char *kind, const std::string &name,
                          const paykan::plugin::LoadReport &plugins) {
  size_t rejected = 0;
  for (const auto &f : plugins.Files)
    rejected += f.Error.empty() ? 0 : 1;
  std::string msg = std::string("unknown ") + kind + " '" + name +
                    "' (see --list-" + kind + "s";
  if (rejected)
    msg += "; " + std::to_string(rejected) + " plugin file" +
           (rejected == 1 ? " was" : "s were") + " rejected";
  return msg + ")";
}

// -- `paykan pkm`

/// `pkm dump f.pkm [--section=...]`: the stable dump (pkm::dump) and, for
/// the whole file or `--section=code`, the PIR text of CODE.  `pkm check
/// f.pkm`: the verdict against this toolchain (docs/design/pkm.md §8.2) and
/// whether IFACE and CODE decode; exit status 0 when the file is usable.
int pkmTool(const Options &opts) {
  namespace pkm = paykan::pkm;
  const std::string &path = opts.InputFilename;
  auto bytes = pkm::readFileBytes(path);
  if (!bytes)
    return fail(bytes.status().message());
  pkm::Error err;
  auto file = pkm::File::read(*bytes, {}, &err);
  if (!file)
    return fail(path + ": " + err.str());
  auto codeText = [&](std::ostream &os) -> bool {
    auto code = (*file).section(pkm::Kind::Code);
    if (code.empty())
      return true;
    auto mod = paykan::pir::binary::decode(code);
    if (!mod) {
      std::cerr << "paykan: " << path
                << ": CODE does not decode: " << mod.status().message() << "\n";
      return false;
    }
    if (auto errors = paykan::pir::verify(*mod); !errors.empty()) {
      std::cerr << "paykan: " << path << ": CODE does not verify:\n"
                << paykan::pir::formatErrors(errors);
      return false;
    }
    paykan::pir::print(*mod, os);
    return true;
  };
  if (opts.Verb == PkmVerb::Extract) {
    // The module's compiled code as a native object, for a program's C
    // (--emit-c) to link with.
    if (!opts.PkmObject)
      return fail("`pkm extract` needs what to extract: --object");
    std::span<const uint8_t> section = (*file).section(pkm::Kind::Object);
    if (section.empty())
      return fail(path + " has no compiled code (its CODE is all of it)");
    auto object = pkm::readObject(section);
    if (!object)
      return fail(path + ": " + object.status().message());
    std::string backendName =
        opts.Backend.empty() ? std::string(paykan::backend::defaultBackend())
                             : opts.Backend;
    auto backend = paykan::backend::Registry::get().create(backendName);
    if (!backend)
      return fail("unknown backend '" + backendName + "'");
    if (!backend->capabilities().CompileModules)
      return fail("backend '" + backendName + "' does not compile modules");
    std::string out = opts.OutputPath.empty() ? std::filesystem::path(path)
                                                    .filename()
                                                    .replace_extension(".o")
                                                    .string()
                                              : opts.OutputPath;
    if (std::string why = overwritesInput(out, opts); !why.empty())
      return fail(why);
    if (paykan::Status s =
            backend->emitModuleObject(toBackend(*object), opts.optLevel(), out);
        !s)
      return fail(path + ": " + s.message());
    return EXIT_SUCCESS;
  }
  if (opts.Verb == PkmVerb::Check) {
    pkm::Verdict v = pkm::checkCompatibility(
        (*file).manifest(), paykan::modules::ModuleResolver::hostIdentity(),
        pkm::Policy::Prebuilt);
    bool ok = v.Outcome == pkm::Verdict::Kind::Usable;
    std::cout << path << ": "
              << (ok ? "usable"
                     : (v.Outcome == pkm::Verdict::Kind::Stale ? "stale: "
                                                               : "rejected: ") +
                           v.Message)
              << "\n";
    auto iface = pkm::readInterface((*file).section(pkm::Kind::Iface),
                                    (*file).manifest().Module);
    if (!iface) {
      std::cerr << "paykan: " << path
                << ": IFACE does not decode: " << iface.status().message()
                << "\n";
      ok = false;
    }
    std::ostream nowhere(nullptr);
    if (!codeText(nowhere))
      ok = false;
    return finishStdout(ok ? EXIT_SUCCESS : EXIT_FAILURE);
  }
  pkm::DumpOptions dopts;
  using S = pkm::DumpOptions::Section;
  const std::string &sec = opts.PkmSection;
  bool codeOnly = sec == "code";
  if (sec.empty() || sec == "all")
    dopts.Which = S::All;
  else if (sec == "manifest")
    dopts.Which = S::Manifest;
  else if (sec == "sections")
    dopts.Which = S::Sections;
  else if (sec == "iface")
    dopts.Which = S::Iface;
  else if (sec == "tmpl")
    dopts.Which = S::Tmpl;
  else if (sec == "symidx")
    dopts.Which = S::SymIdx;
  else if (sec == "obj")
    dopts.Which = S::Object;
  else if (sec == "payloads")
    dopts.Which = S::Payloads;
  else if (!codeOnly)
    return fail("unknown section '" + sec + "' (expected " +
                paykan::driver::pkmDumpSectionList(", ", " or ") + ")");
  if (!codeOnly)
    pkm::dump(*file, std::cout, dopts);
  bool ok = true;
  if (codeOnly || dopts.Which == S::All) {
    if (!codeOnly && !(*file).section(pkm::Kind::Code).empty())
      std::cout << "pir:\n";
    ok = codeText(std::cout);
  }
  return finishStdout(ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

/// The canonical name of the main module and the root its imports are
/// resolved against.  Normally the file's stem and its directory.  For
/// `--emit-pkm` of a module given by a relative path inside the project
/// (`geometry/shapes.pkn`, or `./geometry/shapes.pkn`, run from the project
/// root), the module is named by that path made normal (`geometry::shapes`)
/// and the root is the current directory, so the file is what an importer
/// of `geometry::shapes` expects; one given by an absolute path or one
/// that goes up (`/src/shapes.pkn`, `../shapes.pkn`) is named by its
/// stem.  Each part of the name --emit-pkm gives a module must be an
/// identifier, which an import can name.  --module-name names it outright
/// (the standard library's build names its modules `::name`).
paykan::Status mainModuleIdentity(const Options &opts, std::string &name,
                                  std::string &projectRoot) {
  std::filesystem::path input(opts.InputFilename);
  name = paykan::module_name::mainModuleName(opts.InputFilename);
  projectRoot = input.parent_path().string();
  if (!opts.ModuleName.empty()) {
    // --module-name: as given (its imports still resolve against the
    // file's directory).
    name = opts.ModuleName;
    return paykan::Status::ok();
  }
  if (!opts.EmitPkm)
    return paykan::Status::ok();
  std::filesystem::path normal = input.lexically_normal();
  std::vector<std::string> parts;
  if (!normal.is_absolute())
    for (const auto &c : normal.parent_path()) {
      if (c == "..") {
        parts.clear();
        break;
      }
      parts.push_back(c.string());
    }
  parts.push_back(normal.stem().string());
  for (const std::string &part : parts)
    if (!paykan::module_name::isIdentifier(part))
      return paykan::Status::error(
          "cannot name the module of '" + opts.InputFilename +
          "' after its path: '" + part +
          "' is not an identifier (name it with --module-name)");
  if (parts.size() > 1) {
    name.clear();
    for (const std::string &part : parts) {
      if (!name.empty())
        name += paykan::module_name::kSep;
      name += part;
    }
    projectRoot = "";
  }
  return paykan::Status::ok();
}

/// The --module-path directories, then $PAYKAN_MODULE_PATH's.
std::vector<std::string> modulePath(const Options &opts) {
  std::vector<std::string> dirs = opts.ModulePath;
  if (const char *env = std::getenv(paykan::names::kPaykanModulePathEnv)) {
    std::string_view rest(env);
    while (!rest.empty()) {
      size_t sep = rest.find(':');
      std::string_view dir = rest.substr(0, sep);
      if (!dir.empty())
        dirs.emplace_back(dir);
      if (sep == std::string_view::npos)
        break;
      rest.remove_prefix(sep + 1);
    }
  }
  return dirs;
}

} // namespace

int main(int argc, char *argv[]) {
  auto parsed = paykan::driver::parseCommandLine(argc, argv);
  const Options &opts = parsed.Opts;
  if (opts.ShowHelp && !opts.ShowVersion) {
    paykan::driver::printUsage(std::cout, argv[0]);
    return finishStdout(EXIT_SUCCESS);
  }
  // `pkm` loads no plugin, but --version and --list-* (which need them)
  // still answer after it.
  if (opts.Cmd == Command::Pkm &&
      !(opts.ShowVersion || opts.ListFrontends || opts.ListBackends)) {
    if (!parsed.Error.empty())
      return fail(parsed.Error + ". Try: '" + argv[0] + " --help'");
    return pkmTool(opts);
  }
  // The plugins: the --plugin files, then the plugin directories
  // (paykan/PluginLoader.h).  Each is checked before any of its callbacks
  // can run; what fails is listed below and can't be selected.
  paykan::plugin::DiscoveryOptions discovery =
      paykan::plugin::discoveryOptionsFromEnvironment();
  discovery.ExplicitFiles = opts.Plugins;
  // The installed standard library, next to the executable as the system
  // plugin directory is.
  if (!discovery.ExecutableDir.empty())
    paykan::module_utils::installedStdlibDir() =
        (std::filesystem::path(discovery.ExecutableDir) /
         PAYKAN_STDLIB_DIR_FROM_BINDIR)
            .lexically_normal();
  if (opts.NoPlugins)
    discovery.DisableDiscovery = true;
  const paykan::plugin::LoadReport &plugins =
      paykan::plugin::loadPlugins(discovery);
  if (opts.ShowVersion) {
    printVersion(plugins);
    return finishStdout(EXIT_SUCCESS);
  }
  if (opts.ListFrontends) {
    listFrontends(plugins);
    return finishStdout(EXIT_SUCCESS);
  }
  if (opts.ListBackends) {
    listBackends(plugins);
    return finishStdout(EXIT_SUCCESS);
  }
  if (!parsed.Error.empty())
    return fail(parsed.Error + ". Try: '" + argv[0] + " --help'");
  // A plugin named on the command line must load.
  for (const auto &f : plugins.Files)
    if (f.Explicit && !f.Error.empty()) {
      std::cerr << "paykan: cannot load plugin '" << f.Path << "': " << f.Error
                << "\n";
      return kExitIncompatiblePlugin;
    }
  if (!opts.Frontend.empty() &&
      !paykan::frontend::Registry::get().find(opts.Frontend))
    return fail(unknownPlugin("frontend", opts.Frontend, plugins));
  if (!opts.Backend.empty() &&
      !paykan::backend::Registry::get().find(opts.Backend))
    return fail(unknownPlugin("backend", opts.Backend, plugins));
  // A plugin built with a version this paykan does not accept, or a name two
  // plugins provide, is listed but can't be selected.  The defaults are
  // built-in plugins, always compatible, but they go through the same check
  // (a loaded plugin may clash with one).
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

  // -- Frontend
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
    return finishStdout(driver.dumpTokens(opts.InputFilename, std::cout) == 0
                            ? EXIT_SUCCESS
                            : EXIT_FAILURE);

  if (driver.parseFile(opts.InputFilename) != 0) {
    std::cerr << "parsing failed with " << driver.getErrorCount()
              << " error(s)\n";
    return EXIT_FAILURE;
  }
  auto *root = driver.getRoot();

  if (opts.DumpAST) {
    paykan::ast::ASTPrinter printer(std::cout);
    printer.visit(root);
    return finishStdout(EXIT_SUCCESS);
  }
  if (opts.EmitAST) {
    // What a frontend plugin returns (docs/plugins/ast-format.md).
    return printOutput(opts, nullptr, [&](std::ostream &os) {
      std::string error;
      if (!paykan::ast::interchange::write(*root, os, error))
        return fail("cannot write the AST: " + error);
      return EXIT_SUCCESS;
    });
  }

  // -- Backend
  // Chosen before the imports are resolved: a cache entry holds the code
  // the backend that wrote it compiled, which only that backend links.
  std::unique_ptr<paykan::backend::Backend> backend;
  paykan::backend::Capabilities caps;
  std::string backendName;
  if (!opts.CheckOnly) {
    backendName = opts.Backend.empty()
                      ? std::string(paykan::backend::defaultBackend())
                      : opts.Backend;
    if (backendName.empty())
      return fail("this build has no backend (PAYKAN_BACKENDS was empty); "
                  "only --check-only and --dump-ast are available");
    backend = paykan::backend::Registry::get().create(backendName);
    if (!backend)
      return fail("unknown backend '" + backendName +
                  "' (see --list-backends)");
    caps = backend->capabilities();
  }

  // -- Semantic analysis
  std::string mainName, projectRoot;
  if (paykan::Status s = mainModuleIdentity(opts, mainName, projectRoot); !s)
    return fail(s.message());
  // Imports go through the resolver: a source module's `.pkm` cache entry
  // under <root>/.paykan_cache when it is fresh, a prebuilt `.pkm` when
  // there is no source, the source otherwise (docs/design/pkm.md §8).
  paykan::modules::ResolverOptions resolverOpts;
  resolverOpts.ProjectRoot = projectRoot;
  recordFrontend(driver.getFrontendName(), plugins, resolverOpts);
  resolverOpts.ModulePath = modulePath(opts);
  resolverOpts.ReadCache = !opts.RebuildModules && !opts.NoModuleCache;
  resolverOpts.WriteCache = !opts.NoModuleCache;
  resolverOpts.Verbose = opts.Verbose ? &std::cerr : nullptr;
  if (backend)
    resolverOpts.CompiledBy =
        caps.CompileModules ? toPkm(backend->objectKind()) : pkm::Object{};
  paykan::modules::ModuleResolver resolver(
      resolverOpts, paykan::modules::ModuleResolver::hostIdentity());
  // Sema reuses the DiagEngine constructed above (already carrying the
  // source info for the parsed file), so error counts accumulate across
  // passes and all diagnostics share one output stream.
  paykan::sema::Sema sema(driver.getASTContext(), diag, projectRoot,
                          driver.getFrontendName());
  sema.setResolver(&resolver);
  // A program that is built or run needs an entry point; --check-only and
  // --emit-pkm also accept a module without one, but still check a declared
  // `main`.
  sema.setEntryPointCheck(opts.CheckOnly || opts.EmitPkm
                              ? paykan::sema::Sema::EntryPoint::IfDeclared
                              : paykan::sema::Sema::EntryPoint::Required);
  auto semaCtx = sema.run(root);
  if (!semaCtx)
    return EXIT_FAILURE;
  if (opts.CheckOnly)
    return EXIT_SUCCESS;

  paykan::backend::Input in;
  in.InputFilename = opts.InputFilename;
  in.ProjectRoot = projectRoot;
  in.OptLevel = opts.optLevel();
  in.Objects = opts.Objects;
  // -- Lowering
  // Every backend reads PIR: lower the program (the single home of the
  // ownership semantics), verify it, and hand it over.  --emit-pir prints it
  // instead.
  // Each module keeps its own copies of the instantiations it uses until the
  // cache entries (and --emit-pkm's file) are written: those must not
  // depend on this program.  The main module of a program to run or build,
  // which no module imports, knows every class derived from its classes; a
  // module --emit-pkm writes may be imported.
  paykan::pir::Program program;
  if (!paykan::lowering::lowerProgram(
          semaCtx, root, mainName, projectRoot, program, std::cerr, &resolver,
          paykan::lowering::Copies::PerModule,
          opts.EmitPkm ? paykan::lowering::World::Open
                       : paykan::lowering::World::Closed))
    return fail("lowering to PIR failed");
  if (!paykan::lowering::checkNativeSymbols(program, std::cerr))
    return EXIT_FAILURE;
  auto errors = paykan::pir::verify(program);
  if (opts.EmitPkm) {
    // A library module has no entry point; everything else about the
    // program (its own modules, the links between them) must still hold.
    std::erase_if(errors, [](const paykan::pir::VerifyError &e) {
      return e.Message == "main module defines no '@main'";
    });
  }
  if (!errors.empty())
    return fail("PIR verification failed:\n" +
                paykan::pir::formatErrors(errors));
  // The whole program verified: the modules built from source this run get
  // their cache entries now, never before.  Each keeps what it and its
  // importers reach; with a backend that compiles modules, its code other
  // than its copies of instantiations is compiled, and the program uses the
  // module as an importer will (when nothing else of the program needs a
  // copy it dropped).
  std::map<std::string, paykan::backend::ModuleObject> compiled;
  // What the PIR passes do, to the modules' code and to the program's.
  paykan::pir::Statistics stats;
  for (const std::string &name : resolver.cacheable()) {
    // An import: the main module, first, is never cached (a program's
    // knows what no module that may be imported may know).
    auto mod = std::find_if(
        std::next(program.Modules.begin()), program.Modules.end(),
        [&](const paykan::pir::Module &m) { return m.Name == name; });
    const pkm::Interface *iface = resolver.builtInterface(name);
    if (mod == program.Modules.end() || !iface)
      continue;
    auto written = moduleCode(copyOf(*mod), *iface, backend.get(), caps,
                              opts.SkipPasses, stats);
    if (!written)
      return fail("cannot write the cache entry of module '" + name +
                  "': " + written.status().message());
    WrittenCode &code = *written;
    std::optional<pkm::Object> object;
    if (code.Object)
      object = toPkm(*code.Object);
    resolver.writeCacheEntry(name, code.Code, object ? &*object : nullptr);
    if (code.Object && paykan::modules::providesFor(code.Code, program)) {
      *mod = std::move(code.Code);
      compiled[name] = std::move(*code.Object);
    }
  }
  if (opts.EmitPkm) {
    std::filesystem::path out = opts.OutputPath.empty()
                                    ? std::filesystem::path(opts.InputFilename)
                                          .replace_extension(".pkm")
                                    : std::filesystem::path(opts.OutputPath);
    if (std::string why = overwritesInput(out.string(), opts, &resolver);
        !why.empty())
      return fail(why);
    std::vector<std::string> deps;
    for (auto *imp : root->getImports())
      for (auto &m : imp->getModules())
        deps.push_back(paykan::module_name::canonicalImportName(
            imp->modulePath(m), imp->isSystem()));
    // Its templates, for importers to instantiate without its source.
    std::vector<uint8_t> templates;
    if (auto mt = sema.exportTemplates(root, mainName)) {
      mt->File = resolver.relativeToRoot(opts.InputFilename);
      std::string why;
      templates = paykan::sema::Sema::encodeTemplates(*mt, why);
      if (templates.empty())
        return fail("cannot write the templates of " + opts.InputFilename +
                    ": " + why);
    }
    pkm::Interface iface =
        paykan::sema::Sema::toInterface(sema.exportModuleInfo(root, mainName));
    auto written = moduleCode(std::move(program.Modules.front()), iface,
                              backend.get(), caps, opts.SkipPasses, stats);
    if (!written)
      return fail("cannot write " + out.string() + ": " +
                  written.status().message());
    const WrittenCode &code = *written;
    std::optional<pkm::Object> object;
    if (code.Object)
      object = toPkm(*code.Object);
    paykan::Status s = resolver.writeModuleFile(
        out, mainName, std::move(iface), std::move(deps), opts.InputFilename,
        code.Code, templates, object ? &*object : nullptr);
    if (!s)
      return fail("cannot write " + out.string() + ": " + s.message());
    if (opts.Stats)
      stats.print(std::cerr);
    return EXIT_SUCCESS;
  }
  // The program runs with one copy of each instantiation.
  paykan::lowering::shareInstantiations(program);
  errors = paykan::pir::verify(program);
  if (!errors.empty())
    return fail("PIR verification failed:\n" +
                paykan::pir::formatErrors(errors));
  // What the program's entry point cannot reach is dropped before the
  // backend lowers anything, and the code passes run on what is left
  // (docs/pir.md §12).  The C of --emit-c is linked with each compiled
  // module's code as a whole object (`pkm extract --object`), so everything
  // that code uses stays too.
  paykan::pir::Roots roots = paykan::pir::Roots::entryPoint(program);
  if (opts.EmitSource && opts.EmitStage == "c") {
    roots.Functions = [entry =
                           roots.Functions](const paykan::pir::Module &m,
                                            const paykan::pir::Function &f) {
      return entry(m, f) || m.isCompiled(f);
    };
    roots.Classes = [](const paykan::pir::Module &m,
                       const paykan::pir::Class &c) { return m.isCompiled(c); };
  }
  paykan::pir::PassManager passes(kVerifyPasses);
  addPass(passes, paykan::pir::createDeadCodePass(std::move(roots)),
          opts.SkipPasses);
  addCodePasses(passes, opts.SkipPasses);
  if (paykan::Status s = passes.run(program, stats); !s)
    return fail(s.message());
  if (opts.Stats)
    stats.print(std::cerr);
  if (opts.EmitPIR)
    return printOutput(opts, &resolver, [&](std::ostream &os) {
      paykan::pir::print(program, os);
      return EXIT_SUCCESS;
    });
  in.Program = &program;
  // The compiled code of the modules loaded from a `.pkm` that has some.
  if (caps.CompileModules)
    for (const paykan::pir::Module &m : program.Modules)
      if (const pkm::Object *o = resolver.object(m.Name))
        compiled.try_emplace(m.Name, toBackend(*o));
  for (const auto &[name, o] : compiled)
    in.Compiled[name] = &o;
  if (opts.ListModules) {
    // The files the C of --emit-c needs as objects: it declares the
    // functions of these modules (docs/c-backend.md).
    std::vector<std::string> files;
    for (const auto &[name, o] : compiled) {
      std::string file = resolver.moduleFile(name);
      if (file.empty())
        return fail("the compiled code of module '" + name +
                    "' is in no module file: its cache entry could not be "
                    "written");
      files.push_back(std::move(file));
    }
    for (const std::string &file : files)
      std::cout << file << "\n";
    return finishStdout(EXIT_SUCCESS);
  }

  if (opts.EmitSource) {
    if (!caps.EmitSource)
      return fail("backend '" + backendName + "' has no source output");
    paykan::backend::EmitOptions eo;
    eo.SourceStage = opts.EmitStage;
    return printOutput(opts, &resolver, [&](std::ostream &os) {
      paykan::Status s =
          backend->emit(in, paykan::backend::EmitKind::Source, eo, os);
      return s ? EXIT_SUCCESS : failBackend(s);
    });
  }

  if (opts.Cmd == Command::Build) {
    if (!caps.EmitExecutable)
      return fail("backend '" + backendName + "' cannot build executables");
    paykan::backend::EmitOptions eo;
    eo.OutputPath = opts.OutputPath.empty()
                        ? defaultExecutable(opts.InputFilename)
                        : opts.OutputPath;
    if (std::string why = overwritesInput(eo.OutputPath, opts, &resolver);
        !why.empty())
      return fail(why);
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
  ro.TrackHeap = opts.TrackHeap ||
                 std::getenv(paykan::names::kPaykanTrackHeapEnv) != nullptr;
  auto result = backend->run(in, progArgs, ro);
  if (!result)
    return failBackend(result.status());
  return *result;
}
