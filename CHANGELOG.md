# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0-alpha] - 2026-10-03

The first tagged release, published as a GitHub pre-release. PaykanLang is now
split into a core, pluggable frontends and pluggable backends around a
backend-neutral IR (PIR); the default build needs only CMake and a C++20
compiler, plus a C11 compiler at run time. Fixes found while testing the alpha
go under `[0.1.0]`. Features marked *prototype* (tuples, optional types,
generics) are experimental and may change.

### Breaking changes

- **The default build is the core: the recursive-descent frontend and the C
  backend (#123).** The default backend changes from llvm to c. A plain
  `cmake -B build` builds only those two and downloads nothing. The LLVM
  backend, with its JIT, and the Bison frontend are opt-in at configure time:
  `-DPAYKAN_BACKENDS="llvm;c"`, `-DPAYKAN_FRONTENDS="recursive-descent;bison"`.
  The default backend is the first one listed (`llvm` with `llvm;c`);
  `--list-backends` shows it. Without the LLVM backend, `paykan` runs programs
  with the C backend, which needs a C11 compiler (`cc`, or `$CC`) at run time.
  The release tarballs and the Homebrew formula ship the core only.
- **The Bison frontend is an optional plugin (#17, #60).** The hand-written
  recursive-descent frontend (standard C++ only) is the default. The
  Bison/Flex frontend implements the same grammar (`docs/grammar.md`) and
  builds the same AST; it is selected with `--frontend=bison` and is moving
  to its own repository.
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
- **Conversion constructors are a closed set of specializations, and the
  type argument is optional (#88).** Each builtin target (`Str`, `int`,
  `Int`, `float`, `Float`, `bool`, `Bool`, `char`) has a fixed set of valid
  sources, and no other type, a user class included, can be one.
  - **Inferred form.** `Target(value)` picks the specialization whose source
    is exactly the argument's type: `Str(n)` with an `int` is `Str<int>(n)`,
    `int(f)` is `int<float>(f)`, `int(s)` is `int<Str>(s)` (an `int?`). No
    widening or unwrapping is applied. The explicit `Target<Source>(value)`
    still works, with the argument required to have exactly that type.
    `Str(s)` with a `Str` is still the `Str` constructor.
  - **New boxed forms.** `Str<Int>`, `Str<Float>`, `Str<Bool>` and
    `Str<Char>` format a box exactly like the primitive form. `Int<Str>`,
    `Float<Str>` and `Bool<Str>` parse like `int<Str>` & co. and return the
    optional box (`Int?`, `Float?`, `Bool?`). `Int`, `Float` and `Bool` have
    no other constructor, so `Int(5)` is an error.
  - **`bool<Str>` (new)** returns `bool?`: `True` / `False` for exactly
    `"True"` / `"False"`, the spellings `Str<bool>` prints, and `None` for
    anything else. The runtime gains `PaykanBool_from_str`.
  - **New diagnostic.** Both forms now report a missing pair as `no
    specialization of 'Str' for 'A'; its specializations are int, float,
    bool, char, Int, Float, Bool, Char`, replacing `no conversion from … to
    …`. This covers user classes, `Obj` (a bare `None`), optionals and the
    other builtins. It also replaces `a conversion to 'int' names its source
    type`, since `int(2.5)` is now valid.
  - The samples and the language reference use both forms.
- **Modules are named canonically, and `-O2` is the default (#102).** Every
  module is identified by its canonical module name (`geometry::shapes`,
  `::io` for a system module; the main module by its file stem), never by its
  file path, so no absolute path reaches the PIR, the generated C, symbol
  names or cache keys, and builds of one project in two directories are
  identical. Cache entries are named after the module
  (`.paykan_cache/geometry/shapes.bc`, `shapes.<hash>.{c,o,key}`, system modules under
  `@system/`), and the generated-code ABI version is 6, so older caches are
  rebuilt. `paykan run` and `paykan build` optimise at `-O2` by default (was
  `-O0`); pass `-O0` for unoptimised code.
- **Builtin names are reserved.** A top-level function, class or enum named
  like a builtin function (`print`, `println`, `printerr`, `printerrln`,
  `Str`, `open`) or a builtin class (`Obj`, `Str`, `Array`, `File`, `Error`,
  `Int`, `Float`, `Bool`, `Char`) is a compile-time error at the
  declaration (`'print' is a builtin function and cannot be redeclared`). A
  class silently replaced the builtin of the same name before. One namespace
  holds every top-level name, so a function, class or enum cannot reuse
  another's name either. Fields, methods, parameters and locals are not
  affected.
- **Method symbols are `<Class>.<method>` (#86).** Methods and destructors
  were lowered to `<Class>_<method>`, so a user `fn K_w()` next to
  `class K { fn w() … }` (or `fn K_destroy()`) clashed with the method and
  failed with an internal error. The `.` cannot occur in a Paykan
  identifier, so the names can no longer clash; the LLVM backend's vtables
  are `<class>..vtable`. Code built against the old names, and the
  bitcode cache, must be rebuilt: the generated-code ABI version was bumped
  (to 5, and to 6 by #102).
- **`==` / `!=` on reference types dispatch to `equals`, so `Str ==`
  compares contents.** For classes, `Str`, and arrays, `a == b` now calls
  the virtual `equals` method (and `!=` its negation). `Str` comparison is
  therefore **content** equality (`"hel" + "lo" == "hello"` is `True`; it
  compared identities before); arrays keep identity comparison via
  the default `equals`; comparing arrays of different element types is a
  compile error. Overriding `equals` in a class changes how `==` behaves for it.

### Added

- **Pluggable frontends (#16, #17).** The frontend (lexer and parser) is a
  plugin behind an interface (`include/paykan/Frontend.h`) and a registry:
  `-DPAYKAN_FRONTENDS=<list>` picks the frontends to build,
  `--frontend=<name>` selects one at run time and `--list-frontends` lists
  them. The grammar both frontends implement is specified in
  `docs/grammar.md`; `--dump-tokens` prints a frontend's token stream, and
  the `FrontendDifferential` ctest (`scripts/diff_frontends.py`) checks that
  every sample is accepted or rejected alike, with the same AST.
- **Pluggable backends and PIR (#16, #35, #48).** Programs are lowered from
  the AST to PIR, the backend-neutral Paykan IR (`docs/pir.md`), in which
  every type, retain/release, vtable and scope cleanup is explicit; every
  backend consumes PIR. `--emit-pir` prints it, and its text form parses
  back (the `PIRRoundTrip` tests round-trip the whole samples corpus).
  `-DPAYKAN_BACKENDS=<list>` picks the backends to build, `--backend=<name>`
  selects one and `--list-backends` lists them; `--emit-source` prints a
  backend's output.
- **The C backend (#16, #36).** `--backend=c` translates PIR to strict ISO
  C11 (`docs/c-backend.md`) and compiles it with the system C compiler. It
  is part of the core: standard C++ only, no third-party library. `run`
  builds into a temporary directory and runs the program; `--emit-c` prints
  the C. Imported modules are compiled once and cached
  (`.paykan_cache/`, keyed by the generated C, the compiler and its flags).
- **The LLVM backend translates PIR (#38).** The old AST code generator is
  gone: the llvm backend lowers PIR to LLVM IR and runs it with the ORC JIT
  (`--emit-llvm` prints it).
- **`paykan build` (#36, #38).** `paykan build prog.pkn -o prog` compiles a
  native executable with either backend (the llvm backend emits an object
  file and links it with the system C compiler). `main`'s return value is
  the exit code, and `main(args: Str[])` receives the command line.
- **Out-of-tree plugins (#37).** `cmake --install` installs the interfaces
  and `find_package(Paykan)`, so a backend or frontend can be built outside
  the tree and linked into a custom `paykan` (`docs/writing-a-backend.md`).
  `utils/print-pir` is the smallest complete backend; CI and the
  `PrintPIROutOfTree` ctest build it against an installation.
- **Documented: selective imports, `Stdin`, `File.readbytes` / `File.read`
  and system imports.** These worked before but were not listed:
  `import lib::{foo, bar as b};` imports several modules of one directory;
  `Stdin` is a builtin `File` over standard input; `readbytes(n)` and
  `read()` read `n` bytes or the rest of a file (`None` at the end);
  `import ::io;` looks a module up in `$PAYKAN_STDLIB` (no standard library
  ships yet, #113). See `docs/language/01-language-basics.md` and
  `06-modules.md`.
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
  `docs/language/11-generics.md`.
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
  `docs/language/10-optionals.md`.
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
  `docs/language/10-optionals.md`.
- **Tuples (prototype, #4).** Fixed-arity, heterogeneous, immutable values:
  types `(int, Str)` (nesting, `(int, Str)[]` and `(int[], Str)` allowed),
  literals `(1, "a")`, compile-time-checked element access `t.0` / `t.1.0`,
  multiple return `fn f() -> (int, int)`, and destructuring
  `q, r = divmod(7, 2);` (`_` skips, `q: int, r: int = ...` annotates).
  Tuples are `Obj` subtypes under ARC (retain/release/`mov` like any
  reference value); `==` / `!=` compare element-wise and `toString` renders
  `(1, a)`. Tuple-typed signatures round-trip through module imports.
  Not yet: tuples in `match`, nested destructuring, mutation, named elements.
  See `docs/language/09-tuples.md`.

- **Move semantics with `mov`.** `mov <expr>` transfers ownership of a local
  variable, parameter, or temporary without a retain/release pair. Use of a
  moved variable is a compile error until it is re-assigned, tracked
  flow-sensitively: a `mov` in one `if`/`match` branch does not poison sibling
  branches, paths are merged conservatively after the construct, and moving a
  variable declared outside a loop without re-assigning it before the loop
  repeats is rejected. `mov self` and moving fields or array elements are
  compile errors. See `docs/language/08-memory-model.md`.
- **`-DPAYKAN_BUILD_TESTS=OFF` builds without the test suite (#77, #78).** It
  skips GoogleTest, so a barebones configure downloads nothing (the default is
  ON when PaykanLang is the top-level project). With tests on, an installed
  GoogleTest (`find_package(GTest CONFIG)`) is used before downloading one.
  The Homebrew formula and the release workflow build with tests off. CI and
  the release workflow can be run by hand (`workflow_dispatch`); a manual
  release run is a dry run by default and publishes nothing. JIT errors in the
  CodeGen test harness are now reported in the test's stderr.

### Fixed

- Two modules (or the main file and an import) that each define a free
  function with the same name no longer make the lowering fail with an
  internal error (#70): `x::tag`, `y::tag` and a local `tag` each call
  their own module's function. Two imports that bind one qualifier to
  different modules (`import a::util; import b::util;`) are now an error.
- A tuple or array literal flowing into a typed slot takes the slot's
  element types (#71): `(None, "a")` is an `(int?, Str)` and
  `[("a", 1), ("b", nb)]` a `(Str, int?)[]` where the destination says so.
- A string literal as a `match` subject (`match "s" { "t" { … } _ { … } }`)
  failed PIR verification (#72); it is now a `Str` like any other subject.
- Float range (#73): subnormal values are accepted everywhere (float
  literals, `float<Str>`, PIR constants), and a float literal that
  overflows to infinity, or a nonzero one that underflows to zero, is a
  compile-time error (`float is out of range: <text>`).
- The C backend rebuilds a cached object that was truncated or corrupted
  after it was cached (#74), instead of failing every later link until
  `.paykan_cache` was deleted.
- A user function named `<Class>_<method>` no longer clashes with that
  class's method (#86); see *Breaking changes*.
- `float` `!=` is IEEE 754's unordered not-equal (#58): `nan != nan` is
  `True` on both backends.
- The GCC 13 Release build compiles under `-Werror` (#76).
- A string literal used as an object (`"ab"[1]`, `"abc".len()`, `mov "lit"`
  in any position, a literal in a tuple, array or optional) is now a `Str`
  object; it was passed as its raw C string, an internal compiler error
  (#116). User functions spelled like runtime symbols (`PaykanString_new`,
  `Paykan_println`, `Paykan_panic_div_by_zero`, ...) no longer replace or
  clash with the runtime's, which was an internal compiler error or a silent
  miscompile (#117): runtime externs are now named `$rt.<symbol>` in PIR
  (`docs/pir.md` §3), a name no Paykan identifier can spell.
- **Diagnostics and driver (#119, #120, #126).** A failed import (missing,
  circular, a directory, or a module with errors) is reported once, and uses
  of its names are not reported again; import messages name modules
  canonically (`lib::m`) and files from the source root. `''` is one
  `empty character literal` error. `x: Str? = mov None` is accepted. `-O4`
  and above are rejected; a directory as the source file is a clean error.
  The Bison frontend enforces the 512-level nesting limit. `Stdin` and type
  names are rejected by every binder, including typed declarations and
  parameters.
- **Concurrent builds with different flags no longer link each other's
  objects (#134).** The C backend's cache checked a module's `.key` and
  linked its `.o` later; a concurrent `paykan` with other flags (a sanitizer,
  another `-O`) could replace the object in between, and the link failed
  (`undefined reference to __asan_init`). Entries are now named after a hash
  of their key (`.paykan_cache/geometry/shapes.<hash>.{c,o,key}`), so
  different configurations never share a file and alternating `-O0` / `-O2`
  builds no longer rebuild each other. Each module keeps its four most
  recently used entries (and any used within the hour); older ones are
  removed. The llvm backend's `.bc` cache was not affected.
- **Diagnostics follow-ups (#132).** A module that fails to load is reported
  once per compilation however many import paths reach it (twice from one
  file, or through a diamond); each later import adds a note. `mov None`
  inside an array or tuple literal takes the slot's element type
  (`xs: Str?[] = [mov None]`). Past the nesting limit, the recursive-descent
  frontend skips the nested block whole and reports nothing more. A missing
  or ill-typed `main`, or an empty file, is one Sema error with a source
  location (`program has no entry point 'fn main() -> int'`), on both
  frontends, instead of a PIR verifier failure; `--check-only` still accepts
  a module without `main`.
- An empty array literal `[]` stored into a class field, passed as a
  call/method/`push` argument, returned, stored through a subscript, or nested
  in another literal was compiled as a primitive-element array regardless of
  the slot's element type, so the objects later pushed into it were never
  released (a leak in every `self.items = []` pattern). Sema now adopts the
  destination's array type at every typed sink, not only declarations.
- A tuple holding a NaN compared equal to itself (#111): `t == t` was `True`
  and `t != t` `False` when both sides were the same tuple object, because
  the runtime's tuple `equals` returned "equal" for one object before looking
  at the elements. Tuple equality is now element-wise in every case, so a NaN
  element (directly, in a nested tuple, or in a `float?` element) makes a
  tuple unequal to itself, as IEEE 754 makes the NaN unequal to itself.
  `float?` already compared its values; arrays keep their reference-identity
  `==`.
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
- One rejected class declaration (a builtin or imported name, an undefined
  superclass, a bad member type) no longer makes every other class of the
  module undeclared (#79): the other classes are still declared, the bad
  class is registered as far as it can be, and the follow-on errors on its
  uses are not reported.
- The recursive-descent frontend reports each syntax error once (#79): no
  follow-on error right after a lexical error (an unterminated string, an
  invalid character), one error (with a note at the open `{`) for blocks left
  unclosed at end of file instead of one per block, and one error for a
  multi-character literal such as `'ab'`.
- Output printed before a runtime panic is no longer lost when stdout is not
  a terminal (#92). A panic now flushes stdout (and every open `File`) before
  printing its message and aborting, on both backends, in `paykan run` and in executables from `build`;
  with `2>&1` the output comes before the panic message.
- A runtime panic ends `paykan run` the same way on both backends (#79): with
  exit status 134 (128 + `SIGABRT`). The llvm backend's JIT used to let the
  abort kill the compiler process itself; executables from `build` still die
  by `SIGABRT`.
- A qualified match arm on an enum (`base::Color::Green { … }`) now says to
  use the bare variant name (`'Green'`) instead of claiming it is not a
  variant (#79).
- A failed declaration reports only its own error (#89). A variable whose
  first assignment, initializer, type annotation, destructuring or match-arm
  pattern is in error is still declared, with an internal error type, so its
  later uses are no longer reported as `use of undeclared variable`, and
  nothing built on it (a call, an operator, a member access, a derived
  variable, ...) reports a follow-on either. A later valid assignment
  re-declares it with the value's type. Calls to a function whose signature
  failed are likewise not reported as calls to an undeclared function.
- Array `push` / `pop` no longer thrash at the capacity boundary (#95). An
  array now shrinks only once its length falls to a quarter of its capacity,
  and then to half the capacity, never to exactly its length; capacity never
  drops below 8 slots on its own. A push/pop loop at a length of 524,288 went
  from about 40 s to 15 ms. `--track-heap` also reports the number of
  reallocations.
- A use of another module's generic class or function (`shapes::Box<int>(7)`,
  `x: shapes::Box<int>`, `shapes::first<int>(xs)`, `g::Box<int>(7)`) inside
  a generic body is rejected once, not once per instantiation of that body
  (#112). Debug builds of the compiler now abort with an internal error when
  the identical diagnostic (same location, message and notes) is reported
  twice.
- The runtime lookup of `paykan run` / `build` no longer prefers a stale
  build tree over an installed binary's own prefix (#124). The order is now
  `$PAYKAN_RUNTIME_DIR`, the install prefix around the executable, the build
  tree (only for binaries inside it, marked by `.paykan-build-tree`), then the
  configured install prefix. `PaykanConfig.cmake` records the installed
  runtime (`PAYKAN_RUNTIME_LIBRARY` / `PAYKAN_RUNTIME_INCLUDE_DIR`), and
  drivers made with `paykan_add_driver` use it, so they work wherever they
  are built. The samples that write files put them in a directory the
  harness passes per run (`// args: {tmpdir}`), so parallel runs no longer
  race on fixed `/tmp` paths.
- Dropping a deep chain of objects (a long `Node?` list, a deeply nested tree,
  arrays and tuples nested inside each other) no longer overflows the C stack
  (#118). The runtime destroys objects iteratively, in constant stack space
  (`docs/language/08-memory-model.md`).

### Changed

- **Repository layout.** The language reference moved from its top-level
  directory into `docs/language/` (same file names). The example
  out-of-tree backend moved from `examples/backends/print-pir` to
  `utils/print-pir`. `example_program/` is gone: its `calc` program is now
  the multi-module sample `samples/imports/12_calc`, with its expected
  output, so every harness runs it. `PLAN-0.1.md` (superseded by #27) and
  `proposals/` were removed; the normative parts of the proposals are in
  the reference (`docs/language/09-tuples.md`, `10-optionals.md`,
  `11-generics.md`). New ctests: `MarkdownLinks` (`scripts/check_links.py`)
  and `DocExamples` (`scripts/doc_examples.py`, which runs every doc example
  that states its output).
- Runtime object header grew by 8 bytes: every heap object now carries a
  backpointer to its reference-count box (the unique-box invariant above).
- Cached import bitcode is stamped with an ABI version; caches written by a
  compiler with a different (or missing) ABI stamp are recompiled instead of
  loaded.
- Parse errors changed format from yacc-style one-liners to clang-style
  caret-and-snippet diagnostics.
- The C backend's output is strict ISO C11 (#62): it compiles with
  `-std=c11 -pedantic-errors -Wall -Wextra -Werror` on GCC and Clang (the
  new `CStrictC11` ctest and *Strict C11* CI job, over the whole samples
  corpus), so the C compiler no longer runs with `-w`. Every vtable, the
  runtime's included, is now an array of `PaykanMethod` slots (`Runtime.h`,
  `PAYKAN_SLOT_*`) instead of a struct of function pointers, and `Runtime.h`
  checks the target assumptions (LP64, 8-bit bytes, two's complement,
  IEEE 754 doubles) with `_Static_assert` (`docs/c-backend.md`).

## Initial prototype - 2026-06-28

The first prototype; it was never tagged or released. PaykanLang was a
statically-typed, object-oriented language that compiled to LLVM IR and was
**JIT-executed**: deliberately small, with several features planned for v0.1.

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

[Unreleased]: https://github.com/parsabee/PaykanLang/compare/v0.1.0-alpha...HEAD
[0.1.0-alpha]: https://github.com/parsabee/PaykanLang/releases/tag/v0.1.0-alpha
