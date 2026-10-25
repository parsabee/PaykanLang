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
| Unreachable code | dropped by the driver before the backend sees the program: PIR's dead-code pass from the main module's `main` (docs/pir.md §12); in a module's compiled code, by linking only what the program references (see below) |
| Modules | a module's `.pkm` keeps its copies of instantiations as PIR and the rest of its code as LLVM bitcode (`compileModule`), which a program links on demand; shared copies link under names without their module |
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

A program's binary holds only what its `main` reaches.  The driver runs
PIR's dead-code pass (docs/pir.md §12) on the verified program before any
backend sees it, so no stage lowers the rest of each module it imports
(most of the standard library, for most programs): every output of a
program starts there, `run`, `build`, `--emit-llvm`, `--emit-c`,
`--emit-mlir` and `--emit-pir` alike.

What a program uses of a module's compiled code (below) is not PIR, so the
pass cannot see into it; a compiled declaration carries the names its code
uses instead, and the pass keeps those.  The code itself is linked into the
program's LLVM module with `LinkOnlyNeeded`: only the functions (and what
they reference) the program, or code linked before, refers to.  A module
is linked after every module that imports it, so what it is asked for is
known when it is linked.

## Modules compiled ahead of time

A module's `.pkm` (a cache entry, or what `--emit-pkm` writes) keeps its
code in two parts (docs/design/pkm.md §5.13): its copies of instantiations
of templates stay PIR in CODE, which every program shares and prunes, and
the rest is compiled by the backend when the file is written
(`Backend::compileModule`) into its OBJ section, which CODE declares.  This
backend compiles a module into LLVM bitcode, before optimisation (-O<n>
applies to the program it is linked into), and links only bitcode it made
itself (the same backend, format, target and LLVM; `objectKind()`).

Code compiled in one module calls copies of instantiations that a program
keeps in whichever module comes first, so a shared function or class (PIR
`shared`) links under a name without its module: `pk__S<name>` and
`pkvt__S<name>` rather than `pk_<module>_S<name>` (an escaped module name
never starts `_S`, so the two never meet).

Each module's PIR (the main module whole, an imported one its copies and
declarations) is then lowered and its LLVM IR cached as before, under the
text of what is left of it: another program's use of a module leaves
another part of it, which is cached afresh.

`--emit-c` prints the C of the program's PIR; a compiled module's code is
not C, and is linked as an object (`paykan pkm extract --object`), so for
`--emit-c` the dead-code pass also keeps everything that code uses.

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
  (`func.call`), as both of the replaced translations did.  Each target
  then has the fast path of a `retain` and a `release` inline (docs/pir.md
  §12): the LLVM path expands each call into the count's test, its
  increment or decrement in place, and the call for the rest; the C path
  calls Runtime.h's `static inline` `Paykan_retain_inline` and
  `Paykan_release_inline`, which do the same.
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
