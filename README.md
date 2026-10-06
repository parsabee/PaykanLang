# PaykanLang

[![CI](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml/badge.svg)](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)](CHANGELOG.md)

PaykanLang (`.pkn`) is a high-performance language for building applications on modern,
heterogeneous machines. The goal is application development across many cores and, 
increasingly, across the different compute units in a system (CPUs, GPUs, and other accelerators), 
with work offloaded to them seamlessly rather than wired up by hand. 
It aims to be **memory-safe and thread-safe** by design, with a clean, small surface:
a real module system, C interoperability exposed through modules, and automatic reference
counting in place of a garbage collector or manual memory management. PaykanLang aims to 
provide a safe, ergonomic, performant, and productive interface that compiles to native speed.

This is **v0.1.0**, the first release, which lays the foundation. Implemented and tested
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
(the C plugin API `include/paykan/plugin_api.h`, the PIR text a backend plugin receives, and
the C++ interfaces `Frontend.h`, `Backend.h` and the registry) are versioned but not yet
stable; see [`docs/plugins/overview.md`](docs/plugins/overview.md) and
[#103](https://github.com/parsabee/PaykanLang/issues/103).

---

## Installing

Release builds are published for macOS (Apple Silicon) and Linux (x86_64). Each
[GitHub Release](https://github.com/parsabee/PaykanLang/releases) carries a tarball per
platform and a Debian package for Linux (each with its SHA-256 sum). A tarball holds
`bin/paykan`, the runtime and its header; unpack it
anywhere and put its `bin/` on the `PATH`. The release builds and the Homebrew formula are the
core build (the recursive-descent frontend and the C backend). Programs are compiled with the
system C compiler, so a C11 compiler (`cc`) must be installed. Or build from source (below),
which also offers the opt-in LLVM backend.

### Debian / Ubuntu (apt)

PaykanLang has an apt repository for Linux x86_64. Add it once, then install (and later
upgrade) with apt, which also pulls in a C compiler (`gcc`, or `clang`) for the C backend:

```sh
curl -fsSL https://parsabee.github.io/PaykanLang/apt/paykanlang.gpg \
  | sudo tee /usr/share/keyrings/paykanlang.gpg > /dev/null
echo "deb [arch=amd64 signed-by=/usr/share/keyrings/paykanlang.gpg] https://parsabee.github.io/PaykanLang/apt stable main" \
  | sudo tee /etc/apt/sources.list.d/paykanlang.list
sudo apt update
sudo apt install paykanlang
```

Each release also carries the package itself, `paykanlang_<version>_amd64.deb` (with its
SHA-256 sum), for `sudo apt install ./paykanlang_<version>_amd64.deb`.

It installs `paykan` in `/usr/bin`, the runtime in `/usr/lib`, its header and the plugin
headers in `/usr/include/paykan`, the `find_package(Paykan)` package, and the system plugin
directory `/usr/lib/paykan/plugins/<version>/`. `sudo apt purge paykanlang` removes it.

### Homebrew (macOS)

```sh
brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
brew install parsabee/paykanlang/paykanlang
paykan --version
```

### Plugins

A frontend or backend built for your PaykanLang version plugs into the `paykan` you
installed, with no rebuild: plugins are shared libraries with a C interface
([`include/paykan/plugin_api.h`](include/paykan/plugin_api.h)), so they can be written in
any language that exposes C functions. `paykan` loads the files named with
`--plugin=<file>`, then every plugin in `$PAYKAN_PLUGIN_PATH`, in
`~/.paykan/plugins/<version>/` and in the installation's `lib/paykan/plugins/<version>/`:

```sh
paykan --plugin=./libpaykan_backend_print_pir.so --backend=print-pir --emit-source hello.pkn
mkdir -p ~/.paykan/plugins/0.1.0
cp libpaykan_backend_print_pir.so ~/.paykan/plugins/0.1.0/   # or `cmake --install` it
paykan --backend=print-pir --emit-source hello.pkn
paykan --list-backends    # every backend, with the file a plugin came from
paykan --no-plugins ...   # only the built-in plugins (and --plugin files)

# A frontend: install PaykanLang, install the plugin, select it.
cmake --install build-bison            # PaykanLang_Bison_Frontend, into the plugin directory
paykan --frontend=bison program.pkn
```

A plugin built for another version, or that fails to load, is listed with the reason and
can't be selected (exit status 2). [`docs/plugins/overview.md`](docs/plugins/overview.md)
explains discovery, the checks and what they guarantee;
[`docs/writing-a-backend.md`](docs/writing-a-backend.md) and
[`docs/writing-a-frontend-plugin.md`](docs/writing-a-frontend-plugin.md) show how to write one
([`src/Backends/PrintPIR`](src/Backends/PrintPIR) and [`src/Frontends/ASTText`](src/Frontends/ASTText),
in C). A frontend returns the program
in the [AST interchange format](docs/plugins/ast-format.md); a backend receives it as PIR text.

---

## Building from Source

Prerequisites: CMake 3.24 or newer and a C++20 compiler to build PaykanLang, plus a C11
compiler (`cc`) at run time, which the C backend uses to compile programs. The default build
needs nothing else; the LLVM backend is opt-in.

Dependencies are deliberately limited to:
- standard C and C++ (the compiler and its standard libraries) to build and use PaykanLang;
- GoogleTest, for the test suite only (found on the system, or fetched);
- LLVM, for the opt-in `llvm` backend only;
- Python 3 with its standard library only, for the test and lint scripts in `scripts/`: the
  system's `python3`, or the one given with `-DPython3_EXECUTABLE=<path>`. Without Python,
  the script-driven tests are skipped.

```sh
git clone https://github.com/parsabee/PaykanLang.git
cd PaykanLang
cmake -B build
cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN)"
# the full build, with the opt-in LLVM backend:
# cmake -B build "-DPAYKAN_BACKENDS=llvm;c"
```

The frontend (lexer + parser) is pluggable. `-DPAYKAN_FRONTENDS=<list>` lists the in-tree
frontends to build; today that is `recursive-descent` (standard C++ only), which is always
built and is the default. Out-of-tree frontends are plugins that the installed `paykan`
loads at run time (see [Plugins](#plugins)), selected with `--frontend=<name>`
(`--list-frontends` prints them). Every frontend implements
[`docs/grammar.md`](docs/grammar.md) and must build the same AST as the recursive-descent
frontend; [`docs/writing-a-frontend-plugin.md`](docs/writing-a-frontend-plugin.md) explains how to write
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
[`docs/writing-a-backend.md`](docs/writing-a-backend.md) explains how to write one, as a
loadable plugin for an installed `paykan` (see [Plugins](#plugins)) or in tree, and
[`docs/writing-a-frontend-plugin.md`](docs/writing-a-frontend-plugin.md) does the same for
frontends. Each release lists the plugin build versions it accepts; a plugin built with another
version is listed as incompatible and can't be selected
([plugin compatibility](docs/writing-a-backend.md#7-plugin-compatibility)).

The tests are off by default, so a plain build downloads nothing at all. See
[Testing](#testing) to build them.

The `paykan` binary is placed at `build/bin/paykan`. To install it to a prefix (with the
runtime, `lib/libpaykan_runtime.a` and `include/paykan/Runtime.h`, that `build` and the c
backend link programs against, the plugin interface `include/paykan/plugin_api.h` (with the
generated `include/paykan/plugin_api_version.h` it includes), and the
empty system plugin directory `lib/paykan/plugins/<version>/`):

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
paykan --emit-ast program.pkn   # print it in the AST interchange format (docs/plugins/ast-format.md)
paykan --emit-pir program.pkn   # print the backend-neutral IR (docs/pir.md)
paykan --emit-source program.pkn # print the backend's output (C or LLVM IR)
paykan --emit-c program.pkn     # print the generated C (--backend=c --emit-source)
paykan --emit-llvm program.pkn  # print the LLVM IR (--backend=llvm --emit-source)
paykan --track-heap program.pkn # run, then print heap/leak statistics
paykan -O0 program.pkn          # set the optimization level (0-3, default -O2)
paykan --plugin=FILE ...        # load a plugin library (repeatable; see Plugins above)
paykan --version                # print the version, every plugin and the plugin directories
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

The tests are off by default. They come in suites; build the ones you need with
`-DPAYKAN_BUILD_TESTS=<suites>` (a CMake list), or all of them with `-DPAYKAN_BUILD_ALL_TESTS=ON`:

```sh
cmake -B build "-DPAYKAN_BUILD_TESTS=parser;sema"   # just these two suites
cmake -B build -DPAYKAN_BUILD_ALL_TESTS=ON          # every suite
cmake --build build --parallel "$(getconf _NPROCESSORS_ONLN)"
ctest --test-dir build --output-on-failure
```

Each suite is a `paykan_test_suite()` section of [`tests/CMakeLists.txt`](tests/CMakeLists.txt);
`scripts/affected_tests.py --list` prints them with the tests each runs.
[`tests/suites.json`](tests/suites.json) is generated from the build files by
`scripts/gen_test_suites.py` (the pre-commit hook runs it; CI checks the file is current). The GoogleTest-based ones need GoogleTest: an installed one if CMake finds it, otherwise one
downloaded at configure time.

`scripts/affected_tests.py --base <commit>` prints the suites a change can affect; CI builds
only those on a pull request, and every suite on a push.

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

- **Documentation: [parsabee.github.io/PaykanLang](https://parsabee.github.io/PaykanLang/)** — the manual, a tutorial that builds a real program chapter by chapter ([`docs/manual/`](docs/manual/index.md)), with the reference below.
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
