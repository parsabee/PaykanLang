# The MLIR backend

The design behind the `mlir` backend: PIR is imported into an MLIR dialect,
lowered once into upstream MLIR dialects, and from there to LLVM (the JIT,
`build`, `--emit-llvm`) and to C (`--emit-c`).  It replaced the two
hand-written translations of PIR, the C emitter (`src/Backends/C`, the `c`
backend) and `PIRToLLVM.cpp` (the `llvm` backend), which each re-implemented
every PIR feature, with one lowering shared by both targets.  It was built in
steps, each a pull request that kept every test passing; the last one (step 6
below) removed the `c` and `llvm` backends.  `mlir` is now the only backend in
this repository, always built and the default; other backends are plugins
([writing a backend](../writing-a-backend.md)).

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| LLVM | 22.1.8, whose prebuilt releases ship MLIR with the EmitC conversions (scf, arith, func and memref to EmitC); LLVM 17's have none |
| Input | the verified PIR program, as every backend gets it (docs/pir.md); PIR stays the interface for every backend, in tree and out |
| The `pir` dialect | PIR one to one: an op per item, statement and instruction, PIR's types (`!pir.box`, `!pir.obj`, `!pir.ptr`, `!pir.weak`, `!pir.opt<T>`, `!pir.mem<N, A>`, `!pir.local<T>`); structured regions with no terminators, as PIR's blocks are (`--emit-mlir=pir`) |
| Unreachable code | dropped from the imported program before anything is lowered: MLIR's symbol DCE, run until nothing changes, from the main module's `main` (see below); a `.pkm` keeps all its module's code |
| The shared lowering | `pir` → `func` + `scf` + `arith`, and the few `pir` memory ops neither upstream path covers; one pass, used by both targets (`--emit-mlir=scf`) |
| Where the targets part | after the shared lowering, at the MLIR level: upstream conversions take `func`, `scf` and `arith` to the `llvm` dialect and to EmitC; the remaining `pir` ops get one small pattern set per target |
| LLVM | the `llvm` dialect (`--emit-mlir=llvm`), translated to LLVM IR, then the existing JIT, native object build, bitcode cache and `--emit-llvm` |
| C | EmitC, printed by MLIR's C emitter (`--emit-c`) |
| Options | `--emit-mlir=<stage>`, `pir`, `scf`, `llvm` or `emitc`: the program after that stage, on stdout |
| Backends | `mlir` alone, always built and the default; it replaced `c` and `llvm`, whose `PAYKAN_BACKENDS` names now build it, with a warning (step 6) |

## Why one lowering

Each PIR feature had to be written twice: once in C syntax, once in
LLVM IR.  An object field is a struct member in C and a GEP in LLVM; a
`while` with `break` is a C loop and a set of basic blocks.  MLIR's upstream
dialects already have both translations for everything that is not specific
to PaykanLang: control flow (`scf`), arithmetic and comparisons (`arith`),
functions and calls (`func`).  So the backend lowers PIR into those once,
and lets MLIR take them to either target:

```
PIR ──import──▶ pir dialect                         --emit-mlir=pir
                   │  shared lowering (one pass)
                   ▼
        func + scf + arith + pir memory ops          --emit-mlir=scf
             │                          │
   upstream conversions          upstream conversions
   + pir→llvm patterns           + pir→emitc patterns
             ▼                          ▼
        llvm dialect                 emitc
   (--emit-mlir=llvm)         (--emit-mlir=emitc)
             ▼                          ▼
   LLVM IR: JIT, build,      C source (--emit-c)
   --emit-llvm
```

The rule for each piece of the lowering: if an upstream path from `scf` (or
`func`, `arith`) to EmitC exists, the shared lowering produces that dialect
and both targets use upstream conversions; else, if one exists from the
`llvm` dialect to EmitC, that; else the piece stays a `pir` op and each
target gets a pattern for it.  No upstream path leads from the `llvm`
dialect to EmitC, so the last case covers what no upstream dialect lowers to
both: raw pointers and memory (a local slot's address, a load or store
through a pointer, a pointer plus a byte offset, a global's address, a call
through a function pointer).  Those few ops are all that is written per
target; everything PaykanLang-specific (ARC, objects and their layouts,
vtables, optionals, the sized integer types, structured control flow with
`break`, `continue` and early `return`) is lowered once.

