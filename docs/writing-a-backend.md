# Writing a backend

A backend turns a compiled Paykan program into something: C source, LLVM IR,
an object file, an executable, or a run. Everything in front of it is shared,
so a backend never parses, type-checks or decides ownership: it receives the
**Paykan IR** (PIR, [`pir.md`](pir.md)), in which every type, every
`retain`/`release`, every vtable and every scope cleanup is already explicit,
and translates it one instruction at a time.

Backends are plugins. The core defines the interface
([`include/paykan/Backend.h`](../include/paykan/Backend.h)) and a registry;
a backend is a static library that implements the interface and registers a
factory under a name. The driver lists the registered backends
(`paykan --list-backends`) and selects one with `--backend=<name>`.

The example in [`utils/print-pir`](../utils/print-pir)
is the smallest complete backend (it prints the PIR it receives); copy it to
start your own.

**Stability.** Unlike the language (stable in v0.1, see
[`language/01-language-basics.md`](language/01-language-basics.md)), the
plugin interfaces are not yet stable for out-of-tree authors: `Backend.h`,
`Frontend.h`, the registry and PIR as a plugin sees it may change between
0.x releases. Each release therefore says exactly which plugin builds it
accepts; see [Plugin compatibility](#7-plugin-compatibility) below.

## 1. The interface

```cpp
#include "paykan/Backend.h"

class MyBackend : public paykan::backend::Backend {
public:
  std::string_view name() const override;             // "mine"
  paykan::backend::Capabilities capabilities() const override;
  std::string describe() const override;              // optional: "built on X 1.2"

  paykan::Status emit(const paykan::backend::Input &in,
                      paykan::backend::EmitKind kind,
                      const paykan::backend::EmitOptions &opts,
                      std::ostream &out) override;

  paykan::StatusOr<int> run(const paykan::backend::Input &in,
                            std::span<const std::string> args,
                            const paykan::backend::RunOptions &opts) override;
};
```

- **`capabilities()`** says what the backend can do: `EmitSource` (the
  backend's own source language, written to a stream: C, LLVM IR, ...),
  `EmitObject`, `EmitExecutable` (`paykan build`), `Run` (`paykan run`), and
  the `SourceExtension` of its source output. The driver checks the
  capabilities before calling the backend, so `emit`/`run` only ever see
  requests the backend declared.
- **`emit()`** translates the program. For `EmitKind::Source` the output goes
  to `out`; for `Object`/`Executable` it goes to `opts.OutputPath` (`""` means
  a default next to the input).
- **`run()`** executes the program and returns its exit code. `args[0]` is the
  script path, like `argv[0]`. `opts.TrackHeap` is the driver's `--track-heap`:
  an in-process backend switches the runtime's tracking allocator on
  (`Paykan_heap_set_tracking(1); Paykan_heap_reset();`) before the program
  allocates and dumps it afterwards (`Paykan_heap_dump()`); a backend that runs
  a separate process passes the request on to it. The base class's `run()`
  fails, so a backend without `Run` need not override it.
- **Errors** are reported with `paykan::Status` / `paykan::StatusOr<T>`
  ([`include/paykan/Status.h`](../include/paykan/Status.h)). The core is built
  with `-fno-exceptions`; nothing may throw across the interface.

## 2. The input

`backend::Input` carries:

| Field | Meaning |
|---|---|
| `Program` | `const pir::Program *`: every module of the program, main module first, already run through the verifier |
| `InputFilename`, `ProjectRoot` | the main source file and the directory imports were resolved against (for default output names, caches, diagnostics) |
| `OptLevel` | `-O<n>`, 0..3; what it means is the backend's choice |

A PIR program is a plain data structure
([`include/paykan/pir/PIR.h`](../include/paykan/pir/PIR.h)): modules hold
globals, classes (full layout and vtable as data) and functions; a function
holds parameters, `local` slots and a body of structured statements (`if`,
`while`, `break`, `continue`, `ret`, `unreachable`, instructions). The
semantics of every item and instruction, the runtime ABI the generated code
calls into, and the ownership rules the lowering has already applied are in
[`pir.md`](pir.md). A backend may rely on everything the verifier checks
(§9 there).

Generated code links `libpaykan_runtime.a` and uses `Runtime.h` (installed as
`include/paykan/Runtime.h`); the runtime symbols a program may call are the
`extern fn` declarations of its modules.

## 3. Registration

```cpp
static std::unique_ptr<paykan::backend::Backend> createMyBackend() {
  return std::make_unique<MyBackend>();
}
PAYKAN_REGISTER_BACKEND(mine, "mine", &createMyBackend);
```

Registration is static: the macro defines an object whose constructor adds the
factory to `paykan::backend::Registry` before `main()` runs, together with the
PaykanLang version the plugin was built with (`PAYKAN_PLUGIN_BUILD_VERSION`
from the installed headers it is compiled against; see
[Plugin compatibility](#7-plugin-compatibility)). Nothing references
that object, so the plugin library must be linked **whole** into the driver;
the CMake helpers below do that. Frontends register the same way with
`PAYKAN_REGISTER_FRONTEND` ([`include/paykan/Frontend.h`](../include/paykan/Frontend.h)).

## 4. Building it

### Out of tree

Install Paykan, then build against the package:

```sh
cmake -B build -DPAYKAN_BACKENDS=c      # or any configuration
cmake --build build
cmake --install build --prefix /opt/paykan
```

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyPaykanBackend LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(Paykan REQUIRED)             # -DCMAKE_PREFIX_PATH=/opt/paykan

# A static library linked with Paykan::backend.  Configure fails unless the
# installed Paykan accepts plugins built with its version (section 7).
paykan_add_backend_plugin(paykan_backend_mine MyBackend.cpp)

# A `paykan` driver with every plugin of the installation plus this one.
paykan_add_driver(paykan-mine PLUGINS paykan_backend_mine)
```

If the installation includes the llvm backend (`PAYKAN_BACKENDS` lists `llvm`),
`find_package(Paykan)` also looks up LLVM, because that backend's installed
library links LLVM's imported targets. Pass the same LLVM the installation was
built with, or configure fails with "Could not find a package configuration
file provided by "LLVM"":

```sh
cmake -B build -DCMAKE_PREFIX_PATH=/opt/paykan -DLLVM_DIR=<llvm>/lib/cmake/llvm
```

A plain configure of Paykan builds only the core (recursive-descent + c) and
downloads no LLVM. An installation with the llvm backend comes from a configure
that passes the full list,
`"-DPAYKAN_BACKENDS=llvm;c"`; that configure downloads LLVM 17
into `<paykan-build>/third-party/llvm` unless `LLVM_DIR` names one, so
`<llvm>` above is that directory or the LLVM you passed.

An installation built without the llvm backend needs only `CMAKE_PREFIX_PATH`.
This lasts while the LLVM backend is in tree; it moves to its own repository at
v1.0 ([#61](https://github.com/parsabee/PaykanLang/issues/61)).

`find_package(Paykan)` provides the imported targets `Paykan::backend`,
`Paykan::pir`, `Paykan::frontend`, `Paykan::sema`, `Paykan::ast`,
`Paykan::runtime`, `Paykan::driver` and one target per installed plugin
(`Paykan::backend_c`, `Paykan::frontend_recursive_descent`, ...), the variables
`PAYKAN_BACKENDS` / `PAYKAN_FRONTENDS` / `PAYKAN_PLUGINS`, the installed
runtime's location `PAYKAN_RUNTIME_LIBRARY` / `PAYKAN_RUNTIME_INCLUDE_DIR`,
the installation's version `PAYKAN_TOOLCHAIN_VERSION` and the plugin build
versions it accepts `PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`,
`paykan_add_backend_plugin(<target> [BUILT_WITH <version>] <sources>...)` and
`paykan_add_frontend_plugin(...)` (section 7), and
`paykan_add_driver(<target> [PLUGINS <libs>...])`, which creates an executable
from the driver library and links every plugin whole-archive. That driver's
`run` and `build` use the package's runtime (unless `$PAYKAN_RUNTIME_DIR`
names another), wherever the executable itself is built or copied. The compiler's
headers are installed under `include/paykan/compiler` and are on the include
path of every imported target.

### In tree

Add `src/Backends/<Name>/` with a `CMakeLists.txt` that builds the library,
links `paykan_backend`, and records it with `paykan_add_plugin(<target>)`;
list the backend in `PAYKAN_KNOWN_BACKENDS` (`cmake/PaykanPlugins.cmake`) and
add its `add_subdirectory` to the backend loop in `src/CMakeLists.txt`. Every
executable that calls `paykan_link_plugins()` (the driver and the test
binaries) then links it whole-archive, and `-DPAYKAN_BACKENDS=...` selects it.

## 5. Testing without the frontend

PIR has a text form, so backend tests need neither a frontend nor the
lowering:

```cpp
#include "paykan/pir/Parser.h"
#include "paykan/pir/Verifier.h"

paykan::pir::ParseError err;
auto program = paykan::pir::parseProgram(R"(module "t"
extern fn @$rt.Paykan_println(obj) -> void
cstr @.s = "hi" len 2
extern fn @$rt.PaykanString_new(ptr, i64) -> obj
extern fn @$rt.PaykanString_destroy(obj) -> void
fn @main() -> i64 {
  %s = call @$rt.PaykanString_new(@.s, 2)
  call @$rt.Paykan_println(%s)
  call @$rt.PaykanString_destroy(%s)
  ret 0
}
)", err);
assert(program && paykan::pir::verify(*program).empty());
// backend.emit({.Program = &*program, ...}, ...)
```

`paykan --emit-pir program.pkn` prints the PIR of a real program, and the
example backend above does the same through the backend path. The parity
tests under `tests/CodeGen` and the samples corpus run on every enabled
backend; a new in-tree backend is expected to pass them.

## 6. Checklist

- Translate PIR as it is: do not re-derive ownership, dispatch or layout; the
  lowering is the only place those rules live.
- Link a runtime extern (`extern fn @$rt.Paykan_...`, `extern obj`, `extern
  vtable`) by its C symbol, the PIR name without `$rt.`
  (`pir::runtimeSymbol`); mangle only
  module-defined names (`Box<int>`, `helper::add`), consistently across the
  modules of a program (`docs/pir.md` §3). Resolve a module extern through
  its defining module and `Function::linkName()` (its `symbol`), not its
  local name: `@"x::tag"` in the importer is `@tag` in module `x`.
- Honour `-O<n>` in whatever way fits (`cc -O2`, `PassBuilder`, nothing).
- Report every failure through `Status`; never `exit()`, never throw.
- No core change should be needed: a backend lives entirely in its own
  library.

## 7. Plugin compatibility

A plugin is compiled against one PaykanLang release's headers and may be
linked into a `paykan` of another release. Each plugin therefore records the
PaykanLang version it was **built with**, and each release carries an explicit
**list of the plugin build versions it accepts**
([#103](https://github.com/parsabee/PaykanLang/issues/103)).

- **What a plugin records.** `PAYKAN_REGISTER_BACKEND` and
  `PAYKAN_REGISTER_FRONTEND` pass `PAYKAN_PLUGIN_BUILD_VERSION`, defined by the
  installed `paykan/PluginCompat.h` (`include/paykan/compiler/paykan/` under
  the prefix), to the registry along with the name and the factory. Nothing
  else is needed in the plugin's code.
- **The list.** It lives in one place,
  [`cmake/PluginCompat.cmake`](../cmake/PluginCompat.cmake)
  (`PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`). CMake compiles it into the core and
  exports it in the package (`PAYKAN_PLUGIN_COMPATIBLE_VERSIONS` after
  `find_package(Paykan)`).
- **Matching.** A plugin is compatible if its build version is **exactly**
  one of the list's entries, compared as strings with any pre-release label
  included: a list holding `0.1.0-alpha` accepts plugins built with
  `0.1.0-alpha`, but not `0.1.0`, `0.1.0-beta` or `0.1.1`. There are no ranges
  and no ordering.
- **At run time.** The registry checks the build version when the plugin
  registers and again when it is selected. An incompatible plugin stays
  registered but is never instantiated:

  ```text
  $ paykan --list-frontends
  mine (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.0-alpha accepts 0.1.0-alpha)
  recursive-descent (default)
  $ paykan --frontend=mine program.pkn
  paykan: cannot use frontend 'mine' (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.0-alpha accepts 0.1.0-alpha)
  $ echo $?
  2
  ```

  `paykan --version` prints the accepted versions and every registered
  plugin with the version it was built with and whether it is compatible;
  include it in bug reports. The built-in plugins go through the same check
  and are always compatible: they are built with the release's own version,
  which must be on its list (configure fails otherwise).
- **At configure time.** `paykan_add_backend_plugin()` /
  `paykan_add_frontend_plugin()` fail with a `FATAL_ERROR` when the installed
  Paykan does not accept the version the plugin is built with:

  ```text
  Paykan backend plugin 'paykan_backend_mine' is incompatible: built with
  PaykanLang 0.0.9; the installed PaykanLang 0.1.0-alpha (<prefix>/lib/cmake/Paykan)
  accepts 0.1.0-alpha
  ```

  That version is the installation's own (`PAYKAN_TOOLCHAIN_VERSION`) unless
  the plugin pins the release it is written for with `BUILT_WITH <version>`;
  [`utils/print-pir`](../utils/print-pir) exposes that as
  `-DPRINT_PIR_BUILT_WITH=<version>`. Whatever is pinned, the registered build
  version is that of the headers the plugin is actually compiled against.

**Maintaining the list (each release).** The release's own version
(`project(... VERSION ...)` plus `PAYKAN_VERSION_PRERELEASE`) must be on it.
Keep an older version on it only if plugins built with that version still
work with this release: `Frontend.h`, `Backend.h`, `Registry.h`, the AST and
`ASTContext` a frontend sees, PIR and the runtime ABI (`Runtime.h`) are
unchanged since, or changed only compatibly. Drop it when any of them changes
incompatibly. The CHANGELOG entry of each release states which plugin build
versions it accepts.
