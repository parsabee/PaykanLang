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

This is an **early preview** (`0.1.0-alpha`) that lays the foundation. Implemented and tested
today: static typing with inference, single-inheritance classes with virtual dispatch,
ARC with `mov` move semantics, value-aware `==` (reference types dispatch to a virtual
`equals`), dynamic arrays, a file-based module system, `match` type dispatch, and enums — all
compiled to LLVM IR and JIT-executed. The defining goals — seamless offloading to
heterogeneous compute units, thread-safe concurrency, C interop through modules, and
ahead-of-time native binaries — are the road ahead, not yet shipped. See
[CHANGELOG.md](CHANGELOG.md) for what's in this release.

---

## Installing

### Homebrew (macOS)

```sh
brew tap parsabee/paykanlang https://github.com/parsabee/PaykanLang
brew install parsabee/paykanlang/paykanlang
paykan --version
```

---

## Building from Source

PaykanLang uses CMake and a pre-built LLVM (vendored under `third-party/llvm`), Bison, and Flex.

```sh
git clone https://github.com/parsabee/PaykanLang.git
cd PaykanLang
cmake -B build
cmake --build build --parallel
```

The frontend (lexer + parser) is pluggable. `-DPAYKAN_FRONTENDS=<list>` selects the
frontends to build (default: `recursive-descent;bison`); the first one listed is the default, and
the others are selected at run time with `--frontend=<name>` (`--list-frontends` prints
them). The recursive-descent frontend (standard C++ only) is always built; Bison and Flex are
downloaded and built only when `bison` is listed. Both implement `docs/grammar.md` and must
produce identical ASTs: `scripts/diff_frontends.py` checks that over every sample, and
`--dump-tokens` prints a frontend's token stream.

The backend is pluggable too. `-DPAYKAN_BACKENDS=<list>` selects the backends to build:
`c` (the C backend: emits C11, builds with the system C compiler, standard C++ only) is always
built; `llvm` (LLVM IR / ORC JIT) is optional and `mlir` is a reserved slot. The default backend
is `llvm` when it is listed, `c` otherwise; `--backend=<name>` selects another (`--list-backends`
prints them). LLVM is downloaded only when `llvm` or `mlir` is listed, so
`-DPAYKAN_FRONTENDS=recursive-descent -DPAYKAN_BACKENDS=c` is a **barebones build** that needs
nothing but a C++20 compiler and a C compiler (CI checks that it downloads nothing and links no
third-party library). Every backend consumes the Paykan IR described in [`docs/pir.md`](docs/pir.md);
[`docs/writing-a-backend.md`](docs/writing-a-backend.md) explains how to write one, in tree or
out of tree against `find_package(Paykan)` (see [`examples/backends/print-pir`](examples/backends/print-pir)).

The `paykan` binary is placed at `build/bin/paykan`. To install it to a prefix (the binary
statically links the runtime, so it is self-contained for JIT execution):

```sh
cmake --install build --prefix /usr/local
```

---

## Running

```sh
paykan program.pkn              # JIT-execute a source file
paykan --check-only program.pkn # stop after type-checking (no codegen/JIT)
paykan --emit-llvm program.pkn  # print the generated LLVM IR
paykan --dump-ast program.pkn   # print the parsed AST
paykan --track-heap program.pkn # run, then print heap/leak statistics
paykan -O2 program.pkn          # set the optimization level (0-3)
paykan --version                # print the compiler and LLVM versions
```

`main`'s return value becomes the process exit code, and extra command-line arguments are
passed to `main(args: Str[])` (with `args[0]` the source-file path). Source files use the
`.pkn` extension (`.pk` is also accepted).

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

---

## Development Tooling

Quality gates are enforced in CI and available locally via the Python helpers in `scripts/`
(they use the vendored LLVM 17 tools so results match CI):

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

- **[`language_reference/`](language_reference/)** — the full reference: language basics,
  functions, enums, classes, arrays, modules, `match` statements, the memory model,
  tuples (prototype), and optional types (prototype).
- **[`samples/`](samples/)** — runnable `.pkn` programs exercising every feature
  (`samples/codegen/` has 28 feature demos; see also `imports/`, `sema/`, `leak-check/`).
- **[`example_program/`](example_program/)** — a small end-to-end example (`calc`).

A taste:

```pkn
fn main() -> int {
  println("Hello, world!");
  return 0;
}
```

---

## License

PaykanLang is released under the [MIT License](LICENSE).
