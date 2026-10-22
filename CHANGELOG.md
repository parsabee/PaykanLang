# Changelog

All notable changes to PaykanLang are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Expression bodies (docs/language/03-functions.md, "Expression Bodies";
  docs/language/14-function-types.md): a function or a lambda whose body only
  returns an expression is written with `=>`, `fn double(x: int) -> int => x
  * 2;` for `fn double(x: int) -> int { return x * 2; }`, and
  `fn(x: int) -> int => x * 2` for a lambda. The parser makes the body one
  `return`.
  - `=>` is a new token. `=` stays a native function's (its C symbol), and
    after any other function's signature it is an error that names `=>`.
  - Without the result type, `fn double(x: int) => x * 2;` or
    `fn(x: int) => x * 2`, the result type is the expression's. Sema infers a
    function's before checking any other body, so it may be used before its
    declaration; a generic function's is inferred for each instantiation.
  - An expression with no value, and a function whose expression depends on
    its own result type (`fn fact(n: int) => ... fact(n - 1)`), are errors that
    ask for the result type. A method writes its result type (`fn area() ->
    int => ...`); a `static fn` infers.
  - The AST interchange format marks such a function or lambda with
    `(inferred)` (docs/plugins/ast-format.md), so a template in a `.pkm` keeps
    it.
- Untyped parameters (docs/language/03-functions.md, "Untyped Parameters"): a
  free function's parameter may leave out its type, `fn double(x) => x * 2;`,
  which the parser makes `fn double<T>(x: T) => x * 2;`. Each untyped parameter
  adds a type parameter of its own, after the written ones, so the function is
  generic and each call instantiates it for its argument types.
  - With an expression body, the result type is inferred for each
    instantiation; a block body cannot name the added types.
  - A lambda's, a method's, a `static fn`'s, a trait method's, a native
    function's and `main`'s parameters still need types.
  - The added type parameters are named `typeof(x)`, which no program can
    write, in the AST and its interchange format.
- Two parameters with the same name in a function or a method are an error
  (`duplicate parameter 'x' in function 'f'`), as they were in a lambda. They
  used to be accepted, and a program using one failed in code generation.
- Compound assignment (docs/language/02-basics.md, "Compound assignment"):
  `x += e`, `-=`, `*=`, `/=` and `%=`, on a variable, a field or an element.
  The parser builds `x = x op e`, so the rules of `=` and of the operator
  apply, and the target is evaluated twice (`xs[f()] += 1` calls `f` twice).
  `+=`, `-=`, `*=`, `/=` and `%=` are new tokens (docs/grammar.md).
- `for` loops over ranges of integers (docs/language/02-basics.md, "for"):
  `for i in a..b { }` counts from `a` to `b - 1`, `a..=b` includes `b`, and
  `step s` sets the distance between the values (a negative one counts down).
  - The range is of `int`, a sized integer or `unsigned`, one type for both
    ends: a literal takes the other end's type, and `for b: u8 in 0..=255`
    names it. The step is an `int`.
  - What the loop does is fixed when it starts: the start, end and step are
    evaluated once, in that order, and the variable is read-only in the body.
  - `for _ in 0..n` runs the body `n` times without naming the value.
  - The number of iterations is known before the first, so a loop always ends,
    even at the end of its type: `for b: u8 in 0..=255` runs 256 times.
  - A constant step of `0` is a compile error; a computed one panics when the
    loop starts (`the step of a 'for' loop is 0`).
  - `for` is a keyword (a program can no longer name anything `for`), and `..`
    and `..=` are tokens; `in` and `step` mean something only in a `for`
    header, so a variable may still be called `in` or `step`. `0..n` lexes as
    `0` `..` `n`, never as the float `0.`.
  - The AST interchange format has a `(for ...)` statement
    (docs/plugins/ast-format.md). The loop lowers to the PIR `while` loop, so
    the PIR and the backends are unchanged apart from one new runtime function,
    `Paykan_panic_for_step`.

- The `mlir` backend (docs/design/mlir-backend.md), which replaces the C and llvm backends' two
  translations of PIR with one lowering through MLIR.  It imports the program into `pir`, an
  MLIR dialect with an op for each PIR item, statement and instruction, and lowers that once
  for both of its targets: to func, scf and arith, plus the pir ops for memory, globals and
  optionals; structured control flow with `break`, `continue` and early `return` becomes
  `scf.if` / `scf.while` with an exit flag, objects get their C layouts, and every symbol a
  link name that is a C identifier.  From there the LLVM path: upstream conversions take
  func, scf, cf and arith to the LLVM dialect and a pattern each the remaining pir ops, and
  MLIR translates it to LLVM IR, which `paykan run` runs with the JIT and `paykan build`
  builds into a native object, its imported modules cached as `<module>.bc`.  And the C
  path: upstream conversions take func, scf and arith to EmitC and a pattern each the
  remaining pir ops, and MLIR's C emitter prints the program's C (`--emit-c`).
  `--emit-mlir=<stage>` prints the program at a stage of the pipeline: `pir`, `scf` (after
  the shared lowering), `llvm` or `emitc`.  The `pir-mlir` test suite imports every item and
  instruction of PIR and every sample program, checks that the MLIR verifies and parses back
  to the same text, and that the shared lowering leaves only those ops.
