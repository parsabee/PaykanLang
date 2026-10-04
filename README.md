# PaykanLang

[![CI](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml/badge.svg)](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.0--alpha-blue.svg)](CHANGELOG.md)

PaykanLang (`.pkn`) is a high-performance language for building applications on modern,
heterogeneous machines. The goal is application development across many cores and, 
increasingly, across the different compute units in a system (CPUs, GPUs, and other accelerators), 
with work offloaded to them seamlessly rather than wired up by hand. 
It aims to be **memory-safe and thread-safe** by design, with a clean, small surface:
a real module system, C interoperability exposed through modules, and automatic reference
counting in place of a garbage collector or manual memory management. PaykanLang aims to 
provide a safe, ergonomic, performant, and productive interface that compiles to native speed.

This is the **alpha of v0.1.0** (`0.1.0-alpha`, a pre-release) that lays the foundation. Implemented and tested
today: static typing with inference, single-inheritance classes with virtual dispatch,
ARC with `mov` move semantics, value-aware `==` (reference types dispatch to a virtual
`equals`), dynamic arrays, a file-based module system, `match` type dispatch, enums, tuples,
optionals, generics, and conversion constructors (`Str(n)`, `int<Str>(s)`). Programs are lowered to a
backend-neutral IR (PIR) and compiled by one of two backends: the C backend (emits C11 and
builds it with the system C compiler) or the optional LLVM backend (LLVM IR, run in-process by
a JIT). Either backend can `run` a program directly or `build` a native executable.
The defining goals — seamless offloading to heterogeneous compute units, thread-safe
concurrency, and C interop through modules — are the road ahead, not yet shipped. See
[CHANGELOG.md](CHANGELOG.md) for what's in this release.

