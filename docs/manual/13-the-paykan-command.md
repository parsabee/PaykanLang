# The `paykan` command

`paykan` is the compiler, the runner and the build tool in one. This chapter describes its
commands and every option, the two backends, and the compilation cache.

## Commands

```text
paykan [options] [run] <source-file> [program arguments]
paykan [options] build <source-file> [-o <output>]
```

- **`run`** (the default, so `paykan prog.pkn` is the same as `paykan run prog.pkn`)
  compiles the program and runs it. Everything after the source file is passed to the
  program, so options for `paykan` itself go before the file. `paykan`'s exit status is the
  program's: what `main` returned, or 134 when the program panicked.
- **`build`** compiles the program into a native executable named by `-o`, or after the
  source file without its extension. `build` takes no program arguments, so its options may
  also come after the file.

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

Compiling a program writes a `.paykan_cache/` directory in the source root. Each module's
compiled form is cached there under a key that covers the module's code, the code of what it
uses from the modules it imports, the compiler, the C compiler and the options. On the next
run, unchanged modules are taken from the cache and only the rest is recompiled, so editing
`main.pkn` in a large project does not recompile every library module, while a change to a
class does recompile every module that uses it.

The cache is safe to share and to delete:

- Several `paykan` processes may use one cache at the same time, even with different
  options or backends.
- An entry that is missing, damaged or written under another key is simply rebuilt.
- Deleting `.paykan_cache/` costs only a recompile, so add it to your `.gitignore`.

[Modules](../language/06-modules.md#compilation-cache) in the reference describes the cache
entries in detail.

## Environment variables

| Variable | Effect |
|----------|--------|
| `CC` | the C compiler the c backend uses (default `cc`) |
| `PAYKAN_RUNTIME_DIR` | where to find the runtime (`lib/` and `include/paykan/` under it) |
| `PAYKAN_STDLIB` | the directory of system imports, `import ::name;` ([Modules](10-modules.md)) |
| `PAYKAN_PLUGIN_PATH` | more directories to load plugins from, separated by `:` |

Next: [Extending PaykanLang](14-extending.md).
