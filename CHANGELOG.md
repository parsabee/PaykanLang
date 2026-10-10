# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Borrowed results (docs/language/02-functions-and-calling.md, "Borrowed results"): a
  function or method declared `-> view T` returns a borrow of part of `self` or of a
  `view` or `inout` parameter, never a new value, a copy parameter or a local (`'d' is a
  copy: a 'view' result must be part of 'self' or of a 'view' or 'inout' parameter`).  The
  caller sees the result as a `view`: it is read, passed on to `view` parameters or bound
  to a `view` local, and a value type can be copied out (`the result of 'peek' is a
  'view'; 'tick' is not a 'view fn'`).  A `view` local of a borrowed result borrows what
  the call borrowed, its receiver and its arguments to `view` / `inout` parameters, until
  its last use.  `-> inout T` returns the storage of part of `self` or of an `inout`
  parameter, of exactly its type: the caller binds it to an `inout` local, passes it on to
  an `inout` parameter or reads it, and it borrows the receiver and the `inout` arguments
  as an `inout` local would; it is lowered as the address the function returns.  An
  override keeps how the method it overrides returns; `main` returns a copy.  The result's mode is part of a module's interface
  (a result-mode byte after a `.pkm` record's parameter modes), and in the AST interchange
  format it is a `(qual view)` after the return type.
- `match` arms that borrow the subject (docs/language/07-match-statements.md, "Borrowing
  the subject"): `d: view Dog { … }` binds `d` as a `view` local of the subject, whatever
  the subject is, so nothing changes through it and it passes on only to `view`
  parameters; the variable the subject starts from cannot change until `d`'s last use in
  the arm.  `d: inout Dog { … }` binds `d` as the subject's storage, like an `inout` local:
  assigning to it writes the matched variable or field (a variable, a field, an optional,
  an `inout` parameter; not an expression, `self`, a `let` local, a `view` or a primitive
  inside an optional yet), and the subject's variable cannot be used until `d`'s last
  use.  With the mode before the name (`view d: Dog`) the syntax error shows the fix.  In
  the AST interchange format the arm carries `(qual view)` or `(qual inout)` after its
  type.
- `view fn` (docs/language/04-classes.md, "Methods that don't change self"): a method
  declared `view fn len() -> int` does not change `self`, and is the only kind of method
  that can be called on a `view` (`'c' is a 'view' parameter; 'tick' is not a 'view fn'`).
  `toString`, `equals`, `len` and `length` are `view fn`s.  An override is a `view fn`
  exactly when the method it overrides is one; `__init__` and a free function never are.
  The marker is part of a module's interface (method flag `0x2` in a `.pkm`), and a
  function in the AST interchange format carries it as a trailing `(qual view)`.  In a
  `view fn`, `self` and everything reached through it is read-only, as through a `view`
  parameter (`'self' is read-only in 'view fn get'; cannot assign to its field 'n'`),
  though part of `self` may still be returned.  A method that never changes `self` but
  is not a `view fn` gets a warning (`'area' never changes 'self': make it a 'view fn' so
  'view' parameters can call it`), worked out across a method's overrides and its calls
  on `self`; the samples and the manual mark theirs.  `samples/codegen/48_view_fn.pkn`
  shows them.
- Local borrows (docs/language/01-language-basics.md, "Local borrows"): a local declared
  with a mode where the type goes, `v: view = k + 1;` or `w: view float = k;`, the type
  inferred when left off.  A `view` local reads its initializer, which may be any
  expression: a copy of a value type, the same object, string or array otherwise.  It
  cannot be assigned, destructured into or passed to an `inout` parameter, nothing reached
  through it can be assigned, and only a `view fn` (`toString`, `equals`, `len`,
  `length`, or a method declared one) can be called on it.  An `inout` local (`x:
  inout = k;`, `t: inout = self.total;`) is another name for a variable or a field, of any
  type: assigning to it writes there, and it passes its address on to an `inout`
  parameter.  It names a place as an `inout` argument does (not an expression, an array
  element yet, a string's character, `self`, a `let` local or a `view`), of exactly its
  type, and keeps a field's object alive while it can be used.  In PIR, box slots now have
  addresses (`local.addr`, `field.addr`, `ptr.load`, `ptr.store`).  A local borrow of a
  variable is exclusive while it is live, from its declaration to its last use in its
  block (a loop that uses it keeps it live): the variable an `inout` local names cannot be
  used (`'k' is borrowed by 'inout' local 'x' until 'x' is last used`), and one a `view`
  local reads cannot change (`'c' is viewed by 'view' local 'v' until 'v' is last used;
  'tick' is not a 'view fn'`).  A borrow of a borrow keeps the first variable borrowed.
  A local borrow needs an initializer, is never `let` and is not a destructuring target; written before the name (`view x = e;`) the error shows
  the fix.  In the AST interchange format a local borrow carries `(qual view)` or
  `(qual inout)`.
- A `view` stays a `view` (docs/language/02-functions-and-calling.md): a `view` parameter
  or local can only be passed on to a `view` parameter, whatever its type (`print`,
  `println` and `open` take `view` parameters; the builtin methods read their arguments,
  except `push`; `Str(s)` returns `s` itself and takes none).  One that shares what it
  holds is never stored (assigned, put in an array or tuple, destructured, pushed) or
  returned, and a `match` arm's name for one is a `view` too.  Passing a `view` to an
  `inout` parameter now reads `'v' is a 'view' parameter; it cannot be passed to 'inout'
  parameter 'n'`.
- `view` and `inout` parameters (docs/language/02-functions-and-calling.md, "Parameter
  modes"), on functions, methods and constructors, for parameters of any type (type
  parameters included).  The mode is written where the type goes: `fn bump(n: inout int)`,
  `fn scale(x: inout float, by: view float)`, `fn show(c: view Counter)`, called as
  `bump(k)`.  A `view` parameter is read-only, and so is what it holds: no field or
  element write and only `view fn` calls through it.  An `inout` parameter is
  its caller's storage, passed by address so
  every write reaches the caller at once: a local variable, a parameter (an `inout` one
  passes its address on) or a field of an object (`obj.f`, `self.f`, `a.b.f`; by real
  address, never copied in and out, with the object kept alive for the call), of exactly
  the parameter's type (a subclass variable is not a base-class `inout` argument).  An
  `inout` parameter of a reference type is the caller's variable, not a second
  reference: passing it neither retains nor releases, and assigning to it replaces what
  the caller's variable holds.  A literal or other expression, `self`, a `view` parameter
  or anything reached through one, a `let` local, an array element (not yet) or a
  string's character is an error, and so is the same variable or field path passed to two
  `inout` parameters of one call.  An override keeps every parameter's mode; modes are part
  of a module's interface.  `view` and `inout` are reserved words, a syntax error anywhere
  but before a parameter's type; written before the name (`inout n: int`), the error shows
  the fix.  In the AST interchange format a parameter carries `(qual view)` or
  `(qual inout)`.  `samples/codegen/47_inout_view.pkn` shows them.

- `let` local declarations: `let n = 3;` and `let s: Str = "a";` declare a variable
  that cannot be reassigned (`'n' is declared with 'let' and cannot be reassigned`),
  neither by assignment nor by destructuring.  `let` needs an initializer and applies to
  local declarations only; it fixes the variable, not the object it refers to.  `let` is
  a reserved word now.  In the AST interchange format a `let` local carries `(let)`.

- Imported modules are compiled to `.pkm` module files (docs/design/pkm.md, prototype of
  phase A): the module's interface, its PIR and a manifest, portable across backends.
  A program's imports are cached as `.paykan_cache/<module>.pkm` and reused while the
  source, the toolchain and the interfaces of the module's own imports are unchanged
  (a body edit rebuilds one module, a signature edit its importers); where no source is
  found, a prebuilt `.pkm` next to it, under `--module-path=<dir>` or `$PAYKAN_MODULE_PATH`
  is loaded instead, and the program builds and runs identically on the c and llvm
  backends from it.  New: `--emit-pkm [-o f.pkm]`, `--module-path=<dir>`,
  `--rebuild-modules`, `--no-module-cache`, `--verbose`, and the `paykan pkm dump|check`
  command.  The backends' own caches stay beside the module files for now.
  `samples/imports/13_pkm` walks through it.  Libraries `paykan_pkm` (the file format) and
  `paykan_modules` (the resolver); the runtime ABI version moved to
  `PAYKAN_RUNTIME_ABI_VERSION` in `Runtime.h`.
- `native fn name(params) -> T = "c_symbol";` declares a function whose body is
  a C function written against the runtime ABI (#198): `int`, `float`, `bool`,
  `char`, `Str` and `Obj` cross by the runtime builtins' convention (`Str?` and
  `Obj?` as a result).  `native` is a reserved word.  In PIR the C function is
  a module-less `extern fn @$c.<symbol>`; both backends declare it from its
  signature.  `paykan` does not build native code: `--object=a.o,b.o` (a
  comma-separated list, repeatable) names the objects or archives that define
  it, and every backend links them (the llvm JIT loads them).  Sample:
  `samples/imports/14_native`.

### Changed

- An override of `toString` or `equals` is declared `view fn` (`view fn toString() -> Str`),
  like the `Obj` methods it overrides: a plain `fn` override is an error.
- `.pkm` interfaces carry a function's, a method's and a constructor's
  parameter modes (`view`, `inout`), so calls and overrides in another module
  are checked against them, whether the module is imported from source, from
  the cache or from a prebuilt file.
- PIR: four address instructions, `local.addr`, `field.addr`, `ptr.load` and
  `ptr.store` (docs/pir.md §6), which `inout` parameters lower to.  Only
  scalar (`i64`, `f64`, `bool`, `char`) slots have addresses.  A backend plugin
  must handle them.

### Removed

- The `mov` keyword (#145).  Using it is now an error: `'mov' was removed in v0.2.0;
  ownership transfers are inferred` (also from the AST interchange reader for a
  `(mov ...)` node).  Drop the keyword: `t = mov s` becomes `t = s`, with the same output
  and the same objects freed; `s` simply stays readable.  Until the last-use pass (#186)
  lands, each former `mov` costs one retain/release pair.  The use-after-move,
  loop back-edge and "cannot 'mov' ..." diagnostics went with it.  `mov` stays reserved.

### Fixed

- On macOS, programs are compiled and linked for the deployment target the
  runtime was built for.  CMake 4 builds the runtime for the host's macOS
  version, which can be newer than the system `cc`'s default; `ld` then
  warned on every link ("built for newer 'macOS' version") and the warning
  ended up in the program's stderr.

## [0.1.1] - 2026-10-06

A patch release of 0.1.0.  It accepts plugins built with 0.1.1 and 0.1.0:
no plugin interface changed.

### Added

- The release tarballs (Linux x86-64, macOS arm64) and the Debian package
  ship the llvm backend next to the c backend: `--backend=llvm` runs
  programs with the ORC JIT and builds them as native objects.  LLVM is
  linked into `paykan` statically, so nothing else is needed at run time.
  The c backend stays the default.  The Homebrew formula still builds the
  core (c only).

### Fixed

- The llvm backend builds with GCC 15 (#163): libstdc++ 15 warns on LLVM
  17's `<ciso646>` include in C++20, which `-Werror` made fatal.  The
  targets that include LLVM headers no longer turn warnings into errors;
  the move to LLVM 20 (#164) removes the warning itself.
- `find_package(Paykan)` no longer requires LLVM when the installed
  PaykanLang has the llvm backend: only a driver that links the llvm
  backend (`paykan_add_driver`) needs it, so plugins build against the
  Debian package without LLVM installed.
- The Homebrew formula has the real checksum of the release's source
  archive (#161).

### Changed

- The tests are no longer built by default: a plain `cmake -B build` builds
  only the compiler and downloads nothing.  They are now in suites
  (listed once, in `tests/suites.json`): `-DPAYKAN_BUILD_TESTS=<suites>` builds the
  named ones (a CMake list, e.g. `"parser;sema"`) and
  `-DPAYKAN_BUILD_ALL_TESTS=ON` all of them.  `PAYKAN_BUILD_TESTS=ON` is an
  error that points to `PAYKAN_BUILD_ALL_TESTS`.
- CI builds and runs only the test suites a pull request can affect
  (`scripts/affected_tests.py`); pushes still run every suite.

## [0.1.0] - 2026-10-06

The first tagged release. PaykanLang is now split into a core, pluggable
frontends and pluggable backends around a backend-neutral IR (PIR); the
default build needs only CMake and a C++20 compiler, plus a C11 compiler at
run time. Out-of-tree frontends and backends are loadable modules with a C
interface that the installed `paykan` loads at startup. Tuples, optional types
(optional primitives and optional-mode `match` included) and generics are
stable in v0.1 (see "Stable in v0.1" under Added).

### Breaking changes

- **The default build is the core: the recursive-descent frontend and the C
  backend (#123).** A plain `cmake -B build` builds only those two and
  downloads nothing. The LLVM backend, with its JIT, is opt-in at configure
  time: `-DPAYKAN_BACKENDS="llvm;c"`. The release tarballs and the Homebrew
  formula ship the core only.
- **The default backend is c, in every build (#27).** The default backend
  changes from llvm to c, and stays c even when the LLVM backend is built:
  with `-DPAYKAN_BACKENDS="llvm;c"` (in any order) `--list-backends` shows
  `c (default)`. Running a program through the JIT now needs
  `--backend=llvm`; without it, `paykan` runs programs with the C backend,
  which needs a C11 compiler (`cc`, or `$CC`) at run time. A configuration
  without the c backend (never produced in tree) falls back to the first
  backend listed. The default frontend stays recursive-descent.
- **The Bison frontend moved to its own repository (#60).** The hand-written
  recursive-descent frontend (standard C++ only) is the only in-tree frontend
  and the default; the Bison/Flex frontend (#17), which implements the same
  grammar (`docs/grammar.md`) and builds the same AST, is the out-of-tree plugin
  [PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend),
  built against an installed PaykanLang with `find_package(Paykan)`; it
  produces a `paykan` driver with `--frontend=bison`. PaykanLang no longer
  builds or downloads Bison, Flex or m4: `bison` is no longer
  accepted in `PAYKAN_FRONTENDS` (which lists in-tree frontends only), and
  the full build is `-DPAYKAN_BACKENDS="llvm;c"`. The frontend differential
  check (`scripts/diff_frontends.py`, the "Frontend differential" CI job)
  moved with it. `--trace-parser` / `--trace-scanner` stay, for any
  frontend that has traces.
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
- **Backend plugins are loadable modules with a C interface (#141).** An
  out-of-tree backend is no longer a static C++ library linked into a
  `paykan` driver of its own: it is a shared library (`.so`, `.dylib`) with
  the pure C11 interface of `include/paykan/plugin_api.h`, which the
  **installed** `paykan` loads at startup, with no rebuild of PaykanLang. It
  links nothing of PaykanLang, so it can be written in any language with a C
  layer (C, C++ with any compiler, Rust, ...). It receives the verified
  program as PIR text and talks to `paykan` through a host table
  (diagnostics, output, allocator, log, the runtime's paths, link and run
  helpers). For plugin authors: `paykan_add_backend_plugin()` now builds such
  a module (`MODULE`, `lib<target>.so` / `.dylib`) instead of a static
  library linked with `Paykan::backend`, and a backend written against
  `Backend.h` must be ported to `plugin_api.h` (see
  `docs/writing-a-backend.md`). `src/Backends/PrintPIR` is now a plain-C plugin. The
  static C++ path (`paykan_add_driver`) stays as an advanced option, and the
  built-in plugins keep their in-process C++ interface.
- **Frontend plugins are loadable modules too (#141).** A frontend plugin
  receives the source text through the same C interface and returns the
  program as text in the new, versioned **AST interchange format**
  (`docs/plugins/ast-format.md`, an S-expression form with source
  locations), which `paykan` reads, checks and hands to Sema; syntax errors
  go through the host's diagnostics. `paykan_add_frontend_plugin()` now
  builds such a module instead of a static library linked with
  `Paykan::frontend`, and `paykan_add_frontend_tests()` takes the plugin as
  `PLUGIN <target-or-file>` (instead of `PLUGINS <lib>...`): the suites load
  it with the installed `paykan`'s loader, and a new
  `InstalledPaykan.<frontend>` test runs the installed `paykan` with only
  the plugin loaded over the samples corpus.

### Added

- **A Debian/Ubuntu package.** Each release carries
  `paykanlang_<version>_amd64.deb`, built by CPack (`cmake/Packaging.cmake`,
  `cpack -G DEB`) from the core build and installable with
  `sudo apt install ./paykanlang_<version>_amd64.deb`; it depends on `gcc` or
  `clang` for the C backend.
- **The release tag is checked against the built version.** The version is
  spelled only in the top-level `CMakeLists.txt` (`project()` plus
  `PAYKAN_VERSION_PRERELEASE`); the accepted plugin list starts with it, and
  the release workflow fails when a tag `vX.Y.Z` does not match
  `paykan --version`.
- **Stable in v0.1: tuples, optional types and generics (#27).** They are no
  longer marked prototype/experimental: their documented syntax and semantics
  (`docs/language/09-tuples.md`, `10-optionals.md` with optional primitives
  and optional-mode `match`, `11-generics.md`) are covered by the
  compatibility promise for the 0.1 series. The documented limitations stay: importing generics
  across modules is not supported until v0.2.0 (#57), and a present optional
  primitive is boxed (#96 changes that representation, not the semantics).
  Plugins are versioned separately: the C plugin interface by its plugin API
  version, plugin builds by the release's list of accepted versions (#103).
- **Pluggable frontends (#16, #17).** The frontend (lexer and parser) is a
  plugin behind an interface (`include/paykan/Frontend.h`) and a registry:
  `-DPAYKAN_FRONTENDS=<list>` picks the frontends to build,
  `--frontend=<name>` selects one at run time and `--list-frontends` lists
  them. The grammar every frontend implements is specified in
  `docs/grammar.md`, and `--dump-tokens` prints a frontend's token stream.
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
  `src/Backends/PrintPIR` is the smallest complete backend; CI and the
  `PrintPIROutOfTree` ctest build it against an installation.
- **Documented: selective imports, `Stdin`, `File.readbytes` / `File.read`
  and system imports.** These worked before but were not listed:
  `import lib::{foo, bar as b};` imports several modules of one directory;
  `Stdin` is a builtin `File` over standard input; `readbytes(n)` and
  `read()` read `n` bytes or the rest of a file (`None` at the end);
  `import ::io;` looks a module up in `$PAYKAN_STDLIB` (no standard library
  ships yet, #113). See `docs/language/01-language-basics.md` and
  `06-modules.md`.
- **Generics.** Generic classes (`class Box<T> { … }`,
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
- **Optional types `T?` (issue #5).** Any reference type (a class,
  `Str`, or an array) has an optional form `T?` holding either a `T` or
  `None`. `T` widens to `T?` implicitly; a `T?` never narrows back without a
  `match`, whose `T` arm binds every present value and whose `None` arm (or
  `_`) covers the absent case. `x == None` / `x != None` is a null check,
  two optionals compare with the usual `equals` once both are present, an
  optional field is implicitly `None` if `__init__` does not assign it, and
  `T?[]` / `T[]?` are supported. At runtime a `T?` is the same reference-
  counted box as a `T` with "no box" meaning `None` — no layout change.
  Nested optionals, flow typing, `if let`,
  `??` and `?.` are not supported in v0.1. See
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
- **Tuples (#4).** Fixed-arity, heterogeneous, immutable values:
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
- **Run-time plugin loading (#141).** `paykan` loads plugins from, in order:
  `--plugin=<file>` (repeatable), each directory of `$PAYKAN_PLUGIN_PATH`,
  `~/.paykan/plugins/<version>/`, and the installation's
  `lib/paykan/plugins/<version>/` (found relative to the executable, and
  installed empty). `--no-plugins` / `PAYKAN_NO_PLUGINS=1` turn the directory
  search off. Each plugin's descriptor (from its one entry point,
  `paykan_plugin_init`) is checked before any of its callbacks run: the
  plugin API version (`PAYKAN_PLUGIN_API_VERSION`, 1), then its build
  version against the release's list (#103). An incompatible plugin is
  listed with the reason and its file, and selecting it exits with status 2;
  a file that can't be loaded, has no entry point or a malformed descriptor
  is listed as `rejected plugin <file>: <why>` (and stops a compile with
  status 2 when named with `--plugin`); a name two plugins provide is
  ambiguous and can't be selected. `--list-backends` and `--version` show
  each loaded backend's file; `--version` also prints the plugin API version,
  the plugin directories and every plugin file. The dynamic loader sits
  behind the portability layer (POSIX `dlopen`; a documented Windows stub).
  The CMake package adds `Paykan::plugin_api`, `PAYKAN_PLUGIN_API_VERSION`,
  `PAYKAN_PLUGIN_INSTALL_DIR`, `PAYKAN_EXECUTABLE`,
  `paykan_install_plugin()` and `paykan_check_plugin_built_with()`.
  New docs: `docs/plugins/` (overview, the C API, the AST format, PIR for
  backends).
- **The AST interchange format (#141)**: its writer and reader in the core
  (`paykan/ast/Interchange.h`, `Paykan::ast_interchange`), `paykan
  --emit-ast` to print a program in it, and `src/Frontends/ASTText`, a
  plain-C frontend whose source language is the format itself. The writer
  and reader round-trip the AST of every program in the samples corpus.
- The installed static libraries are built as position-independent code, so
  a plugin may link them into its module (a C++ frontend reusing the core's
  AST and writer internally).
- **Plugin compatibility check (#103).** Every frontend and backend plugin
  records the PaykanLang version it was built with:
  `PAYKAN_REGISTER_FRONTEND` / `PAYKAN_REGISTER_BACKEND` capture
  `PAYKAN_PLUGIN_BUILD_VERSION` from the installed headers
  (`paykan/PluginCompat.h`, generated). Each release holds an explicit list of
  the plugin build versions it accepts, in one place
  (`cmake/PluginCompat.cmake`), compiled into the core and exported in the
  CMake package as `PAYKAN_PLUGIN_COMPATIBLE_VERSIONS`. Versions match as
  exact strings, pre-release label included. The registry checks a plugin at
  registration and at selection; a plugin not on the list is listed by
  `--list-frontends` / `--list-backends` as
  `<name> (incompatible: built with PaykanLang <v>; this paykan <v> accepts
  <list>)`, is never instantiated, and selecting it with `--frontend=` /
  `--backend=` fails with exit status 2. `paykan --version` lists the
  accepted versions and every plugin with its build version and
  compatibility. The built-in plugins go through the same check.
- **`paykan_add_frontend_plugin()` / `paykan_add_backend_plugin()`** in the
  CMake package: they add a plugin library and fail at configure time when
  the installed Paykan does not accept the version the plugin is built with
  (by default the installation's own, `PAYKAN_TOOLCHAIN_VERSION`, or the one
  pinned with `BUILT_WITH <version>`). `src/Backends/PrintPIR` uses
  `paykan_add_backend_plugin()`. See `docs/writing-a-backend.md` (section 7)
  and the new `docs/writing-a-frontend-plugin.md`.
- This release accepts plugins built with `0.1.0`. From now on each
  release's entry states the plugin build versions it accepts.
- **Test support for out-of-tree frontends (#60).** An installation carries
  the frontend-parameterized parser and Sema suites, the fuzz smoke and
  differential tests and the samples corpus, and `find_package(Paykan)`
  provides `paykan_add_frontend_tests()`, which builds them against a
  plugin's frontend (`docs/writing-a-frontend-plugin.md`).
  `-DPAYKAN_INSTALL_TEST_SUPPORT=OFF` leaves them out.

### Changed

- **Repository layout.** The language reference moved from its top-level
  directory into `docs/language/` (same file names). The example
  out-of-tree backend moved from `examples/backends/print-pir` to
  `src/Backends/PrintPIR`. `example_program/` is gone: its `calc` program is now
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
- **Dependencies.** PaykanLang depends only on standard C and C++; GoogleTest
  (tests), LLVM (the opt-in `llvm` backend) and Python 3's standard library
  (the scripts) are the only other tools it uses. The script-driven ctests
  now run with the Python CMake finds (`find_package(Python3)`: the system's,
  or `-DPython3_EXECUTABLE=<path>`) instead of whatever `python3` is on
  `PATH`, and are skipped when there is none. The example plugin backend
  lives in `src/Backends/PrintPIR`.

- **The plugin registration ABI (#103).** `paykan::plugin::Registration`
  takes a `PluginInfo { Name, Create, BuildVersion }` (plain data, the version
  a `const char *`), and registry entries carry `BuildVersion`, `Compatible`
  and `Incompatibility`. `Registry::create()` returns null for an
  incompatible plugin. Plugins that register through the macros need only a
  rebuild. The check lives in a new core library, `paykan_plugin`
  (`Paykan::plugin`), which `Paykan::frontend` and `Paykan::backend` link.
- `paykan --version` prints one more line (`accepts plugins built with
  PaykanLang <list>`), and each plugin line now ends in
  `(built with PaykanLang <v>, compatible)`; a backend's description follows
  after `: ` instead of in parentheses. With #141 it also ends with
  `plugin API <n>`, `plugin directories: ...` and one line per plugin file,
  and a loaded plugin's lines end in ` [<file>]`.
- Registry entries (`paykan::plugin::Registry<I>::Entry`) also carry the
  plugin's `Path`, `Description` and `Conflict`, and their factory is a
  `std::function` (#141).

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
- **Documentation matches the compiler (#121).** The final audit's
  discrepancies are fixed. README: the build prerequisites and defaults.
  Language reference: array printing, selective imports
  (`import a::{b, c as d}`), system imports and `PAYKAN_STDLIB`, `File`'s
  `readbytes` / `read`, `Stdin`, the boxed types, `Str<int>` as a
  conversion, and diagnostics quoted as the compiler prints them.
  `docs/grammar.md`: the comparison-in-arguments case and the nesting limit
  on every frontend. `docs/pir.md`: `destroy`, `bool<Str>` and
  `PaykanBool_from_str`. `docs/c-backend.md`: LLVM is an in-tree, opt-in
  backend. The `MarkdownLinks` and `DocExamples` ctests keep links and
  stated outputs checked.
- `Driver.BuildAndRunDefaultToO2` failed in every sanitizer and coverage
  build (#122), because it expected the C cache key to end in ` -O2`, but
  instrumented builds append their own flags after the level. The test now
  parses the key's compile flags and checks for the `-O` token.
- **Release and CI (#133).** `release.yml` publishes a tag with a
  pre-release suffix (`v0.1.0-alpha`, `-rc1`) as a GitHub pre-release that
  is never marked "Latest", and a "Release plan" job shows that decision on
  every run, dry runs included. The barebones CI checks written as
  `! grep …` partway through a script never failed, because `bash -e`
  ignores a negated command; they are now explicit `if …; then exit 1; fi`
  checks. The Lint job runs actionlint, with shellcheck, and any finding
  fails it.
- **A truncated plugin file no longer crashes `paykan`.** A plugin whose file
  ends inside its loadable segments (an interrupted copy, download or install)
  was mapped by `dlopen` and killed every command with SIGBUS, `--version`
  included. On Linux the loader now checks the file's ELF program headers
  against its size first and lists the file as a rejected plugin.
- **A name two plugins provide is reported as ambiguous even when the first
  provider is incompatible.** The listings, `--version` and selection used to
  report only the first provider's incompatibility.
- **Bounded build parallelism.** README, CI, the release workflow, the Homebrew
  formula and the out-of-tree frontend test passed a bare `--parallel`, which
  with the Makefile generators is an unlimited `make -j`; they now pass the
  number of processors.

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

[Unreleased]: https://github.com/parsabee/PaykanLang/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/parsabee/PaykanLang/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/parsabee/PaykanLang/releases/tag/v0.1.0
