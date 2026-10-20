# Implementation and plugins

These documents describe how PaykanLang compiles a program and how to extend it. You do not
need them to write PaykanLang programs; [the manual](manual/index.md) and
[the language reference](language/index.md) cover that.

A program goes through three stages. A **frontend** parses the source into an AST (the
built-in one is a recursive-descent parser). The compiler type-checks the AST and lowers it
to **PIR**, a small backend-neutral IR. A **backend** turns the PIR into a running program or
an executable: the built-in `c` backend emits C, and the opt-in `llvm` backend emits LLVM IR.
Frontends and backends can also be added to an installed `paykan` as plugins.

- [PIR](pir.md): the Paykan intermediate representation that every backend consumes.
- [The C backend](c-backend.md): the generated C, the runtime and the build cache.
- [Plugins overview](plugins/overview.md): how `paykan` discovers, checks and loads
  plugins.
- [Writing a backend](writing-a-backend.md) and
  [writing a frontend plugin](writing-a-frontend-plugin.md): step-by-step guides.
- [The plugin API](plugins/plugin-api.md): the C interface plugins implement.
- [The AST interchange format](plugins/ast-format.md): how a frontend plugin hands its AST
  to the compiler.
- [PIR for backend plugins](plugins/pir-for-backends.md): the PIR text a backend plugin
  receives.

## The compilation caches

`paykan` caches compiled modules and objects in a `.paykan_cache` directory next to the
sources (see [the C backend](c-backend.md)). An entry is keyed on its source, its imports
and the format versions, so a compiler whose generated code, runtime ABI, PIR, `.pkm`
format or value layout changed without a version bump still trusts what an older build
wrote, and running those cached modules can crash. After such a change, delete every cache
before running a program or a test:

```sh
find . -name .paykan_cache -type d -prune -exec rm -rf {} +
```

A test failure that disappears after this was a stale cache, not a bug.

## Stability

The plugin interfaces are versioned but not yet covered by a stability promise. A plugin is
built with one PaykanLang version, and `paykan` lists a plugin built with a version it does
not accept as incompatible and will not select it.
