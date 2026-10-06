# Extending PaykanLang

`paykan` is built from replaceable parts. A **frontend** reads source text and produces the
program's syntax tree; the compiler checks it and lowers it to PIR, a small intermediate
representation; a **backend** turns the PIR into something that runs. Besides the built-in
ones, frontends and backends can be added to an installed `paykan` as **plugins**, without
rebuilding PaykanLang. This chapter is a short tour; the linked guides have the details.

## Using a plugin

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
[plugins overview](../plugins/overview.md) describes discovery and the checks in full.

## Writing a plugin

Plugins talk to `paykan` through a C interface, `include/paykan/plugin_api.h`, so they can
be written in any language that can export a C function: C, C++, Rust, Zig and others. The
program crosses the boundary as versioned text, so a plugin needs no PaykanLang library:

- A **frontend** receives the source text and returns the program in the
  [AST interchange format](../plugins/ast-format.md). It must accept the language described
  by the [grammar](../grammar.md) and build the same tree as the built-in parser. The
  [frontend guide](../writing-a-frontend-plugin.md) shows how to write one, and how to run
  PaykanLang's parser and type-checker test suites against it.
- A **backend** receives the checked program as
  [PIR text](../plugins/pir-for-backends.md) and can emit source, build an executable or run
  the program. The [backend guide](../writing-a-backend.md) walks through one, and
  [PIR](../pir.md) specifies the IR.

The repository has a small example of each, both in C:
[`src/Backends/PrintPIR`](../../src/Backends/PrintPIR/print_pir.c), a backend that prints the
PIR it receives, and [`src/Frontends/ASTText`](../../src/Frontends/ASTText/ast_text.c), a
frontend whose input is the AST interchange format itself. A complete external example is the
[Bison frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend), a second parser for
the language built with Bison and Flex. The [plugin API reference](../plugins/plugin-api.md)
documents the C interface field by field.

The plugin interfaces are versioned but not yet stable: until PaykanLang reaches 1.0 they
may change between minor releases, and the CHANGELOG says when they do.

Next: [Appendix: a calculator](15-appendix-calculator.md).
