# The MLIR backend

The design behind the `mlir` backend: PIR is imported into an MLIR dialect,
lowered once into upstream MLIR dialects, and from there to LLVM (the JIT,
`build`, `--emit-llvm`) and to C (`--emit-c`).  It replaces the two
hand-written translations of PIR, the C emitter (`src/Backends/C`) and
`PIRToLLVM.cpp`, which each re-implement every PIR feature, with one
lowering shared by both targets.  It is built in steps, each a pull request
that keeps every test passing; the C and llvm backends stay until the `mlir`
backend matches them.

## Summary of the decisions

| Topic | Decision |
|-------|----------|
| LLVM | 22.1.8, whose prebuilt releases ship MLIR with the EmitC conversions (scf, arith, func and memref to EmitC); LLVM 17's have none |
| Input | the verified PIR program, as every backend gets it (docs/pir.md); PIR stays the interface for every backend, in tree and out |
| The `pir` dialect | PIR one to one: an op per item, statement and instruction, PIR's types (`!pir.box`, `!pir.obj`, `!pir.ptr`, `!pir.weak`, `!pir.opt<T>`, `!pir.mem<N, A>`, `!pir.local<T>`); structured regions with no terminators, as PIR's blocks are (`--emit-mlir=pir`) |
| The shared lowering | `pir` → `func` + `scf` + `arith`, and the few `pir` memory ops neither upstream path covers; one pass, used by both targets (`--emit-mlir=scf`) |
| Where the targets part | after the shared lowering, at the MLIR level: upstream conversions take `func`, `scf` and `arith` to the `llvm` dialect and to EmitC; the remaining `pir` ops get one small pattern set per target |
| LLVM | the `llvm` dialect (`--emit-mlir=llvm`), translated to LLVM IR, then the existing JIT, native object build, bitcode cache and `--emit-llvm` |
| C | EmitC, printed by MLIR's C emitter (`--emit-c`) |
| Options | `--emit-mlir=<stage>`, `pir`, `scf` or `llvm`: the program after that stage, on stdout |

## Why one lowering

Each PIR feature has had to be written twice: once in C syntax, once in
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
   (--emit-mlir=llvm)                   │
             ▼                          ▼
   LLVM IR: JIT, build,             C source (--emit-c)
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

## The shared lowering

* **Control flow.**  `pir.if` becomes `scf.if` and `pir.while` `scf.while`.
  SCF has no `break`, `continue` or early `return`, so a function that uses
  them gets an exit flag (a local): `break`, `continue` and `return` set it,
  the statements after one that may have set it run under `scf.if` on the
  flag being clear, and a loop's condition region stops the loop once it is
  set.  A `return`'s value goes into a local the function returns at its
  end.  LLVM's optimiser removes the flag again; C keeps it.
* **ARC, allocation, weak references** become calls to the runtime
  (`func.call`), as both translations already do.
* **Objects.**  Every class's layout is computed once, as C lays out the
  object (docs/pir.md §5: the 16-byte header, then the fields at their
  alignment): a field access is the object's address plus the field's
  offset, read or written through a `pir` pointer op; `new` allocates the
  computed size and zeroes it.  Vtables become `pir` globals of function
  addresses, and `vcall` a load from the vtable and an indirect call.
* **Optionals** never cross into C (docs/pir.md §1: no runtime or native
  function takes or returns one), so each becomes its two parts, the
  presence flag and the payload, as two values; in memory it keeps its C
  layout.
* **Locals** are `pir` slots: memory that LLVM's `mem2reg` turns back into
  values, and that C declares as variables.

## Steps

1. Build the llvm backend on LLVM 22.1.8 (MLIR included).
2. The `pir` dialect, the PIR import and `--emit-mlir=pir` (this document's
   first stage): every PIR program imports into MLIR that verifies and
   round-trips through MLIR's parser.
3. The shared lowering and `--emit-mlir=scf`.
4. The LLVM path: the `llvm` dialect, LLVM IR, the JIT and `build`
   (`--backend=mlir` runs programs), and `--emit-mlir=llvm`; the whole test
   suite passes on it.
5. The C path: EmitC and `--emit-c` from MLIR, and `paykan build` through
   it; the strict C11 check passes on its output.
6. The C emitter and `PIRToLLVM.cpp` are removed; MLIR is the only backend.

## Open questions

* **The C path without LLVM.**  Today the default build needs only a C
  compiler; with MLIR every build links LLVM and MLIR.  Step 6 decides how
  the default configure and the package channels follow.
* **Readable C.**  EmitC's output names every intermediate value; the C
  emitter's is closer to hand-written C.  `--emit-c` stays correct and
  strictly conforming either way.
