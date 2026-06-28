# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.0.0] - Unreleased

First tagged preview release. PaykanLang is a statically-typed, object-oriented
language that compiles to LLVM IR and is **JIT-executed**. This release is a
deliberately small, honest preview; several features are planned for v0.1.

### Added

- Statically-typed language front end: lexer (Flex), parser (Bison/LALR(1)),
  and a full semantic-analysis pass with a central diagnostic engine.
- Primitive types `int`, `float`, `bool`, `char`, and the built-in `Str` class.
- Single-inheritance classes with vtable-based virtual dispatch, automatic
  construction (`__init__`) and destruction.
- Object equality with consistent identity semantics: `==` and the default
  `Obj.equals()` agree, and classes may override `equals()`.
- Automatic reference counting (ARC) for deterministic object lifetimes; no GC.
- Enum types and `match` expressions with class-hierarchy, array, and
  int/str literal value patterns.
- Heap-allocated, element-typed arrays (`int[]`, `Str[]`, `Point[]`, …) with
  subscript read/write, `.len()`, `.push()`, `.pop()`, and 2D arrays.
- File-based module system with selective and aliased imports, with bitcode
  caching of imported modules.
- Built-in I/O: `open()` / `File` / `Error`, and `println` / `printerrln`.
- LLVM 17 backend with JIT execution via the ORC LLJIT.
- `paykan <file.pkn>` runs a program; `--emit-llvm` dumps IR;
  `--version` / `-v` reports the compiler and LLVM versions.
- `cmake --install` rules (GNUInstallDirs): the `paykan` binary, the runtime
  archive, and `Runtime.h` are staged for packaging.
- Integer divide and modulo by zero trap via a `noreturn` runtime panic.
- Direct calls to `destroy()` are rejected at compile time.
- Quality gates: `.clang-format` (LLVM style), `.clang-tidy` (bug-focused),
  sanitizer builds (ASan/LSan, UBSan), and CI on every push and pull request.

[0.0.0]: https://github.com/parsabee/PaykanLang/releases/tag/v0.0.0
