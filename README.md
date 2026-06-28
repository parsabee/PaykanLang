# PaykanLang

[![CI](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml/badge.svg)](https://github.com/parsabee/PaykanLang/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.0.0-blue.svg)](CHANGELOG.md)

PaykanLang (`.pkn`) aims to be a clean, small, statically-typed object-oriented language
that compiles to LLVM and runs at native speed without a garbage collector. Memory is
managed by automatic reference counting, so objects are freed deterministically as soon
as the last reference drops — no GC pauses, no manual `free`. The goal is a language that
feels simple and predictable to write while giving you JIT execution for fast iteration
and (eventually) ahead-of-time native binaries for deployment.

This is **v0.0**, an early but honest preview. The language core is implemented and tested:
static typing and inference, single-inheritance classes with virtual dispatch, ARC, dynamic
arrays, a file-based module system, `match` type dispatch, and enums. Programs run today via
the LLVM JIT. The two main things still missing relative to that goal are ahead-of-time native
compilation (planned for v0.1) and a few rough edges in the object protocol — see
[Known Limitations](#known-limitations) and [CHANGELOG.md](CHANGELOG.md).

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

This README intentionally stays short. To learn PaykanLang:

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

## Known Limitations

v0.0 is an honest preview. The following are known gaps; fixes are planned for v0.1
(see [CHANGELOG.md](CHANGELOG.md) for the full list):

- **`Obj.equals()` on user-class instances is unreliable** — it may return `False`
  even for the same object (`b.equals(b)` is `False` while `b == b` is `True`).
  Use `==` for object identity.
- **`toString()` formatting is incomplete** — arrays print `Array@<addr>[len=N]`
  rather than `[1, 2, 3]`, and the default object `toString()` prints
  `Object@<addr>` rather than a class-name-based format.
- **JIT only** — ahead-of-time native compilation is planned for v0.1.

---

## License

PaykanLang is released under the [MIT License](LICENSE).
