# The `paykan` command

`paykan` is the compiler, the runner and the build tool in one. This chapter describes its
commands and every option, the backend, and the compilation cache.

## Commands

```text
paykan [options] [run] <source-file> [program arguments]
paykan [options] build <source-file> [-o <output>]
paykan pkm dump <file.pkm> [--section=<name>]
paykan pkm check <file.pkm>
```

- **`run`** (the default, so `paykan prog.pkn` is the same as `paykan run prog.pkn`)
  compiles the program and runs it. Everything after the source file is passed to the
  program, so options for `paykan` itself go before the file. `paykan`'s exit status is the
  program's: what `main` returned, or 134 when the program panicked.
- **`build`** compiles the program into a native executable named by `-o`, or after the
  source file without its extension. `build` takes no program arguments, so its options may
  also come after the file.
- **`pkm`** inspects a module file (see [Module Files](15-modules.md#module-files)): `dump`
  prints it (`--section=manifest|sections|iface|code|symidx|payloads` prints one part;
  `code` is exactly the module's PIR text), `check` prints whether this `paykan` can use it
  and exits with 0 when it can.

The source file is the program's **main file**. Its directory is the source root from which
imports are resolved (see [Modules](15-modules.md)).

When compilation fails, `paykan` prints the errors and exits with status 1. A wrong
option prints a message suggesting `--help`, and also exits with 1.

## Options

### Running and building

| Option | Effect |
|--------|--------|
| `-o <file>` | the output file of `build` |
| `-O<n>` | optimization level, `0` to `3` (also `-O <n>` and `-O=<n>`); the default is `2`, and `-O0` compiles fastest and is easiest to debug |
| `--backend=<name>` | the backend to compile with (see below) |
| `--frontend=<name>` | the parser to read the source with; the built-in one, `recursive-descent`, is the default |
| `--track-heap` | run with heap tracking and print the allocation statistics to standard error at exit ([Memory](16-memory.md)) |
| `--check-only` | parse and type-check, then stop: no code is generated and nothing runs. It also accepts a module that has no `main`, which makes it handy for checking a library module on its own |
| `--emit-pkm` | compile one module and write its `.pkm` module file to `-o <file>`, or next to the source, then stop. The module may have no `main`. Like `build`, it takes no program arguments, so `-o` may follow the file |
| `--module-path=<dir>` | a directory of prebuilt `.pkm` modules, searched for an import whose source is not found (repeatable; then the `:`-separated `$PAYKAN_MODULE_PATH`) |
| `--rebuild-modules` | ignore the `.pkm` cache entries of the imported modules (they are rewritten) |
| `--no-module-cache` | neither read nor write `.pkm` cache entries |
| `--verbose` | print one line per imported module on standard error: where it came from (its source, a cache entry, a prebuilt file) and why a cache entry was not used; and one line per cache entry written |
| `--object=<files>` | link these object files or archives (comma-separated; repeatable): they define the program's `native fn`s ([Native functions](03-functions.md#native-functions)) |
| `--` | ends `paykan`'s options: a file name starting with `-` can follow |

### Looking inside the compiler

These print an intermediate form of the program to standard output and stop.
`--emit-c`, `--emit-llvm` and `--emit-mlir` are the mlir backend's (with another
`--backend` they are an error), and only one of them may be given:

| Option | Prints |
|--------|--------|
| `--emit-c` | the program's C, one strictly conforming C11 translation unit ([The C output](../c-backend.md)) |
| `--emit-llvm` | the program's LLVM IR, as `run` executes it at the `-O<n>` level (the mlir backend's `--emit-source`) |
| `--emit-mlir=<stage>` | the program as MLIR at a stage of the mlir backend: `pir`, the program imported into the `pir` dialect, `scf`, after the lowering both targets share, `llvm`, the LLVM dialect, or `emitc`, EmitC, the C `--emit-c` prints ([the MLIR backend](../design/mlir-backend.md)) |
| `--emit-source` | the selected backend's output, whatever its language (the mlir backend's is LLVM IR) |
| `--emit-pir` | PIR, the backend-neutral intermediate representation ([PIR](../pir.md)) |
| `--emit-ast` | the parsed program in the AST interchange format ([AST format](../plugins/ast-format.md)) |
| `--dump-ast` | the parsed program as a readable tree |
| `--dump-tokens` | the token stream, for frontends that support it |
| `--trace-parser`, `--trace-scanner` | the frontend's debugging traces, if it has them |

`--emit-pir` is a good way to see what the compiler does with your code, including the
reference counting it inserts:

