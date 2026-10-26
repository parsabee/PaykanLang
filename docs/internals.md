# Implementation and plugins

These documents describe how PaykanLang compiles a program and how to extend it. You do not
need them to write PaykanLang programs; [the language reference](language/index.md) and
[the standard library](stdlib/index.md) cover that.

A program goes through three stages. A **frontend** parses the source into an AST (the
built-in one is a recursive-descent parser). The compiler type-checks the AST and lowers it
to **PIR**, a small backend-neutral IR. A **backend** turns the PIR into a running program or
an executable: the built-in `mlir` backend takes it through MLIR to LLVM IR, which it runs
with a JIT or builds into a native executable, and to C (`--emit-c`). Frontends and
backends can also be added to an installed `paykan` as plugins.

- [PIR](pir.md): the Paykan intermediate representation that every backend consumes.
- [The C output](c-backend.md): the C that `--emit-c` prints, its strict C11 guarantee and
  the target assumptions the generated code and the runtime share.
- [The MLIR backend](design/mlir-backend.md): the design of the built-in backend.
- [Plugins overview](plugins/overview.md): how `paykan` discovers, checks and loads
  plugins.
- [Writing a backend](writing-a-backend.md) and
  [writing a frontend plugin](writing-a-frontend-plugin.md): step-by-step guides.
- [The plugin API](plugins/plugin-api.md): the C interface plugins implement.
- [The AST interchange format](plugins/ast-format.md): how a frontend plugin hands its AST
  to the compiler.
- [PIR for backend plugins](plugins/pir-for-backends.md): the PIR text a backend plugin
  receives.

## Extending PaykanLang

Frontends and backends can be added to an installed `paykan` as **plugins**, without
rebuilding PaykanLang. This section is a short tour; the documents listed above have the
details.

### Using a plugin

A plugin is a shared library (`.so` on Linux, `.dylib` on macOS) that provides one or more
frontends or backends. Load one for a single command with `--plugin`, then select what it
provides by name:

```sh
$ paykan --plugin=./libpaykan_backend_print_pir.so --backend=print-pir --emit-source hello.pkn
```

To make a plugin always available, put it in a plugin directory. `paykan` searches, in
order: the files named with `--plugin`, each directory in `$PAYKAN_PLUGIN_PATH`, your own
`~/.paykan/plugins/<version>/`, and the installation's `lib/paykan/plugins/<version>/`.
`paykan --version` prints the directories for your installation, and `--list-backends` and
`--list-frontends` show every plugin together with the file it came from. `--no-plugins`
skips the directories.

A plugin is built with a specific PaykanLang version. `paykan` checks each plugin before
running any of its code, and lists one built for a version it does not accept as
incompatible; selecting it fails with exit status 2. The
[plugins overview](plugins/overview.md) describes discovery and the checks in full.

### Writing a plugin

Plugins talk to `paykan` through a C interface, `include/paykan/plugin_api.h`, so they can
be written in any language that can export a C function: C, C++, Rust, Zig and others. The
program crosses the boundary as versioned text, so a plugin needs no PaykanLang library:

- A **frontend** receives the source text and returns the program in the
  [AST interchange format](plugins/ast-format.md). It must accept the language described
  by the [grammar](grammar.md) and build the same tree as the built-in parser. The
  [frontend guide](writing-a-frontend-plugin.md) shows how to write one, and how to run
  PaykanLang's parser and type-checker test suites against it.
- A **backend** receives the checked program as
  [PIR text](plugins/pir-for-backends.md) and can emit source, build an executable or run
  the program. The [backend guide](writing-a-backend.md) walks through one, and
  [PIR](pir.md) specifies the IR.

The repository has a small example of each, both in C:
[`src/Backends/PrintPIR`](../src/Backends/PrintPIR/print_pir.c), a backend that prints the
PIR it receives, and [`src/Frontends/ASTText`](../src/Frontends/ASTText/ast_text.c), a
frontend whose input is the AST interchange format itself. A complete external example is the
[Bison frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend), a second parser for
the language built with Bison and Flex. The [plugin API reference](plugins/plugin-api.md)
documents the C interface field by field.

## The compilation caches

`paykan` caches compiled modules in a `.paykan_cache` directory next to the sources: each
imported module's `.pkm` file and the mlir backend's LLVM bitcode of it (see
[the compilation cache](language/17-the-paykan-command.md#the-compilation-cache)). A `.pkm`
entry is used only when its source and its imports are unchanged and this very `paykan`
wrote it: its manifest records the version and, for a development build, the build ID, a
hash of every file under `src/` and `include/` (`cmake/BuildId.cmake`). An entry another
build wrote is stale and rebuilt, and `--verbose` says so (`is stale (compiled by paykan
0.3.0-dev (46766aa1e1eaf316); this is paykan 0.3.0-dev (b7d975d89acc6d2b))`). A bitcode
entry's key holds the size and modification time of the `paykan` executable, so a rebuilt
compiler translates those again. A release build has no build ID and trusts the entries of
its own version, which the version bump of each release keeps apart from the last one's.

The build ID does not see a change outside `src/` and `include/` that changes the code a
development build generates (a compiler flag set in `cmake/`, say): the compiler built
after it still uses the `.pkm` entries the one before it wrote. After such a change, delete
every cache before running a program or a test:

```sh
find . -name .paykan_cache -type d -prune -exec rm -rf {} +
```

A test failure that disappears after this was a stale cache, not a bug.

## Stability

The plugin interfaces are versioned but not yet covered by a stability promise. A plugin is
built with one PaykanLang version, and `paykan` lists a plugin built with a version it does
not accept as incompatible and will not select it.
