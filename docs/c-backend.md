# The C backend

The C backend (`--backend=c`) translates a verified PIR program
(`docs/pir.md`) into one C11 translation unit against the runtime's
`Runtime.h`, and builds or runs it with the system C compiler.  It is part of
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
  (`Box<int>`) are valid identifiers.  Runtime symbols keep their C names.
* One vtable array per class (`pkvt_pk_<module>_<Class>`, an array of
  generic function pointers), whose address is the class's runtime type
  identity (`match`).  Virtual calls index it with the slot number from PIR.
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
deterministic for a given program.  It assumes an LP64 target (every array and tuple slot is 8 bytes),
like the runtime itself.

## Parity with the LLVM backend

Both backends consume the same PIR semantics (the ownership rules live in the
lowering, `src/Lowering`), so every CodeGen test and every sample must produce
identical stdout, exit codes and zero live heap blocks under `--track-heap`
on both.  `tests/CodeGenTestUtils.h` selects the backend with
`PAYKAN_TEST_BACKEND` and ctest runs the CodeGen and Driver suites once per
backend (`CodeGenTests.llvm`, `CodeGenTests.c`, ...); `SamplesParity` runs
`scripts/samples_parity.py` over the samples corpus on every backend.
