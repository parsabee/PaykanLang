# The C backend

The C backend (`--backend=c`) translates a verified PIR program
(`docs/pir.md`) into one strictly conforming ISO C11 translation unit
(see [Standard C](#standard-c)) against the runtime's `Runtime.h`, and builds or runs it with the system C compiler.  It is part of
the core: it depends on nothing but the standard library and a C compiler at
run time, and it is the default backend of a build without the LLVM plugin.

## Commands

| Command | Effect |
|---|---|
| `paykan --backend=c --emit-c prog.pkn` | write the C to stdout and stop |
| `paykan --backend=c build prog.pkn [-o prog]` | emit, compile and link an executable (default name: the source's stem) |
| `paykan --backend=c run prog.pkn [args…]` / `paykan --backend=c prog.pkn [args…]` | build into a temporary directory, run, forward the exit code |
| `--track-heap` | the program runs with the runtime's tracking allocator and prints the heap statistics to stderr at exit (`live blocks : 0` for a leak-free program) |
| `-O<n>` | passed to the C compiler as `-O<n>` |

The C compiler is `$CC`, then `cc` (in a `PAYKAN_COVERAGE` build, the C
compiler that built the runtime, with `-isysroot` of the build's SDK on
macOS).  The runtime (`libpaykan_runtime.a` and
`Runtime.h`) is found in this order: the build tree the compiler was built in,
`$PAYKAN_RUNTIME_DIR/{lib,include/paykan}`, the install layout next to the
executable (`../lib`, `../include/paykan`), and the install location
configured at build time.  With the build tree's runtime, programs are
compiled and linked with the build's sanitizer and coverage flags, since the
archive is instrumented with them.

A program started with `paykan run` sees the script path as `args[0]`
(`fn main(args: Str[])`), like the JIT backend.  A runtime panic flushes
stdout (and every open `File`), prints its message to stderr and aborts the
program (`SIGABRT`); `paykan run` then exits with 128 + the signal number,
134, which is also what the llvm backend's `paykan run` exits with (its JIT
runs the program in the compiler's own process and turns the abort into that
exit status).  An executable from `build` dies by `SIGABRT` itself, on both
backends.

## The generated C

* One `struct` per class with the runtime's two-word object header (vtable
  pointer, unique-box backpointer) followed by every field, ancestors first.
  Class, function and global names are mangled as `pk_<module>_<name>` (any
  character outside `[A-Za-z0-9_]` becomes `_XX`, and a clash gets a `_<n>`
  suffix), so modules never collide and generics instantiations
  (`Box<int>`) and methods (`K.w` is `pk_<module>_K_2Ew`, distinct from a
  user function `K_2Ew`) are valid identifiers.  Runtime symbols keep their
  C names.
* One vtable per class (`pkvt_pk_<module>_<Class>`), whose address is the
  class's runtime type identity (`match`).  Every vtable, the runtime's
  included, is an array of `PaykanMethod` (`void (*)(void)`, `Runtime.h`):
  each slot holds a method converted to that type, and a virtual call reads
  the slot (`Paykan_vtable_of(obj)[slot]`, with the slot number from PIR)
  and converts it back to the method's own type before calling it, a round
  trip C11 6.3.2.3p8 defines.
* Names from the program never reach C unprefixed, so they cannot clash with
  C keywords, the C library (`errno`, `stdout`, `fmod`, `int64_t`, ...) or
  generated names: fields are `f_<name>`, PIR locals `l_<name>` (`l<k>_<name>`
  for the k-th shadowing local of the same name), and PIR values
  `v<id>_<name>`.
* Every PIR value is a `const`-free C local declared where it is defined;
  PIR locals are C variables declared at the top of the function.
  Structured PIR maps one-to-one: `if`/`else`, `for (;;)` with the condition
  region at the top of the loop (`continue` re-enters it), `break`,
  `return`.
* Integer arithmetic wraps (it is done in `uint64_t`), float comparisons
  are C's own operators (`!=` unordered, true for a NaN operand; the rest
  ordered, as `docs/pir.md` defines `cmp`), and `cast` reinterprets bits
  through `memcpy`, so the generated code has the same semantics as the
  LLVM backend.
* ARC, allocation and freeing are direct runtime calls (`Paykan_retain`,
  `Paykan_release`, `PaykanShared_new`, `PaykanShared_get`, `Paykan_malloc`,
  `Paykan_free`); every other runtime call is cast to the prototype in
  `Runtime.h`.
* `main(int argc, char **argv)` builds the `Str[]` argument array, honours
  `PAYKAN_TRACK_HEAP` (set by `--track-heap`) and returns the Paykan `main`'s
  result.
* Nothing unused is emitted: the bit-cast helpers, allocators (`pknew_*`)
  and string / data globals only when referenced, no local that nothing
  reads, and no binding for a call result nothing reads; an unread
  parameter is consumed with `(void)`.  An `if` on a literal condition is
  emitted as just the branch taken.

