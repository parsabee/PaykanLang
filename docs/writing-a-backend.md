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

The example in [`examples/backends/print-pir`](../examples/backends/print-pir)
is the smallest complete backend (it prints the PIR it receives); copy it to
start your own.

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
factory to `paykan::backend::Registry` before `main()` runs. Nothing references
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

add_library(paykan_backend_mine STATIC MyBackend.cpp)
target_link_libraries(paykan_backend_mine PUBLIC Paykan::backend)

# A `paykan` driver with every plugin of the installation plus this one.
paykan_add_driver(paykan-mine PLUGINS paykan_backend_mine)
```

`find_package(Paykan)` provides the imported targets `Paykan::backend`,
`Paykan::pir`, `Paykan::frontend`, `Paykan::sema`, `Paykan::ast`,
`Paykan::runtime`, `Paykan::driver` and one target per installed plugin
(`Paykan::backend_c`, `Paykan::frontend_bison`, ...), the variables
`PAYKAN_BACKENDS` / `PAYKAN_FRONTENDS` / `PAYKAN_PLUGINS`, and
`paykan_add_driver(<target> [PLUGINS <libs>...])`, which creates an executable
from the driver library and links every plugin whole-archive. The compiler's
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
extern fn @Paykan_println(obj) -> void
cstr @.s = "hi" len 2
extern fn @PaykanString_new(ptr, i64) -> obj
extern fn @PaykanString_destroy(obj) -> void
fn @main() -> i64 {
  %s = call @PaykanString_new(@.s, 2)
  call @Paykan_println(%s)
  call @PaykanString_destroy(%s)
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
- Keep runtime symbol names as declared (`extern fn @Paykan_...`); mangle only
  module-defined names (`Box<int>`, `helper::add`), consistently across the
  modules of a program (`docs/pir.md` §3). Resolve a module extern through
  its defining module and `Function::linkName()` (its `symbol`), not its
  local name: `@"x::tag"` in the importer is `@tag` in module `x`.
- Honour `-O<n>` in whatever way fits (`cc -O2`, `PassBuilder`, nothing).
- Report every failure through `Status`; never `exit()`, never throw.
- No core change should be needed: a backend lives entirely in its own
  library.
