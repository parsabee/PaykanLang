# The C output

`paykan --emit-c prog.pkn` prints the program's PIR as one C translation unit:
the C target of the `mlir` backend ([the MLIR backend](design/mlir-backend.md)),
the only backend in this repository.  The C is strictly conforming ISO C11
under the target assumptions below (see [Standard C](#standard-c)), written
against the runtime's `Runtime.h`, and builds with any C11 compiler and
`libpaykan_runtime.a`.  `paykan run` and `paykan build` do not go through it:
they take the backend's other path, through LLVM (the JIT for `run`, a native
object linked by the system C compiler for `build`).  The C is there to read
what the compiler made of a program, and to build the program with a C
compiler of your choice.

## Commands

| Command | Effect |
|---|---|
| `paykan --emit-c prog.pkn` | write the program's C to stdout and stop |
| `paykan --emit-mlir=emitc prog.pkn` | write the same unit in MLIR's EmitC dialect, as MLIR's C emitter receives it |
| `paykan --emit-mlir=scf prog.pkn` | write the program after the lowering the C and the LLVM paths share |

`--emit-c` needs the mlir backend, the default (`--emit-c` with another
`--backend` is an error), and excludes `--emit-llvm` and `--emit-mlir`.  `-O<n>`
does not change the C; the flags of the C compiler that builds it do.

The code of the modules the program imports from module files is not in the
C: a module file keeps its code other than its copies of instantiations
compiled (LLVM bitcode, [Module Files](language/15-modules.md#module-files)),
so the C declares those functions and the program links them as objects.
`paykan pkm extract --object` writes one module file's compiled code as an
object, and the C keeps everything that code uses.  To build the C:

```sh
paykan --emit-c prog.pkn > prog.c
i=0
for m in <prefix>/lib/paykan/stdlib/<version>/*.pkm $(find .paykan_cache -name '*.pkm'); do
  i=$((i + 1))
  paykan pkm extract --object "$m" -o "module$i.o"
done
ar rcs libmodules.a module*.o
cc -std=c11 -O2 -I<prefix>/include/paykan prog.c libmodules.a \
   <prefix>/lib/libpaykan_runtime.a -lm -o prog
```

(The archive lets the linker take only the modules the program has;
`paykan --version` names the standard library's directory, and
`.paykan_cache` under the program's directory holds its own imported
modules once a compile has written them.)

`<prefix>` is the installation: `/usr` for the Debian package, the unpacked
`paykanlang/` directory of a release tarball.  The executable behaves as one
made by `paykan build`: the Paykan `main`'s result is the exit status, the
command-line arguments reach `main(args: view Str[])` (with `args[0]` the
executable's path), a runtime panic flushes stdout, prints its message to
stderr and aborts the program (`SIGABRT`), and `PAYKAN_TRACK_HEAP` set in the
environment prints the heap statistics at exit, as `--track-heap` does.  A
program with `native fn`s links the objects that define them, as `--object`
does for `paykan build`.

## How it is made

The C comes out of the same pipeline as the LLVM IR, up to where the two
targets part (the design is in [the MLIR backend](design/mlir-backend.md)):

1. PIR is imported into the `pir` MLIR dialect, and the shared lowering
   (`src/Backends/MLIR/Lowering/PIRToStandard.cpp`) takes it to `func`, `scf`
   and `arith`, plus the few `pir` memory ops neither upstream path covers.
   Everything specific to PaykanLang is decided there, once for both
   targets: ARC as runtime calls, every class's layout and field offsets, the
   vtables, the optionals, the exit flags that give `scf` its `break`,
   `continue` and early `return`, and every symbol's link name.
2. Canonicalisation drops the values and call results nothing reads (C would
   warn about the unused variable).
3. One conversion (`src/Backends/MLIR/ToEmitC/ToEmitC.cpp`) takes `func`,
   `scf` and `arith` to EmitC with MLIR's upstream patterns, and the remaining
   `pir` ops with patterns of its own, the only part of the backend written
   for C alone.  It also adds what surrounds them in the translation unit:
   the includes, the optionals' structs, the indirect-call helpers and C's
   `main`.
4. MLIR's C emitter prints the unit, with an empty parameter list spelled
   `(void)`, C11's prototype, in place of the emitter's `()` (an old-style
   declaration in C).

## The generated C

* **The unit.**  A comment naming the generator; `<math.h>`, `<stdbool.h>`,
  `<stdint.h>`, `<stdlib.h>`, `<string.h>` and `"Runtime.h"`; the optionals'
  structs and the `pkicall_<n>` helpers; then each PIR module of the program
  in turn, main module first: a declaration of every function it defines (a
  body or a vtable may use one defined below it), `extern` declarations of
  what it uses from other modules and of its native functions, its constants
  and vtables, and its functions.  C's `main` ends the main module's part.
* **Native functions.**  A native function is declared from its PIR
  signature (`extern int64_t llabs(int64_t);`), which may not be what a
  header says of the symbol: `native fn llabs(x: int) -> int = "llabs"`
  against `<stdlib.h>`'s `long long llabs(long long)`, a native `exit`
  taking an `int64_t`.  So each native function's symbol is renamed while
  the headers are included, `#define llabs pk__hidden_llabs` before them and
  `#undef llabs` after, and the header's declaration is of another name.
  The `#undef` also drops a macro a header defines for the symbol (`isnan`
  in `<math.h>`, or `memcpy` in macOS's `<string.h>` when it fortifies), so
  the C calls the native function.  The compilers know the C library's
  functions and warn of a declaration of one with other types (GCC's
  `-Wbuiltin-declaration-mismatch`, Clang's
  `-Wincompatible-library-redeclaration`): a unit with native functions
  turns that warning off after the includes.  A native function bound to a
  function `Runtime.h` declares (the standard library's `Paykan_io_write`,
  the Buffer functions of a `List`'s instance) is declared the same way; one
  of the functions compiled code calls has their signature ([PIR](pir.md)
  §4).
* **Types** are spelled as `Runtime.h` spells them: an `i64` is `int64_t`, an
  `f64` `double`, a `bool` `bool` and a `char` `int8_t`; a box is
  `PaykanShared *`, an obj `PaykanObject *`, a weak reference `PaykanWeak *`
  and a ptr `void *`.  An optional is the struct of its presence flag and its
  payload, `typedef struct { bool present; <payload> value; }
  pkrt_opt_<payload>;` (`pkrt_opt_i64`, `_f64`, `_bool`, `_char`), passed and
  returned by value.
* **The ownership mask.**  A function that takes a box, other than a
  runtime or native one, takes the mask after its parameters, a `uint64_t`
  as `Runtime.h`'s `owned` is ([PIR](pir.md) §3): `void pk_m_Sshow(PaykanShared*
  v1, uint64_t v2)`.  So does every vtable slot and function value whose
  type takes a box, the runtime's `equals` included.  A call passes the
  mask its arguments' ownership makes, a constant (`pk_m_Sshow(v19,
  UINT64_C(1))`) or the bits a function hands on, `v5 & UINT64_C(2)`
  tested and `v8 ? UINT64_C(1) : UINT64_C(0)` passed; an unused one is
  `(void)` like any parameter.
* **Names.**  Every symbol a module defines has the link name the shared
  lowering gives it, the same in the C and in the LLVM IR: `pk_<module>_S<name>`,
  and `pkvt_<module>_S<class>` for a class's vtable; a copy of an
  instantiation (`Box<int>.get`, shared by every module, [PIR](pir.md) §3)
  has none of its module: `pk__SBox_3Cint_3E_2Eget`.  In both parts letters
  and digits stay, `_` is doubled and every other byte becomes `_XX`, its
  hexadecimal code, so the result is a C identifier, no two symbols share
  one, and none spells a C keyword, a C library name or a runtime symbol: a
  program's `fn PaykanString_new` in module `m` is `pk_m_SPaykanString__new`,
  and the method `Dog.sound` of module `zoo` is `pk_zoo_SDog_2Esound`.
  Runtime and native functions keep their C names.  The program's `main` is
  the `static` function `paykan_main`, and values are `v<n>`, numbered per
  function.
* **Objects** have no C struct of their own.  The shared lowering computes
  each class's layout as C lays out such a struct (the 16-byte header, then
  every field at its alignment, ancestors first; [PIR](pir.md) §5), and a
  field is read or written through the object's address plus the field's
  byte offset, `*(T *)((uint8_t *)obj + offset)`.
* **Vtables** are arrays of `Runtime.h`'s `PaykanMethod`, as the runtime's
  own are: `PaykanMethod pkvt_zoo_SDog[4] = {(PaykanMethod)pk_zoo_SDog_2Edestroy,
  (PaykanMethod)PaykanObject_toString, ...};`, with `NULL` in an empty
  (`null`) slot, which the lowering never emits (a pure virtual method's
  slot holds its trap function, [PIR](pir.md) §5); another module's vtable
  is declared `extern PaykanMethod pkvt_...[];`.  A virtual call reads the object's vtable pointer as a
  `PaykanMethod *` and the slot as a `PaykanMethod`, so every vtable is read
  through the type it is declared with (#62), and converts the slot through
  `uintptr_t` to the `void *` an indirect call takes.
* **Function values and indirect calls.**  A function's address is held as a
  `void *` through `uintptr_t`, `(void *)(uintptr_t)f`, since ISO C converts
  a function pointer to an object pointer in neither direction.  A call
  through one goes through a helper per signature, defined once at the top
  of the unit, which converts it back to the function's type:

  ```c
  static int64_t pkicall_0(void *fn, PaykanObject * a0, int64_t a1) { return ((int64_t (*)(PaykanObject *, int64_t))(uintptr_t)fn)(a0, a1); }
  ```

  The mask's type is the runtime's, so a virtual call of an `equals` the
  runtime implements, and the runtime's of one a program overrides, call the
  function through its own type: `int64_t (*)(PaykanObject *, PaykanShared *,
  uint64_t)`.

* **Locals.**  Each local slot of the shared lowering (a PIR `local`, an exit
  flag, a function's result) is a zeroed array of the unsigned integer of the
  slot's alignment, `uint64_t v3[1] = {0};` for an 8-byte slot, which gives
  C the slot's alignment; it is read and written through a pointer of the
  value's type.
* **Calls.**  A runtime function is called through `Runtime.h`'s prototype,
  which the unit never repeats: its pointer arguments are passed as `void *`
  and its pointer result is cast, so `Runtime.h`'s more precise pointer types
  need no table in the compiler.  A `retain` or `release` calls `Runtime.h`'s
  `static inline` fast path, `Paykan_retain_inline` or
  `Paykan_release_inline`: the object's count is incremented, or decremented
  while it stays above zero, in place, and `Paykan_retain` or
  `Paykan_release` is called only at the limit or to destroy the object.
  Native functions and other modules' functions are declared `extern` from
  their PIR signatures.
* **Arithmetic.**  Integer arithmetic is done in `uint64_t` and converted
  back, so it never overflows a signed type; division by zero and
  `INT64_MIN / -1` are guarded by the lowering before they reach C, and
  `INT64_MIN` is written as the macro.  A float comparison is C's operator,
  `!=` unordered and the others ordered, as `docs/pir.md` defines `cmp`; a
  `cast` between `f64` and `i64` reinterprets the bits with `memcpy`.
* **Constants.**  Strings and data are `static const` arrays
  (`static const char pk_hello_S_2Estr0[3] = "hi";`), an empty one `{0}` of
  one element; a `?` in a string is escaped, so no trigraph forms.
* **Control flow** is structured, as PIR is.  An `if` is C's `if`; a `while`
  is a `do { ... } while (cond);` whose body evaluates the condition and runs
  the loop's body under an `if` on it.  `break`, `continue` and an early
  `return` set the function's exit flag; the statements after one that may
  have set it run under an `if` on the flag being clear, a loop stops once it
  is set, and the function returns its result from a local at its end.
* **C's `main`** sets the runtime up as `paykan run` does: with
  `PAYKAN_TRACK_HEAP` set, the tracking allocator before the program and the
  heap statistics after it; the command-line arguments as the Paykan `main`'s
  `args` slice (`Paykan_args_new`, which also honours `PAYKAN_NO_ARGS`, and
  `Paykan_args_free` after it returns), or `(void)argc; (void)argv;` when it
  takes none; and the Paykan `main`'s result as the exit status.
* **Nothing unused.**  Values nothing reads are dropped before the
  conversion, a constant nothing reads is not emitted, a call result nothing
  reads is not bound, and a parameter a function does not read is consumed
  with `(void)p;`, so a `-Wall -Wextra` build finds no unused variable,
  parameter or static.

The C does not read like hand-written C: EmitC names every intermediate value
and spells every conversion as a cast of its own.  The output is
deterministic for a given program.

## Standard C

Everything `--emit-c` prints is strictly conforming ISO C11 under the target
assumptions below: no compiler extensions (statement expressions,
`__attribute__`, `__builtin_*`, `typeof`, case ranges, labels as values,
inline assembly), no VLAs, no zero-length arrays or empty initializers, no
conversion between function and object pointers except through `uintptr_t`,
no type punning except through `memcpy`, and no reserved identifiers (every
generated name starts with a lowercase prefix: `pk_`, `pkvt_`, `pkicall_`,
`pkrt_`, `paykan_main`, `v<n>`).

The `CStrictC11` ctest (`scripts/c_strict.py`) enforces this over the runnable
programs of the samples corpus (`samples/codegen`, `samples/leak-check` and
`samples/imports/*/main.pkn`): with every C compiler it finds (GCC and Clang),
each program's `--emit-c` output and the runtime (`src/Runtime/*.c`) are
compiled with `-std=c11 -pedantic-errors -Wall -Wextra -Werror` at `-O0` and
`-O2`, linked and run, and every executable must print the same stdout and
stderr, exit with the same status and leave the same live heap blocks (none)
as `paykan run --track-heap`.  The *Strict C11* CI job runs it with both GCC
and Clang required.  MSVC (`clang-cl`) and TinyCC are a goal, not yet checked:
no CI runner provides them.

### Target assumptions

The generated code and the runtime rely on these implementation-defined
properties.  `Runtime.h`, which the C includes, checks each it can with
`_Static_assert` (or `#error`), so a target that breaks one fails to compile
instead of miscompiling.  The LLVM IR the backend runs and builds relies on
the same properties.

| Assumption | Why | Check |
|---|---|---|
| `CHAR_BIT == 8` | byte offsets into objects and `mem` storage (arrays, tuples), tuple layouts, string data, `int8_t` chars | `_Static_assert` |
| LP64: 8-byte pointers, `intptr_t` and `unsigned long` | a reference in an object, an array, a tuple or a `PaykanType` value is an 8-byte box pointer at the offset the shared lowering computed | `_Static_assert` on the sizes; `#error` without `intptr_t` |
| A pointer converted to `intptr_t` / `int64_t` and back is the same pointer (C11 6.3.2.3p5-6 leave it implementation-defined) | PIR's `cast`s between `i64` and `box`, `obj` or `ptr` | documented; true of every LP64 ABI |
| Function pointers are pointer-sized, and one converted to `uintptr_t` and back (6.3.2.3p5-6 again) is the same function | vtable slots are 8 bytes; a function value and a vtable slot are held as a `void *` through `uintptr_t` until a `pkicall_<n>` helper calls them | `_Static_assert` on the size; the conversion is documented, true of every LP64 ABI |
| Two's complement, and converting an out-of-range value to a signed type wraps modulo 2^64 (6.3.1.3p3 leaves it implementation-defined) | `(int64_t)((uint64_t)a + (uint64_t)b)` and the other wrapping operations | `_Static_assert((int64_t)UINT64_MAX == -1 && ...)` |
| IEEE 754 binary64 `double`, with NaN and infinities | float semantics, `cast` bit patterns, `NAN` / `INFINITY` | `_Static_assert` on `<float.h>` (`FLT_RADIX`, `DBL_MANT_DIG`, `DBL_MAX_EXP`, `DBL_MIN_EXP`) |
| Every Paykan object, generated or the runtime's, starts with the same 16-byte header, so a pointer to it can be converted to and used as a `PaykanObject *` | the runtime's ARC and method calls see every object as a `PaykanObject`; the generated code reads an object's vtable pointer as the `PaykanMethod *` the header starts with, and its inline retains and releases change the 32-bit `strong` count after it | `_Static_assert` on the runtime structs' header offsets |

`__STDC_IEC_559__` (full Annex F conformance) is deliberately not required:
GCC and Clang on glibc define it, Apple's clang does not, and the generated
code relies only on the binary64 format, which `<float.h>` pins down.
