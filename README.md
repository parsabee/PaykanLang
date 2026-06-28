# PaykanLang

[![CI](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml/badge.svg)](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.0.0-blue.svg)](CHANGELOG.md)

PaykanLang (`.pkn`) is a high-performance language for building applications on modern,
heterogeneous machines. The goal is to make ordinary application code run fast — across
many cores and, increasingly, across the different compute units in a system (CPUs, GPUs,
and other accelerators), with work offloaded to them seamlessly rather than wired up by
hand. It aims to be **memory-safe and thread-safe** by design, with a clean, small surface:
a real module system, C interoperability exposed through modules, and automatic reference
counting in place of a garbage collector or manual memory management. PaykanLang deliberately
trades the full low-level control of a systems language for safety and ergonomics — it sits
at the productive, high-level end of the spectrum while still compiling to native speed.

This is **v0.0**, an early but honest preview that lays the foundation. Implemented and
tested today: static typing with inference, single-inheritance classes with virtual dispatch,
ARC, dynamic arrays, a file-based module system, `match` type dispatch, and enums — all
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

The `paykan` binary is placed at `build/bin/paykan`. To install it to a prefix (the binary
statically links the runtime, so it is self-contained for JIT execution):

```sh
cmake --install build --prefix /usr/local
```

---

## Running

```sh
paykan program.pkn              # JIT-execute a source file
paykan --emit-llvm program.pkn  # print the generated LLVM IR
paykan --version                # print the compiler and LLVM versions
```

`main`'s return value becomes the process exit code.

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
  functions, classes, arrays, and modules.
- **[`samples/`](samples/)** — runnable `.pkn` programs exercising every feature
  (`samples/codegen/` has 25 feature demos; see also `imports/`, `sema/`, `leak-check/`).
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
