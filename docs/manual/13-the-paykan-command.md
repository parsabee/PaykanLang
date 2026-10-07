# The `paykan` command

`paykan` is the compiler, the runner and the build tool in one. This chapter describes its
commands and every option, the two backends, and the compilation cache.

## Commands

```text
paykan [options] [run] <source-file> [program arguments]
paykan [options] build <source-file> [-o <output>]
paykan pkm dump <file.pkm> [--section=<name>]
paykan pkm check <file.pkm>
paykan fmt [--write | --check] [<file.pkn> | <directory> | -]...
```

- **`run`** (the default, so `paykan prog.pkn` is the same as `paykan run prog.pkn`)
  compiles the program and runs it. Everything after the source file is passed to the
  program, so options for `paykan` itself go before the file. `paykan`'s exit status is the
  program's: what `main` returned, or 134 when the program panicked.
- **`build`** compiles the program into a native executable named by `-o`, or after the
  source file without its extension. `build` takes no program arguments, so its options may
  also come after the file.
- **`pkm`** inspects a module file (see [Module Files](../language/06-modules.md#module-files)): `dump`
  prints it (`--section=manifest|sections|iface|code|symidx|payloads` prints one part;
  `code` is exactly the module's PIR text), `check` prints whether this `paykan` can use it
  and exits with 0 when it can.
- **`fmt`** formats source files (see [Formatting](#formatting) below).

The source file is the program's **main file**. Its directory is the source root from which
imports are resolved (see [Modules](10-modules.md)).

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
| `--track-heap` | run with heap tracking and print the allocation statistics to standard error at exit ([Memory](12-memory.md)) |
| `--check-only` | parse and type-check, then stop: no code is generated and nothing runs. It also accepts a module that has no `main`, which makes it handy for checking a library module on its own |
| `--emit-pkm` | compile one module and write its `.pkm` module file to `-o <file>`, or next to the source, then stop. The module may have no `main`. Like `build`, it takes no program arguments, so `-o` may follow the file |
| `--module-path=<dir>` | a directory of prebuilt `.pkm` modules, searched for an import whose source is not found (repeatable; then the `:`-separated `$PAYKAN_MODULE_PATH`) |
| `--rebuild-modules` | ignore the `.pkm` cache entries of the imported modules (they are rewritten) |
| `--no-module-cache` | neither read nor write `.pkm` cache entries |
| `--verbose` | print one line per imported module on standard error: where it came from (its source, a cache entry, a prebuilt file) and why a cache entry was not used |
| `--` | ends `paykan`'s options: a file name starting with `-` can follow |

### Looking inside the compiler

These print an intermediate form of the program to standard output and stop:

| Option | Prints |
|--------|--------|
| `--emit-c` | the generated C (selects the c backend) |
| `--emit-llvm` | the generated LLVM IR (selects the llvm backend, which must be built in) |
| `--emit-source` | the selected backend's output, whatever its language |
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
  call @$rt.Paykan_println(%str.1)
  call @$rt.PaykanString_destroy(%str.1)
  ret 0
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

[Extending PaykanLang](14-extending.md) explains plugins.

## Formatting

`paykan fmt` lays a source file out in two columns: code on the left, and on the right,
from column 46, either a comment or the body of a short function.

```text
class Range {
  bounds: (int, int);                        // a tuple-typed field
  fn __init__(lo: int, hi: int)              { self.bounds = (lo, hi); }
  fn width() -> int                          { return self.bounds.1 - self.bounds.0; }
  fn toString() -> Str {
    return "[" + Str(self.bounds.0) + ", " + Str(self.bounds.1) + "] of width " + Str(self.width());
  }
}
```

- A comment after code starts at column 46, or two spaces after code that reaches past it.
  A comment on a line of its own stays where it is.
- A function or method collapses onto one line, signature on the left and `{ body }` at
  column 46, when its body has no nested block and no comment and the line fits in 100
  columns. A comment after its header or its closing brace moves to the line above. A
  function written on one line that does not fit becomes a block, one statement per line.
- Nothing else changes: code is never re-wrapped or re-spaced, so the formatter keeps every
  token and comment, in order. It removes trailing whitespace and ends the file with one
  newline.

| Option | Effect |
|--------|--------|
| (none) | print the formatted file to standard output; with no file, or `-`, it formats standard input |
| `--write` | rewrite every file that changes |
| `--check` | list the files that are not formatted, and exit with 1 if there are any |

A directory stands for the `.pkn` files below it, so `paykan fmt --write .` formats a whole
project. A file with a lexical error is reported (`file:line:col: error: ...`) and left as
it is.

## Backends

A backend turns the compiled program into something that runs. Two come with PaykanLang:

- **`c`**, the default, which every build has. It translates the program to C11 and
  compiles it with the system C compiler: `$CC` when the variable is set, `cc` otherwise.
  `run` builds the program into a temporary directory and runs it; `build` writes the
  executable. `-O<n>` is passed on to the C compiler.
- **`llvm`**, opt-in when building PaykanLang from source (`-DPAYKAN_BACKENDS="llvm;c"`). It
  generates LLVM IR. `run` executes the program in-process with LLVM's JIT compiler, with no
  C compiler involved; `build` writes a native object and links it with the system C
  compiler.

Both backends implement the same language and produce the same output. The repository's
test suite runs every sample program, and every program in this manual, on each backend that
is built.

```sh
$ paykan --list-backends
c (default)
llvm: LLVM 17.0.6
$ paykan --backend=llvm prog.pkn
```

The C backend needs the PaykanLang runtime (a static library and its header) to link
programs against. An installed `paykan` finds it next to itself (`../lib` and
`../include/paykan`); `PAYKAN_RUNTIME_DIR` points it elsewhere.

## The compilation cache

Compiling a program writes a `.paykan_cache/` directory in the source root. Each imported
module's compiled form is cached there as a `.pkm` module file (`a::b` under
`.paykan_cache/a/b.pkm`): its interface for the type checker and its PIR for the backends,
with a manifest recording the compiler that wrote it, the source it was built from and the
interface of every module it imports. On the next run an entry is used when all of those
are unchanged and the rest is recompiled, so editing `main.pkn` in a large project does not
recompile every library module, editing a function body recompiles one module, and changing
a class or a signature also recompiles the modules that use it. Each backend keeps its own
generated-code cache beside the `.pkm` entries.

The cache is safe to share and to delete:

- Several `paykan` processes may use one cache at the same time, even with different
  options or backends.
- An entry that is missing, damaged or out of date is simply rebuilt (`--verbose` says why).
- Deleting `.paykan_cache/` costs only a recompile, so add it to your `.gitignore`.

[Modules](../language/06-modules.md#module-files) in the reference describes the module
files, the cache and prebuilt modules in detail.

## Environment variables

| Variable | Effect |
|----------|--------|
| `CC` | the C compiler the c backend uses (default `cc`) |
| `PAYKAN_RUNTIME_DIR` | where to find the runtime (`lib/` and `include/paykan/` under it) |
| `PAYKAN_STDLIB` | the directory of system imports, `import ::name;` ([Modules](10-modules.md)) |
| `PAYKAN_MODULE_PATH` | `:`-separated directories of prebuilt `.pkm` modules, searched after the `--module-path` directories |
| `PAYKAN_PLUGIN_PATH` | more directories to load plugins from, separated by `:` |

Next: [Extending PaykanLang](14-extending.md).