`--emit-c` prints the whole program as one file.  `build` and `run` instead
emit one translation unit per PIR module (a module declares what it imports
as `extern` items, so each unit is self-contained), compile each into
`<project root>/.paykan_cache/<module>.o`, reuse the object while the
module's generated C and its cache key (the C compiler and flags, a hash of
`Runtime.h` and the paykan version) are unchanged, and link the
objects with `libpaykan_runtime.a`.  Cache files are written to a temporary
name and renamed into place, the object before its `.key`, so concurrent
builds that share an import never see a partial entry.  The `.key` also
records the object's size and hash, checked on every reuse: an object
truncated or corrupted after it was cached is rebuilt, not linked.  The output is
deterministic for a given program.  The C compiler runs with `-std=c11` and
the include path only: no `-w`, since the generated code compiles without
warnings (any warning is a backend bug).

## Standard C

Everything `--emit-c`, `build` and `run` generate is strictly conforming ISO
C11 under the target assumptions below: no compiler extensions (statement
expressions, `__attribute__`, `__builtin_*`, `typeof`, case ranges, labels as
values, inline assembly), no VLAs, no zero-length arrays or empty
initializers, no conversion between function and object pointers, no type
punning except through `memcpy`, and no reserved identifiers (every emitted
name starts with a lowercase prefix: `pk_`, `pkvt_`, `pknew_`, `pkrt_`,
`f_`, `l_`, `v<id>`).  Integer arithmetic is done in `uint64_t`, so it never
overflows a signed type (negation is `0 - (uint64_t)x`; PIR has no shifts),
and division by zero and `INT64_MIN / -1` are guarded by the lowering before
they reach C.

The `CStrictC11` ctest (`scripts/c_strict.py`) enforces this over the whole
samples corpus (`samples/codegen`, `samples/leak-check`,
`samples/imports/*/main.pkn` and `example_program/`): with every C compiler
it finds (GCC and Clang), each program's `--emit-c` output and the runtime
are compiled with `-std=c11 -pedantic-errors -Wall -Wextra -Werror` at `-O0`
and `-O2`, linked and run, and `paykan build` is run with those flags added
to `$CC` (which also covers the per-module units); every executable must
print the same stdout and stderr, exit with the same status and leave the
same live heap blocks (none) as `paykan --backend=c run`.  The *Strict C11*
CI job runs it with both GCC and Clang required.  MSVC (`clang-cl`) and
TinyCC are a goal, not yet checked: no CI runner provides them.

### Target assumptions

The generated code and the runtime rely on these implementation-defined
properties.  `Runtime.h`, which every generated unit includes, checks each
with `_Static_assert` (or `#error`), so a target that breaks one fails to
compile instead of miscompiling.

| Assumption | Why | Check |
|---|---|---|
| `CHAR_BIT == 8` | tuple kind bytes, string data, `int8_t` chars | `_Static_assert` |
| LP64: 8-byte pointers, `intptr_t` and `unsigned long` | every array and tuple slot is 8 bytes and holds an `int64_t`, a `double` or a pointer; array lengths are `unsigned long` | `_Static_assert` on the sizes; `#error` without `intptr_t` |
| A pointer converted to `intptr_t` / `int64_t` and back is the same pointer (C11 6.3.2.3p5-6 leave it implementation-defined) | pointers stored in integer slots (`(PaykanShared *)(intptr_t)slot`) | documented; true of every LP64 ABI |
| Function pointers are pointer-sized | vtable slots are 8 bytes, matching the LLVM backend's vtables | `_Static_assert` |
| Two's complement, and converting an out-of-range value to a signed type wraps modulo 2^64 (6.3.1.3p3 leaves it implementation-defined) | `(int64_t)((uint64_t)a + (uint64_t)b)` and the other wrapping operations | `_Static_assert((int64_t)UINT64_MAX == -1 && ...)` |
| IEEE 754 binary64 `double`, with NaN and infinities | float semantics, `cast` bit patterns, `NAN` / `INFINITY` literals | `_Static_assert` on `<float.h>` (`FLT_RADIX`, `DBL_MANT_DIG`, `DBL_MAX_EXP`, `DBL_MIN_EXP`) |
| An object struct can be used through a `PaykanObject *` (and a pointer to it converted to and from `PaykanObject *`): every Paykan object struct, generated or in the runtime, starts with the same two-word header | the runtime's ARC and method calls see every object as a `PaykanObject`; generated code casts between `struct pk_…`, `PaykanString`, `PaykanArray`, … and `PaykanObject *` | `_Static_assert` on the runtime structs' header offsets; the generated code reads vtables only through `memcpy` (`Paykan_vtable_of`), never through another struct type |

`__STDC_IEC_559__` (full Annex F conformance) is deliberately not required:
GCC and Clang on glibc define it, Apple's clang does not, and the generated
code relies only on the binary64 format, which `<float.h>` pins down.

## Parity with the LLVM backend

Both backends consume the same PIR semantics (the ownership rules live in the
lowering, `src/Lowering`), so every CodeGen test and every sample must produce
identical stdout, exit codes and zero live heap blocks under `--track-heap`
on both.  `tests/CodeGenTestUtils.h` selects the backend with
`PAYKAN_TEST_BACKEND` and ctest runs the CodeGen and Driver suites once per
backend (`CodeGenTests.llvm`, `CodeGenTests.c`, ...); `SamplesParity` runs
`scripts/samples_parity.py` over the samples corpus on every backend.
