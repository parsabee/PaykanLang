# Writing a backend

A backend turns a compiled Paykan program into something: C source, LLVM IR,
an object file, an executable, or a run. Everything in front of it is shared,
so a backend never parses, type-checks or decides ownership: it receives the
**Paykan IR** (PIR, [`pir.md`](pir.md)), in which every type, every
`retain`/`release`, every vtable and every scope cleanup is already explicit,
and translates it one instruction at a time.

Backends are plugins, of two kinds:

- **Loadable plugins** (sections 1 to 3; frontends work the same way,
  [`writing-a-frontend-plugin.md`](writing-a-frontend-plugin.md)): a shared
  library with the C interface of
  [`include/paykan/plugin_api.h`](../include/paykan/plugin_api.h),
  written in any language that can export a C function, and loaded at run
  time by an **installed** `paykan`, with no rebuild of PaykanLang. This is
  how a backend is written outside PaykanLang. It receives the program as
  PIR text and talks to `paykan` through a table of host functions.
- **Built-in plugins** (section 4): C++ classes implementing
  [`include/paykan/Backend.h`](../include/paykan/Backend.h), linked into
  `paykan` (`c`, `llvm`). They are part of PaykanLang's source tree; an
  advanced option links such a static C++ plugin into a `paykan` of its own.

Either way the driver lists the backends (`paykan --list-backends`) and
selects one with `--backend=<name>`. [`plugins/overview.md`](plugins/overview.md)
explains how `paykan` finds and checks loadable plugins.

The example in [`src/Backends/PrintPIR`](../src/Backends/PrintPIR) is the smallest
complete loadable backend, in plain C (it prints the PIR it receives); copy it
to start your own.

**Stability.** Unlike the language (stable in v0.1, see
[`language/01-language-basics.md`](language/01-language-basics.md)), the
plugin interfaces are not yet stable: `plugin_api.h` (versioned by
`PAYKAN_PLUGIN_API_VERSION`), the PIR text (`PAYKAN_PIR_TEXT_VERSION`), and
the C++ interfaces of the built-in plugins may change between 0.x releases.
Each release therefore says exactly which plugin builds it accepts; see
[Plugin compatibility](#7-plugin-compatibility) below.

## 1. A loadable backend

A loadable backend is a shared library that exports one function,
`paykan_plugin_init`, returning a static descriptor: the plugin API version,
the PaykanLang version the plugin is built with, and its backends, each a
name, capabilities and two callbacks. The reference is
[`plugins/plugin-api.md`](plugins/plugin-api.md); in short:

```c
#include "paykan/plugin_api.h"

static const PaykanHost *host; /* paykan's functions: output, diagnostics, toolchain */

static int my_emit(void *data, PaykanSession *s, const PaykanBackendInput *in,
                   uint32_t kind, const char *output_path) {
  /* in->pir / in->pir_size: the verified program as PIR text.
   * kind PAYKAN_EMIT_SOURCE: write with host->write_output(s, ...);
   * PAYKAN_EMIT_EXECUTABLE: write output_path (host->link_executable helps). */
  return PAYKAN_OK;
}

static int my_run(void *data, PaykanSession *s, const PaykanBackendInput *in,
                  const PaykanRunRequest *req, int *exit_code) {
  /* build into host->temp_dir(s), then host->run_executable(...) */
  return PAYKAN_OK;
}

static const PaykanBackend backends[] = {
    {
        .struct_size = sizeof(PaykanBackend),
        .name = "mine",
        .description = "built on X 1.2",
        .capabilities = PAYKAN_BACKEND_EMIT_SOURCE | PAYKAN_BACKEND_EMIT_EXECUTABLE |
                        PAYKAN_BACKEND_RUN,
        .source_extension = ".x",
        .emit = my_emit,
        .run = my_run,
    },
};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    .build_version = PAYKAN_PLUGIN_BUILD_VERSION,
    .name = "mine", .version = "1.0",
    .num_backends = 1, .backends = backends,
};

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}
```

- **Capabilities.** `paykan` checks them before calling the backend, so
  `emit`/`run` only see requests the backend declared.
- **Errors.** Report them with `host->diagnostic` and return a non-zero
  status; `paykan` prints the errors as the failure. Nothing may unwind out
  of a callback (no exception, no panic).
- **`run`** builds the program and runs it with `host->run_executable`,
  passing `req->args` (`args[0]` is the script path) and `req->track_heap`
  (`--track-heap`) on as they are.
- **The toolchain.** A native backend links its objects (or C sources)
  against the Paykan runtime with `host->link_executable`; it finds the
  runtime with `host->runtime_library` / `host->runtime_include_dir`, exactly
  as the built-in backends do.

## 2. The input

[`plugins/pir-for-backends.md`](plugins/pir-for-backends.md) describes what
arrives in `PaykanBackendInput`: the program as PIR text (every module, main
module first, already verified), the input file, the project root and the
optimisation level. The semantics of every item and instruction, the runtime
ABI the generated code calls into, and the ownership rules the lowering has
already applied are in [`pir.md`](pir.md). A backend may rely on everything
the verifier checks (§9 there). Generated code links `libpaykan_runtime.a`
and uses `Runtime.h`; the runtime symbols a program may call are the
`extern fn` declarations of its modules.

## 3. Building and installing it

Install PaykanLang, then build against its package:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyPaykanBackend LANGUAGES C)        # or CXX: any language works

