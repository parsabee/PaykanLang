# Writing a frontend plugin

A frontend is a lexer and parser for the Paykan language: it turns source text
into the AST and reports syntax errors. Everything after the AST (Sema, the
lowering to PIR, the backends) is shared, so frontends differ only in how they
parse. Every frontend must build exactly the same AST for the same input;
[`grammar.md`](grammar.md) is the grammar they implement. The built-in
`recursive-descent` frontend is the reference, and the only one in tree:
`-DPAYKAN_FRONTENDS` lists in-tree frontends only. Every other frontend is a
**plugin** that the installed `paykan` loads at run time, with no rebuild of
PaykanLang ([`plugins/overview.md`](plugins/overview.md)). The reference
example of an out-of-tree frontend is the Bison/Flex frontend,
[PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend).

A frontend plugin is a shared library with the C interface of
[`include/paykan/plugin_api.h`](../include/paykan/plugin_api.h), the same as a
backend plugin's ([`writing-a-backend.md`](writing-a-backend.md) covers the
parts they share: building, installing, and
[plugin compatibility](writing-a-backend.md#7-plugin-compatibility)). It can
be written in any language that exposes C functions, and it links nothing of
PaykanLang: it receives the source text and returns the program as text in
the [AST interchange format](plugins/ast-format.md), which `paykan` reads,
checks and hands to Sema. This page covers what is particular to frontends.

## The grammar is the contract

[`grammar.md`](grammar.md) is the specification of the concrete syntax, and
every frontend must follow it: accept and reject exactly the inputs the
recursive-descent frontend accepts and rejects, and build exactly the same
AST, node for node and location for location (`paykan --emit-ast` prints it
in the interchange format, `paykan --dump-ast` as a tree). Diagnostics are the
exception: the messages `grammar.md` lists are binding, but the wording of
other syntax errors and the recovery after the first error are up to each
frontend (`grammar.md` section 9). When the grammar changes, `grammar.md`
changes first, then the recursive-descent frontend, then every out-of-tree
frontend; the test support below makes the comparison mechanical.

## The interface

A frontend is a `PaykanFrontend` in the plugin's descriptor
([`plugins/plugin-api.md`](plugins/plugin-api.md#frontends)):

```c
#include "paykan/plugin_api.h"

static const PaykanHost *host;

static int my_parse(void *data, PaykanSession *s, const PaykanFrontendInput *in,
                    PaykanFrontendOutput *out) {
  /* Parse in->source (in->source_size bytes).  For each syntax error:
   *   host->diagnostic(s, PAYKAN_DIAG_ERROR, NULL, line, column, msg, len);
   *   ++out->error_count;
   * On success, print the AST (docs/plugins/ast-format.md) into memory that
   * my_free releases, and set out->ast / out->ast_size. */
}

static void my_free(void *p) { free(p); }

static const PaykanFrontend frontends[] = {{
    .struct_size = sizeof(PaykanFrontend),
    .name = "mine",                  /* --frontend=mine */
    .description = "my parser",
    .parse = my_parse,
    .dump_tokens = NULL,             /* optional: --dump-tokens */
}};

static const PaykanPlugin plugin = {
    .struct_size = sizeof(PaykanPlugin),
    .api_version = PAYKAN_PLUGIN_API_VERSION,
    .build_version = PAYKAN_PLUGIN_BUILD_VERSION,
    .free_memory = my_free,          /* required with frontends */
    .num_frontends = 1, .frontends = frontends,
};

PAYKAN_PLUGIN_EXPORT const PaykanPlugin *paykan_plugin_init(const PaykanHost *h) {
  host = h;
  return &plugin;
}
```

- Report every syntax error through `host->diagnostic` with its line and
  column in the source and count it in `error_count`; `paykan` prints it
  like its own frontend's, with the source line and a caret. After errors
  the AST is not used.
- Nothing may escape the call: a C++ frontend built with exceptions catches
  them all, a Rust one wraps the call in `catch_unwind`.
- Input nested deeper than `in->max_nesting` (512, `frontend::kMaxNesting`)
  is rejected with `nesting too deep (more than 512 levels)`, as every
  frontend does.
- `in->trace_parsing` / `in->trace_scanning` carry `--trace-parser` /
  `--trace-scanner`, for a frontend with debug traces (write them with
  `host->write_output`).
- Imported modules are parsed with the importing program's frontend.

The smallest complete frontend is [`utils/ast-text-frontend`](../utils/ast-text-frontend):
its source language is the AST format itself, so it hands its input back
unchanged. It is also handy for looking at a frontend's output:
`paykan --plugin=libpaykan_frontend_ast_text.so --frontend=ast-text out.ast`
runs a program another tool wrote as an AST.

**Reusing a parser written in C++.** The plugin may be C++ inside and
reuse whatever it likes internally. A frontend that already builds
PaykanLang's C++ AST (`ast::ASTContext`) can link the installed
`Paykan::ast` and `Paykan::ast_interchange` static libraries into its module
and print its tree with `paykan::ast::interchange::write`
([`include/paykan/ast/Interchange.h`](../include/paykan/ast/Interchange.h)),
as [`tests/OutOfTree/frontend-tests/RDPlugin.cpp`](../tests/OutOfTree/frontend-tests/RDPlugin.cpp)
does with the recursive-descent parser. Only the C interface and the text
cross into `paykan`; the price is building with a compiler and standard
library compatible with the installation's, which a frontend that prints
the format itself doesn't need.

## Building and installing

```cmake
find_package(Paykan REQUIRED)             # -DCMAKE_PREFIX_PATH=<prefix>

# A loadable module, libpaykan_frontend_mine.so (.dylib on macOS).
# Configure fails unless the installed Paykan accepts plugins built with its
# version; BUILT_WITH pins it.
paykan_add_frontend_plugin(paykan_frontend_mine mine.c)
paykan_install_plugin(paykan_frontend_mine)   # into PAYKAN_PLUGIN_INSTALL_DIR
```

```sh
paykan --plugin=build/libpaykan_frontend_mine.so --frontend=mine program.pkn
cmake --install build
paykan --frontend=mine program.pkn
paykan --list-frontends     # mine: my parser [<prefix>/lib/paykan/plugins/<version>/libpaykan_frontend_mine.so]
```

A frontend built with a PaykanLang version the installation does not accept
is listed as `mine (incompatible: ...)` and can't be selected (exit status
2); see [plugin compatibility](writing-a-backend.md#7-plugin-compatibility).

## Testing

`paykan --frontend=mine --dump-ast program.pkn` prints the AST, and
`--emit-ast` the interchange text; compare them with the default frontend's.

An installation carries the frontend-parameterized test suites
(`share/paykan/frontend-tests`: the parser and Sema suites, the fuzz smoke
test and the in-process differential check) and the samples corpus
(`share/paykan/samples`, `PAYKAN_SAMPLES_DIR`). `find_package(Paykan)`
provides `paykan_add_frontend_tests`, which runs them against your plugin;
you provide GoogleTest:

```cmake
include(FetchContent)
FetchContent_Declare(googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz"
    FIND_PACKAGE_ARGS 1.11 CONFIG NAMES GTest)
FetchContent_MakeAvailable(googletest)

enable_testing()
paykan_add_frontend_tests(mine PLUGIN paykan_frontend_mine)
```

This adds four tests:

- `ParserTests.mine`, `SemaTests.mine` and `FrontendTests.mine`: each suite
  loads your plugin module at startup with the installed `paykan`'s own
  plugin loader and checks, so the frontend under test is the module itself,
  through its C interface and the AST format. Every input is parsed with your
  frontend and with the installation's recursive-descent frontend, and any
  difference in accept/reject or in the AST fails the test;
  `FrontendTests.mine` does the same over the whole samples corpus and fuzzes
  your frontend for crashes and hangs.
- `InstalledPaykan.mine`: the **installed** `paykan` with only your plugin
  loaded (`--no-plugins --plugin=<module>`) lists it, and prints the same
  `--dump-ast` with `--frontend=mine` as with recursive-descent for every
  file of the corpus.

Also compare program output (`--frontend=mine` against the default) over a
copy of `PAYKAN_SAMPLES_DIR`, since the import samples write caches next to
themselves; the reference example above has a script for it.

The suites are those of the installed PaykanLang release, so they check your
frontend against exactly the grammar that release implements. Run them
against every release you support, and against PaykanLang's `develop`
(nightly, for example) to learn about grammar changes early.
`-DPAYKAN_INSTALL_TEST_SUPPORT=OFF` builds an installation without them.
