# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Breaking changes

- **Conversion constructors replace the conversion builtins (#64).**
  `StrInt`, `StrFloat`, `StrBool`, `StrChar`, `IntStr` and `FloatStr` are
  removed. Every conversion is now spelled `Target<Source>(value)`, with the
  source type always written and the argument required to have exactly that
  type: `Str<int>(n)`, `Str<float>(f)`, `Str<bool>(b)`, `Str<char>(c)`,
  `int<Str>(s)`, `float<Str>(s)`. The old names are no longer reserved: a
  call to one is an ordinary undeclared-function error. The `Str<…>` formatting is unchanged,
  including the canonical `nan` / `inf` / `-inf`.
  - **Parses return optionals.** `int<Str>(s)` returns `int?` and
    `float<Str>(s)` returns `float?`, with `None` for an invalid string. They
    used to return an `Obj` holding an `Int` / `Float` box or an `Error`.
    Code that matched on `n: Int { … } err: Error { … }` now matches on
    `n: int { … } None { … }`.
  - **Stricter parsing.** Leading whitespace and embedded NUL bytes now make
    a parse fail (`strtoll` / `strtod` used to skip leading whitespace).
- **New numeric conversions (#64).** These are defined in the lowering, so
  both backends behave identically, and none of them is undefined behaviour
  in the generated C:
  - `int<float>` truncates toward zero, and panics on NaN, ±inf or a value
    outside the int64 range.
  - `float<int>` gives the nearest double.
  - `int<bool>` gives 1 / 0, and `bool<int>` is `!= 0`.
  - `int<char>` gives the char's byte code 0..255, and `char<int>` is its
    inverse, panicking outside 0..255.
  - Any other pair is an error that lists the valid sources. PIR gains a
    numeric `ftoi` instruction, which the lowering emits only after the
    range guard.

### Added

- **Generics (prototype).** Generic classes (`class Box<T> { … }`,
  `class Pair<K, V> { … }`) and generic free functions
  (`fn first<T>(xs: T[]) -> T`), implemented by monomorphisation in Sema: each
  distinct type-argument tuple instantiates the declaration once into an
  ordinary class/function named `Box<int>`, `Pair<Str, int>`, `first<int>`.
  Type arguments are accepted in every type position (`Box<int>[]`,
  `Box<Box<int>>`), on constructor calls (`Box<int>(3)`) and on calls
  (`first<int>(xs)`); a generic function's type arguments are inferred from
  the argument types (also through `T[]` and `Box<T>`), as are a generic
  class's from its `__init__` arguments (`Box(3)`). Bodies are checked per
  instantiation; an error inside one names it (`in instantiation of
  'Box<bool>' requested here`). Templates are not exported across modules
  (`mod::Box<int>` is rejected with "generic types cannot be imported yet");
  a module's own instantiations are exported as concrete classes. No
  constraints, variance, defaults or specialisation yet — see
  `proposals/generics.md` and `language_reference/11-generics.md`.
- **Optional types `T?` (prototype, issue #5).** Any reference type (a class,
  `Str`, or an array) has an optional form `T?` holding either a `T` or
  `None`. `T` widens to `T?` implicitly; a `T?` never narrows back without a
  `match`, whose `T` arm binds every present value and whose `None` arm (or
  `_`) covers the absent case. `x == None` / `x != None` is a null check,
  two optionals compare with the usual `equals` once both are present, an
  optional field is implicitly `None` if `__init__` does not assign it, and
  `T?[]` / `T[]?` are supported. At runtime a `T?` is the same reference-
  counted box as a `T` with "no box" meaning `None` — no layout change.
  Nested optionals, flow typing, `if let`,
  `??` and `?.` are not part of the prototype. See
  `language_reference/10-optionals.md` and `proposals/optionals.md`.
- **Optional primitives `int?`, `float?`, `bool?`, `char?` (#66).** A
  primitive widens to its optional implicitly (`x: int? = 5`; an `int` also
  widens to `float?`), and `None` is the absent value. In `match o { n: int
  { … } None { … } }` the arm binds a plain `int`. `== None`, equality of two
  optionals, fields (implicitly `None`), arrays, tuples, parameters, returns,
  ternaries and generic arguments (`Box<int?>`, and `T?` with `T = int`) all
  work. A present value is boxed in the runtime's `Int` / `Float` / `Bool` /
  `Char` object (`Char` is a new boxed class), so it prints and matches as
  that class in an `Obj` slot. `Enum?` is still rejected, now with "optional
  enum types are not supported yet". `int?` does not convert to `float?`. See
  `language_reference/10-optionals.md`.
- **Tuples (prototype, #4).** Fixed-arity, heterogeneous, immutable values:
  types `(int, Str)` (nesting, `(int, Str)[]` and `(int[], Str)` allowed),
  literals `(1, "a")`, compile-time-checked element access `t.0` / `t.1.0`,
  multiple return `fn f() -> (int, int)`, and destructuring
  `q, r = divmod(7, 2);` (`_` skips, `q: int, r: int = ...` annotates).
  Tuples are `Obj` subtypes under ARC (retain/release/`mov` like any
  reference value); `==` / `!=` compare element-wise and `toString` renders
  `(1, a)`. Tuple-typed signatures round-trip through module imports.
  Not yet: tuples in `match`, nested destructuring, mutation, named elements.
  See `language_reference/09-tuples.md` and `proposals/tuples.md`.

- **Move semantics with `mov`.** `mov <expr>` transfers ownership of a local
  variable, parameter, or temporary without a retain/release pair. Use of a
  moved variable is a compile error until it is re-assigned, tracked
  flow-sensitively: a `mov` in one `if`/`match` branch does not poison sibling
  branches, paths are merged conservatively after the construct, and moving a
  variable declared outside a loop without re-assigning it before the loop
  repeats is rejected. `mov self` and moving fields or array elements are
  compile errors. See `language_reference/08-memory-model.md`.
- **`==` / `!=` on reference types dispatch to `equals`.** For classes, `Str`,
  and arrays, `a == b` now calls the virtual `equals` method (and `!=` its
  negation). `Str` comparison is therefore **content** equality
  (`"hel" + "lo" == "hello"` is `True`); arrays keep identity comparison via
  the default `equals`; comparing arrays of different element types is a
  compile error. Overriding `equals` in a class changes how `==` behaves for it.

### Fixed

- An empty array literal `[]` stored into a class field, passed as a
  call/method/`push` argument, returned, stored through a subscript, or nested
  in another literal was compiled as a primitive-element array regardless of
  the slot's element type, so the objects later pushed into it were never
  released (a leak in every `self.items = []` pattern). Sema now adopts the
  destination's array type at every typed sink, not only declarations.
- Passing an array or tuple to a user-defined method (`b.take(xs)`,
  `b.take([1, 2])`) freed the argument twice: the callee owns every
  reference-typed parameter, but the call site only handed over an owned box
  for class and optional parameters.
- Integers flowing into `float[]` element slots (`xs: float[] = [1, 2]`,
  `xs.push(3)`, `xs[0] = 4`, `[1, 2.5]`) were stored as integer bit patterns
  and read back as denormal floats; they are now promoted like scalar stores.
- The import cache key now includes the identity of the compiler binary, so a
  development build with changed codegen but an unchanged version string no
  longer serves stale `.paykan_cache` modules compiled by the previous build.
- Double-frees when an object was reached through more than one owning alias:
  using `self` as a value (return/argument/assignment/`mov`), ownership-taking
  uses of `match` bindings, and array-typed class fields. Fixed by a new
  unique-box runtime invariant — every heap object records its single
  reference-count box in its header, and creating an owning reference reuses
  that box instead of creating a second one.
- `push` / `pop` called on arbitrary receivers (call results, nested
  subscripts) no longer hit an internal assertion.
- `mov` soundness holes: moves in loops of loop-external variables (previously
  compiled, then crashed on the second iteration), false "moved" errors in
  sibling branches, and `mov self` (previously accepted) are now handled
  correctly at compile time.
- A `match` over both `bool` literals now counts as exhaustive for
  "always returns" and field-initialisation analysis.
- The imported-module bitcode cache no longer goes stale or lands in the
  wrong place: it now lives in `.paykan_cache/` under the project root (the
  main file's directory) instead of the current working directory, entries
  are keyed by a content hash of the module plus every module it transitively
  imports (and the compiler/ABI version) instead of file timestamps — so
  editing a dependency's class layout or signature recompiles its importers —
  and entries are written atomically; a corrupt or truncated entry is
  regenerated instead of being loaded.
- Declaring `self` as a method parameter is now a clear compile error instead
  of a confusing arity error at the call site.
- Parser diagnostics are now clang-style (caret + source snippet), matching
  semantic errors; a raw newline inside a string or character literal is
  rejected with a targeted message (use `\n`); the unused `&` token was
  removed from the lexer; out-of-range integer literals are reported reliably;
  an unopenable file no longer calls `exit()` from library code.

### Changed

- Runtime object header grew by 8 bytes: every heap object now carries a
  backpointer to its reference-count box (the unique-box invariant above).
- Cached import bitcode is stamped with an ABI version; caches written by a
  compiler with a different (or missing) ABI stamp are recompiled instead of
  loaded.
- Parse errors changed format from yacc-style one-liners to clang-style
  caret-and-snippet diagnostics.

## [0.0.0] - 2026-06-28

First preview release (not yet tagged in git). PaykanLang is a statically-typed, object-oriented
language that compiles to LLVM IR and is **JIT-executed**. This release is a
deliberately small, honest preview; several features are planned for v0.1.

### Added

- Static typing with full semantic analysis and clear, source-located diagnostics.
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
- The `destroy()` destructor is compiler-generated and final: user classes cannot
  override it, and direct calls are rejected at compile time.

[0.0.0]: https://github.com/parsabee/PaykanLang/releases/tag/v0.0.0