- A `.pkm` holds its module's generic classes, structs and functions in a `TMPL` section
  (docs/design/pkm.md §4.3), so an importer instantiates them from a prebuilt or cached module
  as from its source (#160).  Every module compiles its own copy of the instantiations of
  other modules' templates it uses, so its `.pkm` is the same in every program, and a program
  keeps one copy of each.  Modules with templates, or with such copies, are now cached; an
  entry records the templates' hash of every module it imports, so editing a generic body
  recompiles the modules that may hold a copy of it.  `--emit-pkm --module-name=<name>`
  names the module the file records.
- PIR passes (docs/pir.md §12): a pass manager, and a dead-code pass that erases what no root
  of a program reaches, following calls, symbol operands, the classes instructions name,
  vtables and the declarations between modules.  `paykan` runs it on every program with
  `main` as the root before the backend sees it: an executable, and what `run` compiles,
  hold only the code the program uses, and `--emit-pir`, `--emit-llvm`, `--emit-c` and
  `--emit-mlir` print that.  A module's cached LLVM IR is keyed on the part of it the
  program uses.
- A `.pkm` keeps only the code its module and its importers can reach, and compiles most of
  it (docs/design/pkm.md §5.13): the module's own code stays, with its copies of the
  instantiations its interface exposes, and its other copies only as far as its own code
  uses them.  The copies stay PIR (marked `shared`, docs/pir.md §3), which every program
  shares and prunes; the rest is compiled by the backend (LLVM bitcode, for the mlir
  backend) into a new `OBJ` section, which the module's PIR declares, each function with the
  names its code uses.  A program links only the part of that code it uses.  A cache entry
  whose code another backend or another LLVM compiled is rebuilt.  The standard library
  ships compiled.
- `paykan pkm extract --object <file.pkm>` writes a module's compiled code as a native
  object, which a program's `--emit-c` C is linked with; `paykan pkm dump --section=obj`
  shows who compiled it.  `paykan --version` names the standard library's directory.
- Backends may compile modules (the `CompileModules` capability: `compileModule`,
  `objectKind`, `emitModuleObject`, and `Input::Compiled`, docs/writing-a-backend.md).

### Changed

- The standard library's containers hand out an element two ways (docs/stdlib/collections.md).
  `view fn`s lend it as a `view`, which stays the container's: `List`'s `at` and `last`,
  `LinkedList`'s `front` and `back`, the `peek`s, and `HashMap`'s `at` and `atOr`.  The new
  `get`s hand over a copy the caller keeps (`-> T`): `List.get(i)`, `HashMap.get(k)` and
  `getOr(k, otherwise)`, `LinkedList.getFront()` and `getBack()`, and `get()` on `Stack`,
  `Queue` and the priority queues.  A copy of an object shares it with the container, so the
  `get`s are not `view fn`s, and a `view` of a container reads with `at` (`'xs' is a 'view'
  parameter; 'get' is not a 'view fn'`).  `List.get` and `HashMap.get` / `getOr` were the
  lending reads; code that reads through a `view` of the container, or binds the result to a
  `view` local, now calls `at` / `atOr`.  `memory::Buffer<T>` follows suit: `at(i)` lends a
  slot, and `get(i)` is the copy that was `copyUnchecked(i)`.
- `--emit-c` prints the C of the program's PIR.  A module loaded from a module file with
  compiled code (the standard library's, an import's cache entry) is linked as an object
  (`paykan pkm extract --object`), and the C keeps everything that code uses
  (docs/c-backend.md).  A loaded backend plugin gets such a module's code declared, not
  defined (docs/plugins/pir-for-backends.md).

- The standard library is installed as `.pkm` files only: each module is compiled, as
  `::<name>`, against the `.pkm` files of the modules it imports, and a program loads all of
  them from those files instead of compiling the generic ones from their sources every time.

- A `match` on an enum, or on integer, `char` or `bool` literals, is a `switch` statement in
  PIR (docs/pir.md §6), a case per value and the wildcard its default, instead of an if-else
  chain; one on floats, classes, optionals or strings stays a chain.  Both targets switch on
  it: the LLVM IR at every optimisation level (`switch i64`), and `--emit-c`'s C (`switch`).
- LLVM is 22.1.8 (was 17.0.6, for the llvm backend), whose prebuilt release includes MLIR.
  Only what PaykanLang uses is extracted (headers, the LLVM and MLIR libraries and the tools
  the build and the scripts run), about 2 GB of the archive's 12.  The prebuilt LLVM links
  with zlib and zstd from the system (`zlib1g-dev libzstd-dev` on Debian and Ubuntu, `brew
  install zstd` on macOS); a configure without them says so.  The vendored clang-format and
  clang-tidy are LLVM 22's too.
- `mlir` is the only backend built in this repository, always built and the default:
  `paykan --list-backends` prints `mlir (default)` and the configure `Backends: mlir
  (default: mlir)`.  `paykan run` runs a program in process with LLVM's ORC JIT, with no C
  compiler involved; `paykan build` emits a native object and links it against
  `libpaykan_runtime.a` with the system C compiler.  Other backends are plugins
  (docs/writing-a-backend.md).
- `--emit-c` prints the mlir backend's C, made with MLIR's EmitC: the whole program as one
  strictly conforming ISO C11 translation unit (docs/c-backend.md, now "The C output").  The
  CStrictC11 test (scripts/c_strict.py) compiles it for every runnable sample with
  `-std=c11 -pedantic-errors -Wall -Wextra -Werror` on GCC and Clang, at `-O0` and `-O2`,
  and runs it like `paykan run`.  Vtables are arrays of Runtime.h's `PaykanMethod`: an
  object's vtable pointer is read as a `PaykanMethod *` and a slot as a `PaykanMethod`,
  converted to the method's type at the call through `uintptr_t` (#62).  `--emit-llvm` is
  the mlir backend's LLVM IR (its `--emit-source`); `--emit-llvm`, `--emit-c` and
  `--emit-mlir=<stage>` need the mlir backend and exclude each other.
- A default configure needs LLVM 22.1.8 with MLIR: it uses `-DLLVM_DIR=<llvm>/lib/cmake/llvm`
  when given, and otherwise downloads the prebuilt release once into
  `build/third-party/llvm`.  So the core build is no longer standard C++ only, and a plain
  configure is no longer one that downloads nothing (#16, #77, #123): it downloads LLVM and
  nothing else.  The tests stay off by default, and GoogleTest is fetched only when they are
  enabled.  The DefaultConfigure test configures with no option but the enclosing build's
  `LLVM_DIR` and checks that the defaults are recursive-descent + mlir, with nothing
  downloaded.
- The bitcode module cache (`.paykan_cache/<module>.bc`) is the only backend cache.
- The release tarballs and the Debian package ship the defaults, the recursive-descent
  frontend and the mlir backend, with LLVM linked in statically.  Homebrew installs the
  release's prebuilt tarballs (macOS ARM64, Linux x86-64 and ARM64) instead of building from
  source ([parsabee/homebrew-paykanlang#2](https://github.com/parsabee/homebrew-paykanlang/pull/2)).
- CI: the "Barebones | Linux x86-64" job, which built without LLVM, is replaced by "Install |
  Linux x86-64": a plain configure, installed, with plugins loaded into the installed
  `paykan`.  Every job builds the defaults.

### Removed

- The `c` backend: the C emitter and its build (src/Backends/C: CEmitter, CBackend, CBuild,
  CNames; include/paykan/backends/c), the `paykan_c_build` library and the C object cache
  (`.paykan_cache/<module>.<hash>.{c,o,key}`).  `--emit-c` is the mlir backend's.
- The `llvm` backend: src/Backends/LLVM/LLVMBackend.cpp and PIRToLLVM.cpp.
  src/Backends/LLVM/JIT and src/Backends/LLVM/CodeGen stay, as the mlir backend's JIT and
  native code generation.
- `--backend=c` and `--backend=llvm` (unknown backends now).  In `PAYKAN_BACKENDS` the names
  `c` and `llvm` are still accepted, with a warning, and build `mlir`.

## [0.2.0] - 2026-10-20

### Added

- Linux ARM64 (aarch64) release builds: a `paykanlang-<version>-linux-arm64.tar.gz` tarball
  and an `arm64` Debian package, built and smoke-tested like the x86_64 ones, with the LLVM
  backend (the pinned LLVM 17.0.6 aarch64 build is downloaded at configure time).
- `::result` (stdlib/result.pkn, imported `as res`): errors as values.  `enum ErrorCode`,
  the language's standard set of errors (`Ok`, `NotFound`, `PermissionDenied`,
  `AlreadyExists`, `InvalidArgument`, `OutOfMemory`, `Io`, `EndOfFile`, `OutOfRange`, ...),
  `struct Error { code: ErrorCode; msg: Str?; }` and
  `struct Result<T> { error: Error; value: T?; }`, the value present exactly when the code
  is `Ok`, with `ok(v)`, `none()`, `name(code)`, `fromErrno(errno)` and `isOk()` /
  `message()`.  Every standard library function that can fail returns one: `io::open`,
  `io::File`'s methods, `memory::Buffer`'s.  The runtime maps a C `errno` to its code
  (`Paykan_error_code`) and text (`Paykan_error_text`).
- `::io` (stdlib/io.pkn): `print`, `println`, `printerr` and `printerrln` take any
  `T: Show` (so `io::println(5)` needs no `Str(...)`); `class File`, whose `impl` methods
  `write` returns an `Error` and `readln`, `readbytes` and `read` a `Result<Str>`
  (`EndOfFile` at the end); `open(path, mode) -> Result<File>`; and `stdin() -> File`.
- Optionals of enums, tuples and structs (docs/language/12-optionals.md): `Suit?` is held
  like `int?`, a `(int, Str)?` or a `Point?` in place as a presence flag and the value,
  `None` all zeros.  They are made, compared (`== None`, `==`), unwrapped by `match`
  (`p: Point { ... } None { ... }`), printed, and stored in fields, arrays, tuples,
  parameters and results like any value.  A template's `T?` of an optional `T` is `T`
  itself, so `Result<Str?>` works (a written `int??` is still an error).  The value of an
  optional tuple or struct is at most 64 bytes: `Big?` for a 72-byte `Big` is rejected (`use
  'Box<Big>?' (memory::Box) instead`).  A nested tuple literal widens into an optional slot
  (`a: (int, (Str, Str)?) = (1, ("x", "y"));`).
- An optional of a `Show` type is `Show`: `x.toString()` on an optional is "None" or its
  value's text.
- `::math`, a standard library module of the C library's math functions on `float`
  (docs/stdlib/math.md): `sqrt`, `cbrt`, `pow`, `hypot`,
  the exponentials and logarithms, the trigonometric and hyperbolic functions,
  `floor`, `ceil`, `trunc`, `round`, `fmod`, `remainder`, `abs`, `min`, `max`,
  `copysign`, `erf`, `erfc`, `gamma` and `lgamma`, each a native function bound
  directly to its `<math.h>` symbol, plus `pi()`, `e()`, `inf()`, `nan()`, `isNaN`,
  `isInf` and `isFinite` in Paykan, with a stdlib test and a docs example.
- Sized numeric types (docs/language/02-basics.md, "Sized numeric types";
  docs/design/sized-types.md): `i8`, `i16`, `i32`, `u8`, `u16`, `u32`, `unsigned`, `f16`
  and `f32`, with `i64`, `u64` and `f64` as other spellings of `int`, `unsigned` and
  `float`.  Every numeric value takes an 8-byte slot, like an `int`, so a sized type
  changes no layout, runtime structure or `.pkm` record.  The operands of an operation have
  one type (`int` with `float` stays the one promotion), integer results wrap to the type's
  width, `f16` and `f32` results are rounded after each operation, and an `unsigned`
  divides, takes the remainder and orders unsigned.  A numeric literal takes the sized type
  its context expects when it fits it (`x: u8 = 255`, `x + 1`, a `match` arm) and is an
  error when it does not.  The conversion constructors convert between every pair of
  numeric types (integer to integer keeps the low bits, float to integer truncates and
  panics outside the target's range, integer to float rounds), `Str(x)` prints every
  numeric type (`unsigned` without a sign) and `u8(s)` & co. parse a `Str` into a `u8?` &
  co.  Every numeric type is `Show` and `ValueType` and the integer ones `Hashable`.  PIR
  gains `wrap`, `mask`, `udiv`, `urem`, `ucmp`, `utof`, `ftou` and `fround32` (docs/pir.md
  §6) and the runtime `PaykanString_from_uint`, `PaykanString_parse_uint`,
  `Paykan_round_f16`, `Paykan_panic_float_to_type` and the layout kind
  `PAYKAN_KIND_UINT`.  Not yet: bitwise operators, literal suffixes, hexadecimal literals,
  `char` to the sized types, implicit widening.
- Weak references (docs/language/16-memory.md, "Weak references"; design in
  docs/design/weak-references.md): `weak y: Point? = x;` declares a local, and `weak
  parent: Node?;` a class field, that refers to an object without keeping it alive.  The
  type is `T?` of a class `T` (a user class, `Str`, `io::File` or `Obj`), inferred from the
  initializer when left off (`weak u = x;`), and a local without an initializer starts as
  `None`.  A weak reference reads as a `T?`: the object while something holds it, `None`
  once it has been destroyed; it is assigned a `T` or a `T?`, matched, compared with `==
  None`, passed, returned and copied into a strong variable or field (which then keeps the
  object), and a lambda captures a `weak` variable weakly.  It is the back link that breaks
  a reference cycle (a child's `parent`, a closure stored in the object it refers to).  A
  weak reference cannot be passed to an `inout` parameter, named by an `inout` local or
  bound by an `inout` match arm; `weak` is a keyword.  A struct field cannot be `weak` (a
  struct is a value); not yet: a `weak` tuple or array element, parameter or result.  In
  PIR a weak reference has the type `weak` and the ops `weak.new`, `weak.load` and
  `weak.release`; the runtime has `Paykan_weak_new`, `Paykan_weak_load` and
  `Paykan_weak_release`; a `.pkm` interface marks a weak field with a flags byte (`pkm
  dump` prints `weak field parent: Node?`).
- Structs (docs/language/11-structs.md): `struct Point { x: int; y: int; }` declares a
  value type with named fields, held in place and copied by assignment like a tuple, with
  no superclass, no vtable and no identity.  `Point(1, 2)` constructs one from its fields
  in order, or through an `__init__` declared in an `impl Point { ... }` block, which also
  holds its methods and `static fn`s; `p.x = 5` assigns a field in place (also through a
  field, an array element or an `inout`), and in a method `self` is the receiver's
  storage.  `==` compares field by field, `toString()` renders `Point(x: 1, y: 2)`, every
  struct implements `Show` and `ValueType`, and `impl Hashable for` and user traits work
  as for classes, so a struct is a key of `HashMap` and `HashSet`.  A struct crosses
  modules and prebuilt `.pkm` files (a `STRUCT` interface record), and the runtime renders
  and compares one through its layout (`PaykanStruct_toString`, `PaykanStruct_equals`,
  the layout kind `PAYKAN_KIND_STRUCT`).  Not yet: `match` on a struct, a fixed-size array
  field and an `impl` for another module's struct.
- Generic structs (docs/language/11-structs.md, "Generic structs"): `struct Pair<A, B>
  { a: A; b: B; }` is a template like a generic class, instantiated by its type
  arguments (`Pair<int, Str>`) or inferred from a constructor call (`Pair(1, "a")`, from
  the fields or from an `__init__` of an `impl Pair<A, B>` block), with `impl` and `impl
  Trait for` blocks and bounds, in every position a struct goes, across modules too.
  A `static fn` of its `impl` block is the instantiation's: `Pair<int, Str>::of(1, "a")`.
- Private names, at two levels (docs/language/05-classes.md "Private fields and methods",
  docs/language/15-modules.md "Private to the module", design in docs/design/visibility.md).
  A field or a method whose name starts with two underscores, `__x`, is private to its
  class or struct: it is reachable only from that type's own methods, `impl` blocks, trait
  impls and `static fn`s (`'__count' is private to 'Counter': it is reachable only from its
  own methods`), not from a subclass, the rest of the module or an importer; the default
  `__init__` still takes every field.  A name that starts with one underscore is private to
  its module: an importer cannot name a `_` free function, class, struct, enum or trait
  (`'m::_f' is private to module 'm'`), while the module's exported generic templates keep
  using it, and a `_` field or method is reachable from any code of the module that
  declares its type, a subclass and a free function included, and from no importer (`'_w'
  of 'Pub' is private to module 'lib::shapes'`).  A top-level declaration named with `__`
  is rejected (`a module's own function is spelled '_f'`).  A public function, field or
  method cannot name a private type (`'make' is public but its result names the private
  type '_Node'; make the function private or the type public`).  `fn ___init__(...)`
  (three underscores) in a class's or a struct's `impl` block is a module-private
  `__init__`: the type is constructed (`T(...)`) and extended (`__super__(...)`) by the
  module's own code only, its templates included wherever they are instantiated, and an
  importer makes one through the module's functions (`'m::T' cannot be constructed here:
  its initializer '___init__' is private to module 'm'`).  `io::File`'s is one: `open` and
  `stdin` make a `File`, a program cannot.  `__init__` and `__super__` keep their meaning.
  Private names stay in a `.pkm` interface and are refused by name.
- Function types and lambdas (docs/language/14-function-types.md, design in
  docs/design/function-types.md): `(int, float) -> bool` is the type of a function
  value, with the parameters' modes part of it (`(view Str, inout int) -> bool`) and the
  result a copy; `() -> R` takes no parameter, `-> void` returns nothing, `->` is
  right-associative, and `((int) -> int)[3]` / `((int) -> int)?` give a function type a
  suffix.  A value is a lambda `fn(x: int) -> int { return x + 1; }`, or a named function
  used without calling it (`square`, `m::square`, `Point::origin`).  A lambda captures the
  variables of the function it is written in by copy (a reference retained, a value held in
  place copied), read-only inside it; a `view`, an `inout`, a `view fn`'s `self` or a
  struct method's `self` cannot be captured.  A function value is called like a function
  (`f(1)`, `obj.cb(1)`, `fs[0](1)`), with the arguments checked and passed as for the
  function's own parameters.  Function types go in every type position, cross modules
  and prebuilt `.pkm` files, and are type arguments (`List<(int) -> int>`).  A function
  value is a value held in place (docs/design/function-types.md, "Run-time
  representation"): 16 bytes, the address of its code and the box of its closure, the
  object holding its captures, which is null when it captures nothing, so a named function
  or a lambda that captures nothing used as a value allocates nothing.  Copying one shares
  the closure; `==` is true for the same code and the same closure, so `square == square`
  is `True` and `List.contains(square)` finds it (within a module: a value of the function
  made in another module is a different value for now), while each evaluation of a
  capturing lambda makes a new closure.  A function type is a `ValueType`, not an `Obj`,
  and `((int) -> int)?` takes the same 16 bytes, `None` having no code.  A call loads the
  code and the closure, keeps the closure for the call and calls the code through its
  address, through the PIR instructions `fn.addr` and `icall` (docs/pir.md); at `-O2` LLVM
  makes the call direct where the code is known.  Not yet: borrowed results in a function
  type, methods and generic functions as values, inferred lambda parameter types, capture
  by reference.

- The standard library ships with the compiler (docs/stdlib/index.md), in
  `stdlib/`: `::collections` has `List<T>`, `LinkedList<T>` (doubly linked, each node
  holding the previous one as a `weak` reference so no reference cycles form),
  `Stack<T>` and `Queue<T>` (on a `LinkedList<T>`), `HashMap<K: Hashable, V>` and
  `HashSet<K: Hashable>` (separate chaining), and `MinPriorityQueue<T>` and
  `MaxPriorityQueue<T>` (binary heaps of elements pushed with an `int` priority, equal
  priorities popping in push order); `::memory` has `Buffer<T>`, a struct that is a handle
  to the runtime's storage, so its copies share the slots.  Every index and size of the
  library is an `unsigned` (`len()` returns one, `get(i)` & co. take one, and `indexOf`
  returns an `unsigned?`, `None` for a missing element), and the containers hold elements
  and keys of the sized numeric types too (`List<u8>`, `HashMap<u32, Str>`, `Box<f32>`).
  A broken precondition of a container panics (`pop from an empty Stack`).  A `Buffer`
  starts empty, `Buffer<T>()`, and its operations report what goes wrong instead of
  panicking: `grow`, `reserve`, `push`, `set`, `swap` and `shrink` return a `res::Error`
  (`OutOfMemory`, `OutOfRange`, `InvalidArgument`, `NotFound`) and `copy(i)` a
  `res::Result<T>`; `get(i)` and `copyUnchecked(i)` panic on a bad index, for code that
  has checked it.  The build compiles each module to a `.pkm` with the `paykan` it built
  and stages it with its source in `lib/paykan/stdlib/<version>/` next to `bin/paykan`;
  `cmake --install` installs them in `<prefix>/lib/paykan/stdlib/<version>/` and the
  release packages include them.  `-DPAYKAN_BUILD_STDLIB=OFF` leaves the library out, and
  it is off by default when cross-compiling.  The `stdlib` test suite
  (`scripts/stdlib_tests.py`) runs `tests/Stdlib` on every backend.
- A system import (`import ::collections;`) finds the installed standard library: it
  looks in `$PAYKAN_STDLIB` when it is set, then in `<prefix>/lib/paykan/stdlib/<version>/`
  next to the `paykan` executable when that exists, then in `<source-root>/stdlib`.
- The built-in trait `Hashable` (docs/language/06-traits.md, "Hashable"):
  `view fn hash() -> int`, implemented by `int`, `bool`, `char`, `Str` and the sized
  integer types, and by a class or a struct with an `impl Hashable for` block.  The hash
  of equal values is equal on every run and backend: the runtime's `PaykanInt_hash` (a
  `bool` or `char` hashes as its `int` value) and `PaykanString_hash` (FNV-1a over the
  bytes), both ending in the SplitMix64 finalizer.  Declaring a trait named `Hashable` is
  an error.
- The built-in trait `ValueType` (docs/language/06-traits.md, "ValueType"), with no
  methods: implemented by the types held in place, `int`, `float`, `bool`, `char`, the
  sized numeric types, every enum, tuple, struct and function type, and a fixed-size array
  (which cannot be a type argument yet).  A class, `Str` and an optional are not, and `impl
  ValueType for` a class is an error (`class 'P' cannot implement 'ValueType': a class is
  shared by reference, not held in place`).
- `memory::Box<T: ValueType>`: one value in a shared object, whose `get()` is the
  value's storage (`-> inout T`).
- Runtime functions for the standard library's panics, reached through `native fn`s:
  `Paykan_panic_message(Str)` prints `paykan: <message>` and aborts, and
  `Paykan_panic_value`, the same as a generic `native fn` with a result, for where a value
  is due.
- `[v; n]` is an array of `n` copies of `v` (docs/language/09-arrays.md, "Repeated
  values"): `slots: Entry?[8] = [None; 8];`, and `[[0; 3]; 2]` is an `int[2][3]` of zeros.
  The count is an integer literal of at least 1 (`[0; 0]` is an error, `an array has at
  least one element: the count must be 1 or more`), and the literal types as one of `n`
  elements, so `[0; 4]` flowing into a `float[4]` is four `0.0`s. `v` is evaluated once:
  `[Node(); 3]` holds one object three times. In PIR, `[0; n]` (or `0.0`, `False`,
  `'\0'`) of a scalar element type is one `mem.zero` of the array. In the AST interchange
  format it is `(array-repeat LOC EXPR INT)`.
- Traits (docs/language/06-traits.md): `trait Named { view fn name() -> Str; }` lists the
  methods a type must have, and `impl Named for A { ... }` declares that class `A` has
  them. The block's methods join `A` like an `impl` block's, and a method `A` already has
  meets a requirement too. Traits can be generic (`trait Into<T>`), and `Self` names the
  implementing type. A class implements its superclasses' traits, and every class
  implements the built-in `Show` (`view fn toString() -> Str`) through `Obj`, as do
  `int`, `float`, `bool` and `char` (`n.toString()` is `Str(n)`) and tuples. Bounds
  (`fn f<T: Show + Into<int>>`, `class Box<T: Named>`) are checked where the template is
  instantiated (`'Str' does not implement trait 'Named', which 'greet' requires of 'T'`).
  A trait is not a type.
- `impl` blocks (docs/language/05-classes.md, "`impl` Blocks"): `impl Point { ... }` adds
  `fn`, `view fn` and `static fn` methods to a class of the same module, and holds its
  `__init__`. They are called directly, not through the vtable, and subclasses inherit
  them. A `static fn` is called as `Point::origin()`, or `mod::Point::origin()` from an
  importer. No `impl` method may reuse a name the class or a superclass has, and none can
  be overridden.
- Generic `impl` blocks (docs/language/13-generics.md, "`impl` Blocks"): `impl Box<T> { ... }`
  and `impl Named for Box<T> { ... }` give each instantiation of `Box` the block's methods
  and trait, checked when it is instantiated, also in a module that imports `Box`. A
  `static fn` in one is the instantiation's, called as `Box<int>::of(3)`
  (`m::Box<int>::of(3)` through an import).
- Generic classes and functions of an imported module (docs/language/13-generics.md,
  "Modules"): `gen::Box<int>(1)`, `gen::Box("s")`, `b: gen::Box<Str>` and `gen::first(xs)`
  instantiate `gen`'s templates in the importer, in every position a local template may be
  used, its own templates included.  A body names what its module declares and imports,
  whatever the importer calls those names, and is checked per
  instantiation in the template's file (`in instantiation of 'lib::gen::show<int>'
  requested here`).  `Box<int>` is one class however many modules instantiate it, the
  template's module included, and is compiled once per program; inference through
  `Box<T>` parameters sees an imported `Box<int>` too.  A module with templates, with
  instantiations of another module's, or using such an instantiation, is built from source
  every time (a `.pkm` does not hold templates yet).  Instantiating both this module's
  `Box<Str>` and an imported template's is an error (`conflicts with another template's
  instantiation of the same name`).
- Runtime functions for the standard library's `memory::Buffer<T>`: `PaykanBuffer_new`,
  `_len`, `_get`, `_copy`, `_isSet`, `_set`, `_reserve`, `_grow` and `_shrink`
  (`Runtime.h`, "Buffer"), generic native functions over a `PaykanBuffer` of values of any
  size, laid out by their `PaykanType`.  New values are all zero bytes (0, False or None);
  the buffer retains the references it holds and releases them when they are overwritten,
  shrunk away or destroyed; growing at least doubles the allocation and shrinking to a
  quarter of it halves it.  An index out of bounds, a negative count, shrinking by more
  than the length and reading a never-set reference that may not be None panic (`buffer
  index 3 out of bounds (len=3)`); `PaykanBuffer_tryGrow` and `PaykanBuffer_tryReserve`
  report a failed allocation as `ENOMEM` instead of aborting (`memory::Buffer`'s
  `OutOfMemory`).
- Generic native functions (docs/language/03-functions.md, "Generic native
  functions"): `native fn pick<T>(first: bool, a: view T, b: view T) -> T = "c_pick";`.
  Each instance calls the same C function with a `PaykanType` for each type argument first
  (`Runtime.h`: the type's size, alignment, kind and reference offsets), and passes what
  the function types `T` by address (a `-> T` result through a `void *out`), so a value
  of any type and size crosses into C; `PaykanType_copy` and `PaykanType_drop` copy and
  release one.  In the AST interchange format a generic native function has its
  `(type-params ...)` like any other.  See `samples/imports/15_native_generic`.
- Borrowed results (docs/language/03-functions.md, "Borrowed results"): a
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
  override keeps how the method it overrides returns; `main` returns a copy.  The result's
  mode is part of a module's interface (a result-mode byte after a `.pkm` record's
  parameter modes), and in the AST interchange format it is a `(qual view)` after the
  return type.
- `match` arms that borrow the subject (docs/language/08-match.md, "Borrowing
  the subject"): `d: view Dog { … }` binds `d` as a `view` local of the subject, whatever
  the subject is, so nothing changes through it and it passes on only to `view`
  parameters; the variable the subject starts from cannot change until `d`'s last use in
  the arm.  `d: inout Dog { … }` binds `d` as the subject's storage, like an `inout` local:
  assigning to it writes the matched variable or field (a variable, a field, an optional,
  an `inout` parameter; not an expression, `self`, a `let` local, a `view`, or a primitive
  or an enum inside an optional yet), and the subject's variable cannot be used until
  `d`'s last use.  With the mode before the name (`view d: Dog`) the syntax error shows the
  fix.  In the AST interchange format the arm carries `(qual view)` or `(qual inout)` after
  its type.
- `view fn` (docs/language/05-classes.md, "Methods that don't change self"): a method
  declared `view fn len() -> int` does not change `self`, and is the only kind of method
  that can be called on a `view` (`'c' is a 'view' parameter; 'tick' is not a 'view fn'`).
  `toString`, `equals`, `len` and `length` are `view fn`s.  An override is a `view fn`
  exactly when the method it overrides is one; `__init__` and a free function never are.
  The marker is part of a module's interface (method flag `0x2` in a `.pkm`), and a
  function in the AST interchange format carries it as a trailing `(qual view)`.  In a
  `view fn`, `self` and everything reached through it is read-only, as through a `view`
  parameter (`'self' is read-only in 'view fn get'; cannot assign to its field 'n'`), and
  part of `self` that is an object, string or optional (or an array or tuple holding one)
  is returned as a `view`, `-> view T`: `view fn name() -> Str { return self.name; }` is an
  error, `'self' is read-only in 'view fn name', so part of it can only be returned as a
  'view': write '-> view Str'`, while a field of a value type (or an array or tuple of
  them) is returned as a copy.  A method that never changes `self` but is not a `view fn`
  gets a warning (`'area' never changes 'self': make it a 'view fn' so 'view' parameters
  can call it`), worked out across a method's overrides and its calls on `self`; a method
  that returns part of `self` does not.  The samples and the manual mark theirs.
  `samples/codegen/48_view_fn.pkn` shows them.
- Local borrows (docs/language/02-basics.md, "Local borrows"): a local declared
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
  type, and keeps a field's object alive while it can be used.  A local borrow of a
  variable is exclusive while it is live, from its declaration to its last use in its
  block (a loop that uses it keeps it live): the variable an `inout` local names cannot be
  used (`'k' is borrowed by 'inout' local 'x' until 'x' is last used`), and one a `view`
  local reads cannot change (`'c' is viewed by 'view' local 'v' until 'v' is last used;
  'tick' is not a 'view fn'`).  A borrow of a borrow keeps the first variable borrowed.
  A local borrow needs an initializer, is never `let` and is not a destructuring target;
  written before the name (`view x = e;`) the error shows the fix.  In the AST
  interchange format a local borrow carries `(qual view)` or
  `(qual inout)`.
- A `view` of a reference stays a `view` (docs/language/03-functions.md, "A
  `view` of a reference stays a `view`"): a `view` of an object, a `Str`, an optional of
  one, or a struct, tuple or array holding one can only be passed on to a `view` parameter
  (`io::println` and the other print functions take a `view T`; the builtin methods read
  their arguments; `Str(s)` returns `s` itself and takes none).  A value type read through
  a `view` (`n: view int`, `k: view = 3`, `xs[i]` with `xs: view int[]`, a `-> view int`
  result, `self.x` with `x: int` in a `view fn`) is passed to a by-value parameter as a
  copy, as assigning it copies it.  A `view` that shares what it holds is never stored
  (assigned, destructured) or returned, and a `match` arm's name for one is a `view` too;
  an array or tuple literal is a new value, so any `view` can be one of its elements
  (`[v]`, `(s, 1)`, `return (s, 1);`).  Passing a `view` to an `inout` parameter reads
  `'v' is a 'view' parameter; it cannot be passed to 'inout' parameter 'n'`.
- `view` and `inout` parameters (docs/language/03-functions.md, "Parameter
  modes: view and inout"), on functions, methods and constructors, for parameters of any
  type (type parameters included).  The mode is written where the type goes: `fn bump(n:
  inout int)`, `fn scale(x: inout float, by: view float)`, `fn show(c: view Counter)`,
  called as `bump(k)`.  A `view` parameter is read-only, and so is what it holds: no field
  or element write and only `view fn` calls through it.  An `inout` parameter is
  its caller's storage, passed by address so
  every write reaches the caller at once: a local variable, a parameter (an `inout` one
  passes its address on) or a field of an object (`obj.f`, `self.f`, `a.b.f`; by real
  address, never copied in and out, with the object kept alive for the call), of exactly
  the parameter's type (a subclass variable is not a base-class `inout` argument).  An
  `inout` parameter of a reference type is the caller's variable, not a second
  reference: passing it neither retains nor releases, and assigning to it replaces what
  the caller's variable holds.  A literal or other expression, `self`, a `view` parameter
  or anything reached through one, a `let` local, an array element (not yet) or a
  string's character is an error.  An override keeps every parameter's mode; modes are
  part of a module's interface (a `.pkm` record's parameter modes), so calls and overrides
  in another module are checked against them.  `view` and `inout` are reserved words, a
  syntax error anywhere but before a parameter's type; written before the name (`inout n:
  int`), the error shows the fix.  In the AST interchange format a parameter carries
  `(qual view)` or `(qual inout)`.  `samples/codegen/47_inout_view.pkn` shows them.
- What a call passes to an `inout` parameter shares no storage with anything else the
  call borrows (docs/language/03-functions.md, "Exclusivity"): another
  `inout` argument, a `view` argument, or the receiver of a method that may change it or
  of a `view fn`, and a variable overlaps a field chain on it.  `both(k, k)` with
  `fn both(a: view int, b: inout int)` is `'k' is passed to 'view' parameter 'a'; it
  cannot also be passed to 'inout' parameter 'b'`, and `p.take(p.x)` is `'p.x' is part of
  'p', the receiver of 'take' (not a 'view fn'); it cannot also be passed to 'inout'
  parameter 'm'`.

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
- Each build of the compiler has a build ID, a hash of its sources (`cmake/BuildId.cmake`,
  #188), which a `.paykan_cache` module entry records (`core.build`) and which must match
  for the entry to be used, so a development build never reuses an entry another build of
  the same version wrote; `--verbose` reports `is stale (compiled by paykan <version>
  (<id>); this is paykan <version> (<id>))`.  A release build (`-DPAYKAN_RELEASE=ON`, set
  by the release workflow and the Homebrew formula) has no build ID, and a prebuilt `.pkm`
  is still judged by its version alone.
- `native fn name(params) -> T = "c_symbol";` declares a function whose body is a C
  function written against the runtime ABI (#198; docs/language/03-functions.md,
  "Native Functions"): `int`, `float`, `bool`, `char`, an enum (as its ordinal, an
  `int64_t`), `Str` and `Obj` cross by the runtime builtins' convention (`Str?` and `Obj?`
  as a result).  Only `int`, `float`, `bool`, `char` and an enum cross by value: any other
  parameter is a `view`, which C borrows, or an `inout`, which C changes in place
  (`parameter 's' must be 'view' or 'inout' ('s: view Str'); only int, float, bool, char
  and an enum cross into C by value`).  An `inout` argument is the caller's variable:
  `int64_t *`, ..., `PaykanShared **` for a `Str` or `Obj`, and for a generic instance's
  `inout T` the value's own address.  The result can be a `view` of a `view` or `inout`
  parameter.  A call in the declaring module calls the C function directly with the
  caller's own references, so it does no reference counting.  `native` is a reserved word.
  In PIR the C function is a module-less `extern fn @$c.<symbol>`; both backends declare it
  from its signature.  `paykan` does not build native code: `--object=a.o,b.o` (a
  comma-separated list, repeatable) names the objects or archives that define it, and
  every backend links them (the llvm JIT loads them).  Sample: `samples/imports/14_native`.
- `scripts/pkfmt.py` checks (`python3 scripts/pkfmt.py`) or applies (`--apply`) the layout
  of Paykan source (`.pkn`); every tracked file is formatted with it, and the Lint job
  checks it.

### Changed

- `::collections`' `List`, `Stack`, `Queue`, `HashMap`, `HashSet`, `MinPriorityQueue` and
  `MaxPriorityQueue` are structs, each holding the one object that keeps its state (the hash
  table of a `HashMap` or a `HashSet` is a private class of its own), so a copy, a parameter
  or a field still names the same container. As structs they are `ValueType`s and not
  `Obj`s: put one in a `memory::Box` where an `Obj` is needed. A container in a `let`
  variable can no longer be changed, and printing one shows the struct's fields.
  `LinkedList` stays a class.
- The documentation has one language reference: the manual's chapters and the reference's
  are merged, a chapter per feature that teaches it and then gives its precise rules
  (`docs/language/`, in reading order from getting started to the `paykan` command). The
  standard library has a section of its own, a page per module with every function and type
  (`docs/stdlib/`). Extending PaykanLang with plugins moved to the implementation overview
  (`docs/internals.md`).
- The package channels have repositories of their own (#178, #181).  Homebrew installs from
  the tap `parsabee/homebrew-paykanlang` (`brew install parsabee/paykanlang/paykanlang`, no
  tap URL), and the signed apt repository is `https://parsabee.github.io/paykanlang-apt`,
  published by that repository instead of the documentation site.  The Release workflow
  opens the tap's formula bump pull request and sends the apt repository its publish
  dispatch, with the `RELEASE_CHANNELS_TOKEN` secret.  A tap of the compiler repository
  moves with
  `brew tap --custom-remote parsabee/paykanlang https://github.com/parsabee/homebrew-paykanlang`.
- Every format is versioned by the release: PIR (binary, and the text a backend plugin
  receives), the runtime ABI, the `.pkm` container, manifest and interface, the plugin API
  and the AST interchange format all carry the release version, packed as
  `major * 10000 + minor * 100 + patch` (200 for 0.2.0) where the formats had their own
  numbers, and dotted in text (`(paykan-ast 0.2.0 ...)`, `pkm dump`, diagnostics).  A
  reader accepts exactly its own release's: a prebuilt `.pkm` or a plugin of another
  release is rejected, and a cache entry of another release is rebuilt.  0.2.0 accepts
  plugins built with 0.2.0 only.
- Printing and files moved to the standard library: the builtins `print`, `println`,
  `printerr`, `printerrln`, `open`, the classes `File` and `Error` and the identifier
  `Stdin` are gone.  A program writes `import ::io;` and `io::println(...)`,
  `io::open(...)`, `io::stdin()`; the samples, tests and docs are rewritten so.  Only `Obj`
  and `Str` remain builtin classes, and `print`, `open`, `File` and `Error` are free names
  (a program that imports `::io` still cannot declare a `File`, `Error`, `Result` or
  `ErrorCode` of its own: type names are global across an import graph).
- An if-statement chains with the new keyword `elif` instead of `else if`
  (docs/grammar.md): `if x > 0 { … } elif x == 0 { … } else { … }`.  `else if` after an
  if-statement's block is a syntax error that names `elif`; the conditional expression
  keeps `else if` (`if x > 0 then 1 else if x == 0 then 0 else -1`) and rejects `elif`.
  `elif` is reserved.  The AST is unchanged: an `elif` is an `IfStmt` nested as the else
  branch.
- `if` and `while` take their condition without parentheses (docs/grammar.md):
  `if x > 0 {`, `elif x == 0 {`, `while i < n {`; the block's `{` ends the condition.
  A parenthesised condition is still a parenthesised expression, so `if (x > 0) {`
  keeps parsing.  A statement starting with `if` is a ternary expression statement when
  `then` follows its condition, as before.  The samples, the standard library, the
  tests and the docs are rewritten without the parentheses.
- Arrays are `T[n]` (docs/language/09-arrays.md), values of a fixed length, instead of the
  dynamically sized `T[]` objects: `push`, `pop`, `toString` and `equals` on an array are
  gone, and storage that grows is `collections::List<T>` or `memory::Buffer<T>`.
  `xs: int[3] = [1, 2, 3];` holds three
  ints inline. An array is a value, so `ys = xs` copies it, and its one method is `len()`.
  A literal for a `T[n]` has exactly `n` elements. An index out of bounds is an error when
  it is a constant, and a panic (`paykan: array index 3 out of bounds (len=3)`) otherwise.
  A `let` array's elements cannot be assigned. `int[2][3]` is two `int[3]`s, and an array
  holds at most 16384 elements, counted across its rows. A parameter can be one in every
  mode (a plain one is the function's own copy), and so can a result, also across
  modules. An element can be a scalar or an optional of one (`int?[3]`), an enum, a
  reference (`Str[2]`, `Point[4]`: a copy of the array shares the elements' objects) or
  another array. For now an array is only ever a local variable, a parameter, a result or
  a field of a class (held inside its object): not a tuple element, a struct field, an
  optional, a type argument, a `match` arm or an `Obj` (`a fixed-size array ('int[2]') can
  only be a local variable, a parameter, a result or a field for now`), and it has no `==`
  and is not printed. In a variable declaration with an initializer a length can be `_`,
  the initializer's: `x: int[_][_] = [[1, 2, 3], [4, 5, 6]];` is an `int[2][3]`, and
  `z: float[_] = f();` takes the length of the array `f` returns.
- An array literal is a `T[n]` of its own length (docs/language/09-arrays.md,
  "Literals"): `[1, 2, 3]` is an `int[3]`, `[1, 2.5]` a `float[2]` and `[[1, 2], [3, 4]]`
  an `int[2][2]`, and objects of different classes give their nearest common ancestor.
  Where the literal flows into a typed slot, its elements take the slot's element type
  (`xs: float[2] = [1, 2]`). An array has at least one element: `[]` is an error (`an
  empty array literal has no type: an array has at least one element`).
- Slices (docs/language/09-arrays.md, "Slices"): `T[]` is the type of a `view` or `inout`
  parameter, a slice. It takes an array of any length whose elements are exactly `T`
  (a literal becomes one) or another slice, and reads or writes the elements in place: a
  slice is indexed, measured with `len()` and passed on. A generic `xs: view T[]` takes
  `T` from the elements. Anywhere else `T[]` is an error (`variable 'x' has type 'int[]':
  a slice can only be a 'view' or 'inout' parameter, and an array has a length
  ('int[3]')`). In PIR a slice parameter is two, the `ptr` address of the elements and
  their `i64` number.
- An object is one allocation (docs/language/16-memory.md, "The reference counts"):
  its 16-byte header (src/Runtime/Runtime.h, `PaykanObject`) is the vtable pointer, then a
  32-bit strong count and a 32-bit weak count, `{ vtable, strong count, weak count,
  fields… }`, and the separate `PaykanShared` box is gone.  A strong reference
  (`PaykanShared *`) and a weak reference (`PaykanWeak *`) are the object's own pointer:
  `PaykanShared_new` is a retain and returns the object, `PaykanShared_get` is the
  identity, and both backends lower `unbox` to the identity.  A count that would pass
  `INT32_MAX` panics (`too many strong references to one object`) instead of wrapping.
  A `destroy` (vtable slot 0) releases what the object holds and tears down its resources
  but does not free the object: the runtime frees it after the destroy, or keeps it,
  empty, until the last weak reference to it is released (`Paykan_weak_release`).
  Generated destructors do not end in `free`, `PaykanObject_destroy` is a no-op,
  `PaykanString_destroy` frees the bytes only, and a string temporary the generated code
  drops goes through the new `Paykan_dispose(obj)` (destroy and free an object that was
  never boxed).  Deferred destruction keeps its pending objects on a runtime stack (256
  inline slots, then the C heap); the destroy order is unchanged.  Every object struct,
  the runtime's and the generated ones, starts with the header; the runtime's immortal
  `None` and stdin `File` start with a strong count of 1.  The runtime ABI version is the
  release's (below): delete every `.paykan_cache` directory written by an older build.
- The program's entry point is `fn main() -> int` or `fn main(args: view Str[]) -> int`
  (docs/language/03-functions.md): the arguments are a slice, which `main`
  reads (`args[0]` is the script path, or the executable's) but does not keep.  Any other
  signature is an error (`the program's entry point must be 'fn main() -> int' or 'fn
  main(args: view Str[]) -> int', not 'fn main(int) -> int'`).  In PIR `@main` is
  `() -> i64` or `(ptr, i64) -> i64` (docs/pir.md §4): the backend's entry code makes the
  slice before it calls `@main` and drops it after, in an executable with the runtime's
  `Paykan_args_new` and `Paykan_args_free`.  A backend plugin must do the same.
- A class without an `__init__` gets the default one (docs/language/05-classes.md, "The
  default `__init__`"): it takes the superclass constructor's arguments, then one per
  field of the class, in order, so `class Point { x: int; y: int; }` is built as
  `Point(1, 2)`, and `Box(3)` infers `Box<int>` from `class Box<T> { v: T; }`.
  `Point()` no longer leaves the fields zero (a `Str` or object field was null):
  `function 'Point' expects 2 argument(s), got 0 (it has no '__init__', so it takes
  x: int, y: int)`. A subclass needs no `__init__` of its own when its superclass has
  one. An `__init__` that a subclass writes calls `__super__(...)` first unless the
  superclass's constructor is the default one of a class with no fields, as are its
  superclasses'.
- `__init__` goes in an `impl` block of its class (docs/language/05-classes.md, "`impl`
  Blocks"): `class A { fn __init__() { ... } }` is an error that says to write
  `impl A { fn __init__() { ... } }`. The class body holds the fields and the virtual
  methods.
- Assigning `self` in a class method (`self = C(1)`) is now an error (`cannot assign to
  'self'`): `self` names the object the method was called on, and the assignment changed
  nothing the caller could see.
- An override of `toString` or `equals` is declared `view fn` (`view fn toString() -> Str`),
  like the `Obj` methods it overrides: a plain `fn` override is an error.
- PIR: `mem(N, A)` slots (docs/pir.md §1 and §6), a local's or a field's N bytes
  aligned to A, for aggregates: fixed-size arrays and tuples.  A `mem` slot is never a
  value: it is reached through its address, with `ptr.add` to its parts, and copied and
  cleared with `mem.copy` and `mem.zero`.  A backend plugin must handle the three
  instructions.
- PIR: four address instructions, `local.addr`, `field.addr`, `ptr.load` and
  `ptr.store` (docs/pir.md §6), which `inout` parameters and locals lower to.  Scalar
  (`i64`, `f64`, `bool`, `char`), optional and `box` slots have addresses, and so has a
  `mem` local.  A backend plugin must handle them.
- PIR: inline optionals (#96, docs/pir.md §1 and §6).  Four value types, `opt_i64`,
  `opt_f64`, `opt_bool` and `opt_char` (a bool presence flag, then the payload at its
  alignment: 16 bytes for `opt_i64` and `opt_f64`, 2 for `opt_bool` and `opt_char`), and four
  instructions, `some`, `none`, `is_some` and `unwrap`; optional slots have addresses and
  may be fields.  A backend plugin must handle them.
- An optional is not an object (#96): a `T?` no longer converts to `Obj`, so `o: Obj = maybe`
  and passing, returning or storing a `T?` where an `Obj` is expected are errors (`cannot use
  optional 'Node?' as 'Obj': an optional is not an object; unwrap it with match`).  The print
  family takes an optional as it is and prints `None` or the value, as before.  A `view` is
  never converted: a `view T` cannot be passed to a `view T?` parameter or bind a `view T?`
  local (`'v' is a 'view' parameter; a 'view' is not converted, and parameter 1 of 'take' is a
  'view int?'`); a value that is not a `view` still widens.
- An optional primitive (`int?`, `float?`, `bool?`, `char?`) is a value, the primitive and a
  presence flag held inline (#96), instead of a heap box: a local, parameter, result, field,
  array element or tuple element of one allocates nothing and is never reference counted, and
  `int<Str>` & co. return one without allocating (the runtime's `PaykanString_parse_int` &
  co.).  The semantics are unchanged.  An `int?[3]` holds its elements inline, 16 bytes
  each, and a tuple holds an `int?` element inline at its offset.  In #96's benchmark
  (10M pushes onto a `Stack<int>` and 10M pops through `pop() -> T?`) popping through the
  `int?` allocates nothing and takes as long as popping a plain `int`.

- Tuples are values held in place (docs/language/10-tuples.md, "Tuples as Values"), like
  `T[n]`: a tuple's elements are laid out like a C struct's fields in the variable, field
  or array element that holds it, a reference element as its box and an `int?` element or
  a nested tuple inline.  Assigning, passing or returning a tuple copies it and retains
  the references it holds, which each copy releases in turn; a tuple of `int`s allocates
  nothing.  A tuple is not an `Obj`: `o: Obj = (1, 2)` is an error (`initializer of type
  '(int, int)' does not match declared type 'Obj' for variable 'o'`), and `Tuple` is no
  longer a builtin class name (`x: Tuple` is an unknown type, and a program may declare a
  class `Tuple`).  `==` / `!=`, printing and `toString()` are unchanged, and a tuple meets
  a `Show` bound; `t.equals(u)` is gone (`method call on non-class type '(int, int)'`):
  compare with `==`.  A `view` of a tuple of values is copied when it is assigned; one
  holding a reference follows the `view` rules (`'p' is a 'view' parameter; it cannot be
  stored, ...`).  Runtime: the `PaykanTuple` object (its vtable, `PaykanTuple_new`,
  `_get`, `_set`, `_set_obj`, `_destroy`) is replaced by `PaykanTuple_toString(value,
  layout)` and `PaykanTuple_equals(a, b, layout)` over a tuple layout the compiler emits
  per tuple type (`Runtime.h`, "Tuples"), and `PaykanTypeKind` gains `PAYKAN_KIND_TUPLE`:
  a generic native function's `T` that is a tuple crosses by address, its references at
  their offsets.  In PIR a tuple is `mem` storage, passed and returned by address like a
  fixed-size array, and its layout a `.tupleN` `data` global (docs/pir.md §1, §4, §7);
  the lowering emits no `bytes` globals, and a `.pkm` file's `bytes` may hold any values.

### Removed

- The `mov` keyword (#145).  Using it is now an error: `'mov' was removed in v0.2.0;
  ownership transfers are inferred` (also from the AST interchange reader for a
  `(mov ...)` node).  Drop the keyword: `t = mov s` becomes `t = s`, with the same output
  and the same objects freed; `s` simply stays readable.  Until the last-use pass (#186)
  lands, each former `mov` costs one retain/release pair.  The use-after-move,
  loop back-edge and "cannot 'mov' ..." diagnostics went with it.  `mov` stays reserved.
- The boxed primitive classes `Int`, `Float`, `Bool` and `Char` (#96), now that an optional
  primitive is a value: with them went their conversions `Int<Str>`, `Float<Str>`,
  `Bool<Str>` and `Str<Int>` & co. (use `int<Str>` & co., which return `int?` & co.), and
  the runtime's `PaykanInt` / `PaykanFloat` / `PaykanBool` / `PaykanChar` (`Basic.c`).  The
  names are no longer reserved, so a program may declare a class `Int`.

### Fixed

- A `weak` local read from a `view` (`weak h = self.head;` in a `view fn`) was an ordinary
  reference once matched, so a `view fn` could return part of `self` as a copy and the caller
  could change it through a `view`.  Its reads are `view`s now: a `match` arm's name for one
  is bound to a `view`, which cannot be changed, stored, returned or captured
  (`'n' is bound to a 'view'; it cannot be returned`).  A `-> view T` function may return
  a `match` arm's name for a `view` of `self` or of a parameter.  `LinkedList`'s `front()`
  and `back()` and `Stack` and `Queue`'s `peek()` return a `view T`.
- A `view fn` of the standard library handed out an object element as a copy, so a `view`
  of a container could change its elements: `List`'s `get` and `last`, `HashMap`'s `get`
  and `getOr` and the priority queues' `peek` return a `view` now (`view fn get(i:
  unsigned) -> view T`; `getOr` takes `otherwise: view V`), and `memory::Buffer`'s `copy`
  and `copyUnchecked` are no longer `view fn`s (a `view` of a buffer reads with `get`).
  A value element is still copied out (`n = xs.get(0);`); an object element is read or
  bound to a `view` local (`f: view = fs.get(i);`).
- `"a" + n` and `n + "a"` with a number `n` passed type checking and then failed in PIR
  verification ("arithmetic on obj"); they are a type error now, `operator '+' is not
  defined for types 'Str' and 'int' (convert the 'int' with 'Str(...)')`.
- A process that compiles two programs importing the same source module (the test
  binaries, an embedding host) no longer fails to lower the second: the module's analysis
  is cached with its exports (`Sema::ModuleContextCache`), so every compilation that
  imports it can lower it.
- A conditional expression whose branches are calls of functions returning nothing, as a
  statement (`if c then f() else g();`) in a function that returns a value, failed PIR
  verification (`control can fall off the end of a non-void function`): the rest of the
  function was emitted inside the branch.  One branch now runs for its effect.
- A top-level declaration named after a builtin type (`class int { ... }`, `struct u8`,
  `enum unsigned`, `fn float(...)`, under any of the types' spellings) was accepted and
  silently shadowed by the builtin; it is now rejected (`'u8' is a builtin type and cannot
  be redeclared`), as a declaration named after a builtin function or class is.
- `Str(s)` with a fresh string argument — a method call's result (`Str(y.toString())`), a
  conversion's or a concatenation's — failed PIR verification (the call was classified as
  not holding a box while it passed its argument's box through); the `Str` constructor now
  takes its argument's ownership classification, so the string is passed on and released
  once.
- Calling `__init__` on an object (`c.__init__(2)`) stopped the compiler in the lowering;
  it is now an error (`'__init__' cannot be called directly; it runs when a value is
  constructed ('C(...)')`).
- A generic class whose method instantiates its template with an ever-growing type
  argument (`Deep<Deep<T>>`) is now reported (`exceeds the maximum instantiation depth`)
  instead of instantiating without end, as a field's type already was.
- A literal argument of `__super__(...)` was typed on its own instead of by the superclass
  `__init__`'s parameter: `__super__([1, 2])` for a `float[2]` parameter was an error
  (`argument 1 of '__super__' has type 'int[2]', expected 'float[2]'`), as was `None` or
  `(None, 3)` for an optional or a tuple of optionals.  It now takes the parameter's type,
  as a call's argument does.
- The llvm backend's `run` (and `--emit-llvm`) optimised the program under LLVM's default data
  layout instead of the host's, which the JIT then ran it with: an object with a `char` or
  `bool` field before an `int` was allocated a few bytes short, and stores could land at the
  default layout's offsets.  The module now gets the host's layout before it is optimised.
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
  (`docs/language/10-tuples.md`, `10-optionals.md` with optional primitives
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
  ships yet, #113). See `docs/language/02-basics.md` and
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
  `docs/language/13-generics.md`.
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
  `docs/language/12-optionals.md`.
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
  `docs/language/12-optionals.md`.
- **Tuples (#4).** Fixed-arity, heterogeneous, immutable values:
  types `(int, Str)` (nesting, `(int, Str)[]` and `(int[], Str)` allowed),
  literals `(1, "a")`, compile-time-checked element access `t.0` / `t.1.0`,
  multiple return `fn f() -> (int, int)`, and destructuring
  `q, r = divmod(7, 2);` (`_` skips, `q: int, r: int = ...` annotates).
  Tuples are `Obj` subtypes under ARC (retain/release/`mov` like any
  reference value); `==` / `!=` compare element-wise and `toString` renders
  `(1, a)`. Tuple-typed signatures round-trip through module imports.
  Not yet: tuples in `match`, nested destructuring, mutation, named elements.
  See `docs/language/10-tuples.md`.

- **Move semantics with `mov`.** `mov <expr>` transfers ownership of a local
  variable, parameter, or temporary without a retain/release pair. Use of a
  moved variable is a compile error until it is re-assigned, tracked
  flow-sensitively: a `mov` in one `if`/`match` branch does not poison sibling
  branches, paths are merged conservatively after the construct, and moving a
  variable declared outside a loop without re-assigning it before the loop
  repeats is rejected. `mov self` and moving fields or array elements are
  compile errors. See `docs/language/16-memory.md`.
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
  the reference (`docs/language/10-tuples.md`, `10-optionals.md`,
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
  (`docs/language/16-memory.md`).
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

[Unreleased]: https://github.com/parsabee/PaykanLang/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/parsabee/PaykanLang/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/parsabee/PaykanLang/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/parsabee/PaykanLang/releases/tag/v0.1.0