## Unreachable code

A program's binary holds only what its `main` reaches: the rest of each
module it imports (most of the standard library, for most programs) is
dropped right after the import (`dropUnreachable`,
`src/Backends/MLIR/Lowering/DropUnreachable.cpp`), so no stage lowers it.
Every output of a program starts there: `run`, `build`, `--emit-llvm`,
`--emit-c` and `--emit-mlir`.  `--emit-pir` and `.pkm` files hold all of
each module's code, since an importer may use any of it.

MLIR's symbol DCE does the work: it erases each private symbol (function,
declaration, global) that no live one refers to.  Two kinds of reference
are not symbol references in the dialect, and the driver gives them to it
between runs, until a run erases nothing:

* A declaration names another module's function by its `module` and
  `link_symbol` attributes.  A definition is public, and so live, while a
  declaration of it elsewhere lives; the main module's `main` is the other
  public symbol.  A declaration that dies takes its definition with it at
  the next run.
* A class is named by a string (a class and its constructor share a
  name), and a class op is never erased by symbol DCE.  A class no op names
  any more (`new`, `vcall`, a field access, `vtable_addr`, or a live
  class's superclass) is erased, and its vtable's functions die with it at
  the next run.  Class names are unique across a program, so a class is
  live wherever a name of it is used.

Each module is then lowered and its LLVM IR cached as before, under the
text of what is left of it: another program's use of a module leaves
another part of it, which is cached afresh.

## The shared lowering

* **Control flow.**  `pir.if` becomes `scf.if` and `pir.while` `scf.while`.
  SCF has no `break`, `continue` or early `return`, so a function that uses
  them gets an exit flag (a local): `break`, `continue` and `return` set it,
  the statements after one that may have set it run under `scf.if` on the
  flag being clear, and a loop's condition region stops the loop once it is
  set.  A `return`'s value goes into a local the function returns at its
  end.  LLVM's optimiser removes the flag again; C keeps it.
* **Switches.**  A PIR `switch` (a `match` on an enum, or on integer,
  `char` or `bool` literals) becomes an `scf.index_switch` on its value as
  an index (a `char` or `bool` zero-extended, as its cases are).  Upstream
  conversions make it a C `switch` (`emitc.switch`); the LLVM path makes it
  an LLVM `switch` on the whole 64-bit value itself, as upstream's switches
  on an `i32`.  A case that returns, breaks or continues is lowered as an
  `if`'s branch would be.
* **ARC, allocation, weak references** become calls to the runtime
  (`func.call`), as both of the replaced translations did.
* **Objects.**  Every class's layout is computed once, as C lays out the
  object (docs/pir.md §5: the 16-byte header, then the fields at their
  alignment): a field access is the object's address plus the field's
  offset, read or written through a `pir` pointer op; `new` allocates the
  computed size and zeroes it.  Vtables become `pir` globals of function
  addresses, and `vcall` a load from the vtable and an indirect call.
* **Optionals** stay `!pir.opt` values, with `pir.some`, `pir.none`,
  `pir.is_some` and `pir.unwrap`: each target makes them the struct C lays
  out (`{ bool present; <payload> value; }`), passed and returned by value as
  the `c` and `llvm` backends did.  (Their two parts as two values would need
  two results from a function returning one, which EmitC, like C, cannot
  express.)
* **Locals** are `pir.alloca` slots: memory that LLVM's `mem2reg` turns
  back into values, and that C declares as variables.
* **Names.**  Every symbol gets its link name here, the same for both
  targets and in every module that references it: `pk_<module>_S<name>`
  (a vtable `pkvt_...`), both parts escaped so the result is a C identifier
  and no two symbols share one; runtime and native functions keep their C
  names.

## The C path

Upstream conversions take `func`, `scf` and `arith` to EmitC
(src/Backends/MLIR/ToEmitC); a pattern each takes the `pir` ops the shared
lowering keeps; MLIR's C emitter prints the result, the whole program as one
translation unit (`--emit-c`; [the C output](../c-backend.md) describes it).
`paykan run` and `paykan build` take the LLVM path.  (Until step 6 an `mlir-c`
backend also built and ran that C with the system compiler, one cached object
per module, as the `c` backend built its own.)  What is C's alone is decided
there, at the MLIR level:

* **Types** are spelled as Runtime.h spells them: a box `PaykanShared *`, an
  obj `PaykanObject *`, a weak `PaykanWeak *`, a ptr `void *` (what a native
  function's prototype in Runtime.h uses), an optional the struct
  `pkrt_opt_<payload>`.
* **The runtime** is Runtime.h's.  The shared lowering marks what the runtime
  defines (`pir.runtime`); the C path declares none of it, and calls a
  runtime function with its pointer arguments as `void *` and its pointer
  result cast, so Runtime.h's more precise pointer types need no table here.
  Natives and other modules' functions are declared from their signatures.
* **Function pointers** are held as `void *` through `uintptr_t` (ISO C
  converts neither to the other directly), called through a helper per
  signature, and vtables are `PaykanMethod` arrays, as Runtime.h's own.
* **Locals** are zeroed arrays of the slot alignment's integer, which gives
  C their alignment.
* **Strictly conforming C11**, which a `-std=c11 -pedantic-errors -Wall
  -Wextra -Werror` build accepts (the CStrictC11 test): the program's `main`
  becomes `paykan_main` behind C's `main()`, which sets the runtime up as
  `run` does; unread values are dropped (canonicalisation) and unread
  parameters marked `(void)`, so C warns about no unused variable; and an
  empty parameter list is printed `(void)`, not the C++ `()` of MLIR's
  emitter.

## Steps

1. Build the llvm backend on LLVM 22.1.8 (MLIR included).
2. The `pir` dialect, the PIR import and `--emit-mlir=pir` (this document's
   first stage): every PIR program imports into MLIR that verifies and
   round-trips through MLIR's parser.
3. The shared lowering and `--emit-mlir=scf`.
4. The LLVM path: the `llvm` dialect, LLVM IR, the JIT and `build`
   (`--backend=mlir` runs programs), and `--emit-mlir=llvm`; the whole test
   suite passes on it.  The JIT, the bitcode cache and native builds move
   to a library both LLVM backends share (src/Backends/LLVM/CodeGen).
5. The C path: EmitC and `--emit-c` from MLIR (`--backend=mlir-c`), and
   `paykan build` through it; the strict C11 check passes on its output.
   Building C with the system compiler moves to a library both C backends
   share (paykan/backends/c/CBuild.h).
6. The C emitter and `PIRToLLVM.cpp` are removed; MLIR is the only backend.
   Done: the `c` and `llvm` backends (src/Backends/C,
   include/paykan/backends/c, `LLVMBackend.cpp`, `PIRToLLVM.cpp`) and the
   `paykan_c_build` library are gone, and `mlir-c` became the `mlir`
   backend's `--emit-c`.  `mlir` is always built and the default
   (`paykan --list-backends` prints `mlir (default)`); `--emit-llvm`,
   `--emit-c` and `--emit-mlir=<stage>` are its source outputs, one at a
   time.  src/Backends/LLVM keeps its JIT and its native code generation
   (src/Backends/LLVM/JIT and src/Backends/LLVM/CodeGen), and the bitcode
   cache (`<module>.bc`) is the only backend cache.

## Open questions

* **The C path without LLVM.**  Before step 6 the default build needed
  only a C++ compiler and, at run time, a C compiler; with MLIR every build
  links LLVM and MLIR.  Decided in step 6: a default configure uses the LLVM
  22.1.8 that `-DLLVM_DIR` names, or downloads the prebuilt release (MLIR
  included) once into `build/third-party/llvm`; the release tarballs and the
  Debian package link it statically, and Homebrew installs those prebuilt
  tarballs instead of building from source.
* **Readable C.**  EmitC's output names every intermediate value; the
  removed C emitter's was closer to hand-written C.  `--emit-c` stays
  correct and strictly conforming either way.