find_package(Paykan REQUIRED)               # -DCMAKE_PREFIX_PATH=<prefix>

# A loadable module, libpaykan_backend_mine.so (.dylib on macOS), that
# includes paykan/plugin_api.h and links nothing of PaykanLang.  Configure
# fails unless the installed PaykanLang accepts plugins built with its
# version (section 7); BUILT_WITH pins it.
paykan_add_backend_plugin(paykan_backend_mine mine.c)

# `cmake --install` puts it into the installation's plugin directory
# (PAYKAN_PLUGIN_INSTALL_DIR, <prefix>/lib/paykan/plugins/<version>).
paykan_install_plugin(paykan_backend_mine)
```

```sh
cmake -B build -DCMAKE_PREFIX_PATH=<prefix>
cmake --build build
paykan --plugin=build/libpaykan_backend_mine.so --backend=mine program.pkn   # try it
cmake --install build                                                       # install it
paykan --backend=mine program.pkn
```

Once a copy is installed, `--plugin=build/...` loads a second file that
provides `mine`, so the name is ambiguous ([`plugins/overview.md`](plugins/overview.md));
try a rebuilt plugin with `paykan --no-plugins --plugin=build/... --backend=mine`.

The package provides `Paykan::plugin_api` (the header), the installation's
version `PAYKAN_TOOLCHAIN_VERSION`, the plugin build versions it accepts
`PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`, `PAYKAN_PLUGIN_API_VERSION`,
`PAYKAN_PLUGIN_INSTALL_DIR`, the installed `PAYKAN_EXECUTABLE`, and the
functions `paykan_add_backend_plugin(<target> [BUILT_WITH <version>]
<sources>...)`, `paykan_install_plugin(<target>...)` and
`paykan_check_plugin_built_with(...)` (the version check alone, for a plugin
CMake doesn't compile).

Without CMake, compile against the installed header:

```sh
cc -std=c11 -shared -fPIC -I<prefix>/include -o libpaykan_backend_mine.so mine.c
```

In Rust, a `cdylib` declares the structs with `#[repr(C)]` and exports
`paykan_plugin_init` with `#[no_mangle] pub unsafe extern "C"`, and builds
with its own toolchain (`cargo`); `paykan_check_plugin_built_with()` gives a
CMake wrapper the version check.

A loadable plugin is independent of the installation's configuration: it
works the same with a core install (recursive-descent + c) and with one
that has the llvm backend, and needs no LLVM.

## 4. Built-in backends (C++)

### The C++ interface

The built-in backends implement `Backend.h` in process; a loadable plugin
never sees these types.

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

### The input

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

### Registration

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

### In tree

Add `src/Backends/<Name>/` with a `CMakeLists.txt` that builds the library,
links `paykan_backend`, and records it with `paykan_add_plugin(<target>)`;
list the backend in `PAYKAN_KNOWN_BACKENDS` (`cmake/PaykanPlugins.cmake`) and
add its `add_subdirectory` to the backend loop in `src/CMakeLists.txt`. Every
executable that calls `paykan_link_plugins()` (the driver and the test
binaries) then links it whole-archive, and `-DPAYKAN_BACKENDS=...` selects it.

### A static C++ plugin in a driver of its own (advanced)

A C++ backend can also be built out of tree as a static library and linked
into a custom `paykan` (a single binary, no plugin loading needed). This
ties the plugin to the C++ interfaces and to the compiler and standard
library the installation was built with; prefer a loadable plugin.

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

# A static library that registers itself (PAYKAN_REGISTER_BACKEND).
add_library(paykan_backend_mine STATIC MyBackend.cpp)
target_link_libraries(paykan_backend_mine PUBLIC Paykan::backend)

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
and `paykan_add_driver(<target> [PLUGINS <libs>...])`, which creates an executable
from the driver library and links every plugin whole-archive. That driver's
`run` and `build` use the package's runtime (unless `$PAYKAN_RUNTIME_DIR`
names another), wherever the executable itself is built or copied. The compiler's
headers are installed under `include/paykan/compiler` and are on the include
path of every imported target.

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

A loadable backend gets the same text: `paykan --emit-pir program.pkn`
prints the PIR of a real program, and `src/Backends/PrintPIR` returns exactly that
through the plugin path, so a backend can be tested with saved PIR files and
with `paykan --plugin=... --backend=mine` on real programs. The parity
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
- Report every failure through `host->diagnostic` and a status code (or
  `Status` in C++); never `exit()`, never let anything unwind out.
