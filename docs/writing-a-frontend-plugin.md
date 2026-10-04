# Writing a frontend plugin

A frontend is a lexer and parser for the Paykan language: it turns source text
into the AST and reports syntax errors. Everything after the AST (Sema, the
lowering to PIR, the backends) is shared, so frontends differ only in how they
parse. Every frontend must build exactly the same AST for the same input;
[`grammar.md`](grammar.md) is the grammar they implement. The built-in
`recursive-descent` frontend is the reference, and the only one in tree:
`-DPAYKAN_FRONTENDS` lists in-tree frontends only, and every other frontend
is built out of tree against an installed PaykanLang, as below. The reference
example of an out-of-tree frontend is the Bison/Flex frontend,
[PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend).

Frontends are plugins in the same way backends are, so most of
[`writing-a-backend.md`](writing-a-backend.md) applies as is: the installed
package, linking the plugin whole into a driver with `paykan_add_driver`, and
[plugin compatibility](writing-a-backend.md#7-plugin-compatibility). This page
covers what differs.

## The grammar is the contract

[`grammar.md`](grammar.md) is the specification of the concrete syntax, and
every frontend must follow it: accept and reject exactly the inputs the
recursive-descent frontend accepts and rejects, and build exactly the same
AST, node for node and location for location (`paykan --dump-ast` prints
it). Diagnostics are the exception: the messages `grammar.md` lists are
binding, but the wording of other syntax errors and the recovery after the
first error are up to each frontend (`grammar.md` section 9). When the
grammar changes, `grammar.md` changes first, then the recursive-descent
frontend, then every out-of-tree frontend; the test support below makes the
comparison mechanical.

## The interface

```cpp
#include "paykan/Frontend.h"

class MyFrontend : public paykan::frontend::Frontend {
public:
  std::string_view name() const override; // "mine"

  paykan::frontend::ParseResult
  parse(std::string_view filename, std::string_view source,
        paykan::ast::ASTContext &ctx, paykan::sema::DiagEngine &diag,
        const paykan::frontend::Options &opts) override;

  // Optional: --dump-tokens.  The default returns false (not supported).
  bool dumpTokens(std::string_view filename, std::string_view source,
                  std::ostream &os) override;
};
```

- `parse()` builds the translation unit in `ctx` and reports every syntax
  error through `diag`, counting it in `ParseResult::ErrorCount`. Nothing may
  escape the call: a frontend built with exceptions catches them all.
- Input nested deeper than `paykan::frontend::kMaxNesting` is rejected with
  `nesting too deep (more than kMaxNesting levels)`, as every frontend does.
- `Options` carries `--trace-parser` / `--trace-scanner`, for a frontend
  that has debug traces.

## Registration and building

```cpp
static std::unique_ptr<paykan::frontend::Frontend> createMyFrontend() {
  return std::make_unique<MyFrontend>();
}
PAYKAN_REGISTER_FRONTEND(mine, "mine", &createMyFrontend);
```

```cmake
find_package(Paykan REQUIRED)             # -DCMAKE_PREFIX_PATH=<prefix>

# A static library linked with Paykan::frontend.  Configure fails unless the
# installed Paykan accepts plugins built with its version; BUILT_WITH pins it.
paykan_add_frontend_plugin(paykan_frontend_mine MyFrontend.cpp)

paykan_add_driver(paykan-mine PLUGINS paykan_frontend_mine)
```

`paykan-mine --list-frontends` then lists `mine`, and `--frontend=mine`
selects it. A frontend built with a PaykanLang version the driver does not
accept is listed as `mine (incompatible: ...)` and can't be selected (exit
status 2); see
[plugin compatibility](writing-a-backend.md#7-plugin-compatibility) for the
rules and how the list is maintained.

## Testing

`paykan --frontend=mine --dump-ast program.pkn` prints the AST.

An installation carries the frontend-parameterized test suites
(`share/paykan/frontend-tests`: the parser and Sema suites, the fuzz smoke
test and the in-process differential check) and the samples corpus
(`share/paykan/samples`, `PAYKAN_SAMPLES_DIR`). `find_package(Paykan)`
provides `paykan_add_frontend_tests`, which builds them against your
frontend; you provide GoogleTest:

```cmake
include(FetchContent)
FetchContent_Declare(googletest
    URL "https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz"
    FIND_PACKAGE_ARGS 1.11 CONFIG NAMES GTest)
FetchContent_MakeAvailable(googletest)

enable_testing()
paykan_add_frontend_tests(mine PLUGINS paykan_frontend_mine)
```

This adds `ParserTests.mine`, `SemaTests.mine` and `FrontendTests.mine`. Each
parses with your frontend and also parses every input with the
installation's recursive-descent frontend, failing on any difference in
accept/reject or in the AST; `FrontendTests.mine` does the same over the whole
samples corpus and fuzzes your frontend for crashes and hangs. Also compare
`--dump-ast` and program output through your driver (`--frontend=mine`
against the default) over a copy of `PAYKAN_SAMPLES_DIR`, since the import
samples write caches next to themselves; the reference example above has
scripts for both.

The suites are those of the installed PaykanLang release, so they check your
frontend against exactly the grammar that release implements. Run them
against every release you support, and against PaykanLang's `develop`
(nightly, for example) to learn about grammar changes early.
`-DPAYKAN_INSTALL_TEST_SUPPORT=OFF` builds an installation without them.