**Stability.** Tuples, optional types (optional primitives and `match` on an optional
included) and generics are **stable in v0.1**: their syntax and semantics as documented in
[`docs/language/`](docs/language/) are covered by the compatibility promise for the 0.1
series, so a program that follows the reference keeps its meaning in every 0.1.x release.
The limitations the reference documents stay: generics cannot be imported across modules
until v0.2.0 ([#57](https://github.com/parsabee/PaykanLang/issues/57)), and a present optional primitive is boxed ([#96](https://github.com/parsabee/PaykanLang/issues/96) will change that
representation, not the semantics). Not covered by the promise: the plugin interfaces
(`include/paykan/Frontend.h`, `Backend.h`, the registry and PIR as an out-of-tree plugin
sees them) are not yet stable for out-of-tree authors; their versioning policy is [#103](https://github.com/parsabee/PaykanLang/issues/103).

---

## Installing

Release builds are published for macOS (Apple Silicon) and Linux (x86_64). Each
[GitHub Release](https://github.com/parsabee/PaykanLang/releases) carries a tarball per
platform (with its SHA-256 sum) holding `bin/paykan`, the runtime and its header; unpack it
anywhere and put its `bin/` on the `PATH`. The release builds and the Homebrew formula are the
core build (the recursive-descent frontend and the C backend). Programs are compiled with the
system C compiler, so a C11 compiler (`cc`) must be installed. Or build from source (below),
which also offers the opt-in LLVM backend.

### Homebrew (macOS)

```sh
brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
brew install parsabee/paykanlang/paykanlang
paykan --version
```

---

## Building from Source

Prerequisites: CMake 3.24 or newer and a C++20 compiler to build PaykanLang, plus a C11
compiler (`cc`) at run time, which the C backend uses to compile programs. The default build
needs nothing else (the test suite's GoogleTest aside, see below); the LLVM backend is
opt-in.

```sh
git clone https://github.com/parsabee/PaykanLang.git
cd PaykanLang
cmake -B build
cmake --build build --parallel
# the full build, with the opt-in LLVM backend:
# cmake -B build "-DPAYKAN_BACKENDS=llvm;c"
```

The frontend (lexer + parser) is pluggable. `-DPAYKAN_FRONTENDS=<list>` lists the in-tree
frontends to build; today that is `recursive-descent` (standard C++ only), which is always
built and is the default. Out-of-tree frontends are plugins built against an installed
PaykanLang with `find_package(Paykan)`; each produces a `paykan` driver that has its
frontend registered next to the installation's, selected at run time with
`--frontend=<name>` (`--list-frontends` prints them). Every frontend implements
[`docs/grammar.md`](docs/grammar.md) and must build the same AST as the recursive-descent
frontend; [`docs/writing-a-frontend.md`](docs/writing-a-frontend.md) explains how to write
one and how to run PaykanLang's parser and Sema suites against it. The reference example is
the Bison frontend, [PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend).
`--dump-tokens` prints a frontend's token stream.

The backend is pluggable too. `-DPAYKAN_BACKENDS=<list>` selects the backends to build:
`c` (the C backend: emits C11, builds with the system C compiler, standard C++ only) is always
built; `llvm` (LLVM IR, the ORC JIT for `run`, native objects linked by the system C compiler
for `build`) is an in-tree, opt-in backend (`-DPAYKAN_BACKENDS="llvm;c"`). The default backend
is always `c`, whatever the list's order: with `"llvm;c"`, `--list-backends` shows
`c (default)` and the llvm backend (and its JIT) is selected with `--backend=llvm`.
`--backend=<name>` selects a backend at run time; `--list-backends` prints them and marks the
default. (A configuration without the c backend, which this tree never produces, falls back to
the first backend listed.) LLVM is
downloaded only when `llvm` is listed, so the default configure (`recursive-descent` + `c`) is
a **barebones build** that needs nothing but a C++20 compiler and a C compiler (CI checks that
it downloads nothing and links no third-party library). The full build is
`-DPAYKAN_BACKENDS="llvm;c"`.
Every backend consumes the Paykan IR described in [`docs/pir.md`](docs/pir.md);
[`docs/writing-a-backend.md`](docs/writing-a-backend.md) explains how to write one, in tree or
out of tree against `find_package(Paykan)` (see [`utils/print-pir`](utils/print-pir)).

The test suite needs GoogleTest: an installed one is used if CMake finds it, otherwise it is
downloaded at configure time. `-DPAYKAN_BUILD_TESTS=OFF` skips the tests and GoogleTest (for
packagers and offline builds), so the barebones build then downloads nothing at all.

The `paykan` binary is placed at `build/bin/paykan`. To install it to a prefix (with the
runtime, `lib/libpaykan_runtime.a` and `include/paykan/Runtime.h`, that `build` and the c
backend link programs against):

```sh
cmake --install build --prefix /usr/local
```

---

## Running

```sh
paykan program.pkn              # run a source file (same as `paykan run program.pkn`)
paykan build program.pkn -o prog # compile a native executable
paykan --backend=c program.pkn  # pick a backend (--list-backends prints them)
paykan --frontend=NAME program.pkn # pick a frontend (--list-frontends prints them)
paykan --check-only program.pkn # stop after type-checking (no codegen)
paykan --dump-ast program.pkn   # print the parsed AST
paykan --emit-pir program.pkn   # print the backend-neutral IR (docs/pir.md)
paykan --emit-source program.pkn # print the backend's output (C or LLVM IR)
paykan --emit-c program.pkn     # print the generated C (--backend=c --emit-source)
paykan --emit-llvm program.pkn  # print the LLVM IR (--backend=llvm --emit-source)
paykan --track-heap program.pkn # run, then print heap/leak statistics
paykan -O0 program.pkn          # set the optimization level (0-3, default -O2)
paykan --version                # print the version and the built frontends and backends
paykan --help                   # list every option
```

`run` uses the c backend unless `--backend=llvm` is given: the c backend compiles the program
with the system C compiler into a temporary directory and runs that; `--backend=llvm` executes
it in-process through LLVM's ORC JIT.
`--emit-llvm` and `--emit-c` need their backend to be built in. `main`'s return value becomes
the process exit code, and extra command-line arguments after the source file are passed to
`main(args: Str[])` (with `args[0]` the source-file path under `run`, and the executable's path for a
program made by `build`). Source files use the
`.pkn` extension (`.pk` is also accepted).

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

---

## Development Tooling

Quality gates are enforced in CI and available locally via the Python helpers in `scripts/`
(they prefer the LLVM 17 tools under `build/third-party/llvm`, which a configure with the
`llvm` backend downloads, so results match CI; otherwise they use the tools on the `PATH`):

```sh
python3 scripts/clang_format.py --apply          # format the tree
python3 scripts/run_clang_tidy.py --build-dir build   # bug-focused clang-tidy gate
python3 scripts/coverage.py --build-dir build-cov     # coverage report + floor gate
```

Install the pre-commit hooks (clang-format on commit, clang-tidy on push):

```sh
pip install pre-commit        # or: brew install pre-commit
pre-commit install
pre-commit install --hook-type pre-push
```

---

## Learning the Language

To learn PaykanLang:

- **[`docs/language/`](docs/language/)** — the full reference: language basics,
  functions, enums, classes, arrays, modules, `match` statements, the memory model,
  tuples, optional types, and generics; the conversion
  constructors (`Str(n)`, `int<Str>(s)`, ...) are under "Conversions" in
  [`01-language-basics.md`](docs/language/01-language-basics.md).
- **[`samples/`](samples/)** — runnable `.pkn` programs exercising every feature
  (`samples/codegen/` has the feature demos; see also `imports/`, `sema/`, `leak-check/`).
  [`samples/imports/12_calc/`](samples/imports/12_calc/) is a larger multi-module program:
  an arithmetic calculator with variables, a hash map and a REPL.

A taste:

```pkn
fn main() -> int {
  println("Hello, world!");
  return 0;
}
```

Output:

```
Hello, world!
```

---

## License

PaykanLang is released under the [MIT License](LICENSE).