- Keep `paykan_plugin_init` and global constructors free of side effects
  ([`plugins/overview.md`](plugins/overview.md)).
- No core change should be needed: a backend lives entirely in its own
  library.

## 7. Plugin compatibility

A plugin is built against one PaykanLang release and may be loaded into (or
linked into) a `paykan` of another release. Each plugin therefore records the
PaykanLang version it was **built with**, and each release carries an explicit
**list of the plugin build versions it accepts**
([#103](https://github.com/parsabee/PaykanLang/issues/103)).

- **What a plugin records.** A loadable plugin puts
  `PAYKAN_PLUGIN_BUILD_VERSION` in its descriptor's `build_version`:
  `plugin_api.h`'s own version unless the build defines it
  (`paykan_add_backend_plugin` defines it as `BUILT_WITH`, or the
  installation's version). A built-in or static C++ plugin's
  `PAYKAN_REGISTER_BACKEND` / `PAYKAN_REGISTER_FRONTEND` pass the
  `PAYKAN_PLUGIN_BUILD_VERSION` of the installed `paykan/PluginCompat.h`
  (`include/paykan/compiler/paykan/` under the prefix) to the registry along
  with the name and the factory.
- **The plugin API version.** A loadable plugin also declares the
  `PAYKAN_PLUGIN_API_VERSION` it is built for; a `paykan` that does not
  support it rejects the file (`rejected plugin <file>: built for plugin API
  N; ...`). It changes only when `plugin_api.h` changes incompatibly.
- **The list.** It lives in one place,
  [`cmake/PluginCompat.cmake`](../cmake/PluginCompat.cmake)
  (`PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`). CMake compiles it into the core and
  exports it in the package (`PAYKAN_PLUGIN_COMPATIBLE_VERSIONS` after
  `find_package(Paykan)`).
- **Matching.** A plugin is compatible if its build version is **exactly**
  one of the list's entries, compared as strings with any pre-release label
  included: a list holding `1.2.3` accepts plugins built with `1.2.3`, but
  not `1.2.3-rc1`, `1.2.4` or `1.3.0`. There are no ranges
  and no ordering.
- **At run time.** The plugin loader checks a loaded plugin's build version
  before calling anything but its entry point, and the registry checks it
  again when the plugin is selected. An incompatible plugin stays registered
  but is never called:

  ```text
  $ paykan --list-backends
  c (default)
  mine (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.1 accepts 0.1.1, 0.1.0) [/home/me/.paykan/plugins/0.1.1/libmine.so]
  $ paykan --backend=mine program.pkn
  paykan: cannot use backend 'mine' (incompatible: built with PaykanLang 0.0.9; this paykan 0.1.1 accepts 0.1.1, 0.1.0) [/home/me/.paykan/plugins/0.1.1/libmine.so]
  $ echo $?
  2
  ```

  `paykan --version` prints the accepted versions and every registered
  plugin with the version it was built with and whether it is compatible;
  include it in bug reports. The built-in plugins go through the same check
  and are always compatible: they are built with the release's own version,
  which is always the first entry of its list.
- **At configure time.** `paykan_add_backend_plugin()` /
  `paykan_add_frontend_plugin()` fail with a `FATAL_ERROR` when the installed
  Paykan does not accept the version the plugin is built with:

  ```text
  Paykan backend plugin 'paykan_backend_mine' is incompatible: built with
  PaykanLang 0.0.9; the installed PaykanLang 0.1.1 (<prefix>/lib/cmake/Paykan)
  accepts 0.1.1, 0.1.0
  ```

  That version is the installation's own (`PAYKAN_TOOLCHAIN_VERSION`) unless
  the plugin pins the release it is written for with `BUILT_WITH <version>`;
  [`src/Backends/PrintPIR`](../src/Backends/PrintPIR) exposes that as
  `-DPRINT_PIR_BUILT_WITH=<version>`. For a loadable plugin the pinned version
  is also the one its descriptor declares. (A static C++ plugin's
  registered build version is always that of the headers it is compiled
  against.)

**Maintaining the list (each release).** The release's own version
(`project(... VERSION ...)` plus `PAYKAN_VERSION_PRERELEASE` in the top-level
`CMakeLists.txt`, the one place the version is spelled) is always on it: the
list starts with `${PAYKAN_VERSION}`, so a version bump needs no change to it.
Add an older version after it only if plugins built with that version still
work with this release: `plugin_api.h` (or its API version is still
supported), the PIR text, the runtime ABI (`Runtime.h`), and for static C++
plugins `Frontend.h`, `Backend.h`, `Registry.h` and the AST and `ASTContext`
a frontend sees, are unchanged since, or changed only compatibly. Drop it when any of them changes
incompatibly. The CHANGELOG entry of each release states which plugin build
versions it accepts.