```text
$ paykan --emit-pir hello.pkn
module "hello"
cstr @.str0 = "Hello, world!" len 13

fn @main() -> i64 {
  %str.1 = call @$rt.PaykanString_new(@.str0, 13)
  %shared.2 = box %str.1
  call @"$::io::println<Str>"(%shared.2)
  ret 0
}

fn @"$::io::println<Str>"(%value.1: box) -> void {
  local %value.0: box
  store %value.0, %value.1
  %value.2 = load %value.0
  %value.obj.3 = unbox %value.2
  %mcall.4 = vcall %value.obj.3 : (obj) -> box [1] ()
  call @"$::io::_println"(%mcall.4)
  %5 = load %value.0
  release %5
  ret
}
...
```

### Information

| Option | Effect |
|--------|--------|
| `--version`, `-v` | the version, the frontends and backends with their versions, and the plugin directories |
| `--help`, `-h` | a summary of every option |
| `--list-backends` | the available backends; the default is marked `(default)` |
| `--list-frontends` | the available frontends |

### Plugins

| Option | Effect |
|--------|--------|
| `--plugin=<file>` | load a frontend or backend plugin from a shared library (repeatable) |
| `--no-plugins` | do not search the plugin directories (files named with `--plugin` still load) |

[Extending PaykanLang](../internals.md#extending-paykanlang) explains plugins.

## Backends

A backend turns the compiled program into something that runs. PaykanLang has one,
**`mlir`**, the default, which every build has. It takes the program through MLIR, where one
lowering serves two targets ([the MLIR backend](../design/mlir-backend.md)):

- **LLVM IR.** `run` executes the program in-process with LLVM's JIT compiler, with no C
  compiler involved; `build` writes a native object and links it against the runtime with
  the system C compiler: `$CC` when the variable is set, `cc` otherwise. `-O<n>` selects
  LLVM's optimization pipeline. `--emit-llvm` (or `--emit-source`) prints the IR.
- **C.** `--emit-c` prints the program as one strictly conforming C11 translation unit,
  which any C11 compiler builds against the runtime ([The C output](../c-backend.md)).

`--emit-mlir=<stage>` prints the program at a stage of the pipeline. Other backends are
plugins ([Plugins](#plugins)), selected with `--backend=<name>`. The repository's test suite
runs every sample program, and every program in this documentation, on the mlir backend.

```sh
$ paykan --list-backends
mlir (default): MLIR 22.1.8
$ paykan --backend=mlir prog.pkn
```

`build` needs the PaykanLang runtime (a static library and its header) to link programs
against. An installed `paykan` finds it next to itself (`../lib` and `../include/paykan`);
`PAYKAN_RUNTIME_DIR` points it elsewhere.

## The compilation cache

Compiling a program writes a `.paykan_cache/` directory in the source root. Each imported
module's compiled form is cached there as a `.pkm` module file (`a::b` under
`.paykan_cache/a/b.pkm`): its interface for the type checker and its PIR for the backends,
with a manifest recording the compiler that wrote it, the source it was built from and the
interface of every module it imports. On the next run an entry is used when all of those
are unchanged and the rest is recompiled, so editing `main.pkn` in a large project does not
recompile every library module, editing a function body recompiles one module, and changing
a class or a signature also recompiles the modules that use it. A module that declares
generic classes, structs or functions, or uses another module's, is not cached: a `.pkm`
does not hold generic templates yet, so it is compiled from source every time. `::io` and
`::result` are such modules, and so is every module that calls `io::println`, a generic
function. The mlir backend also caches each imported module's LLVM IR there, as bitcode
(`.paykan_cache/a/b.bc`), and translates the module again only when its PIR or the
compiler changed.

The cache is safe to share and to delete:

- Several `paykan` processes may use one cache at the same time, even with different
  options or backends.
- An entry that is missing, damaged or out of date is simply rebuilt (`--verbose` says why).
- Deleting `.paykan_cache/` costs only a recompile, so add it to your `.gitignore`.

[Modules](15-modules.md#module-files) describes the module
files, the cache and prebuilt modules in detail.

## Environment variables

| Variable | Effect |
|----------|--------|
| `CC` | the C compiler `build` links executables with (default `cc`) |
| `PAYKAN_RUNTIME_DIR` | where to find the runtime (`lib/` and `include/paykan/` under it) |
| `PAYKAN_STDLIB` | the directory of system imports, `import ::name;`, instead of the installed standard library ([Modules](15-modules.md)) |
| `PAYKAN_MODULE_PATH` | `:`-separated directories of prebuilt `.pkm` modules, searched after the `--module-path` directories |
| `PAYKAN_PLUGIN_PATH` | more directories to load plugins from, separated by `:` |
| `PAYKAN_NO_PLUGINS` | set to anything but `0`: do not search the plugin directories, like `--no-plugins` |
| `PAYKAN_TRACK_HEAP` | when set (to any value), a program prints the heap statistics at exit as under `--track-heap`, an executable made by `build` included |
