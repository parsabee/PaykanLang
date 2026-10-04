# Writing a frontend

A frontend turns Paykan source into the AST (`include/AST.h`): it scans,
parses and reports syntax errors, and nothing more. Sema, the lowering to PIR
and every backend after it are shared, so two frontends that build the same
AST compile a program identically.

Frontends are plugins, like backends ([writing-a-backend.md](writing-a-backend.md)).
The core defines the interface
([`include/paykan/Frontend.h`](../include/paykan/Frontend.h)) and a registry;
a frontend is a static library that implements the interface and registers a
factory under a name. The driver lists the registered frontends
(`paykan --list-frontends`) and selects one with `--frontend=<name>`.

PaykanLang has one frontend in tree, `recursive-descent`, which is the
default. `-DPAYKAN_FRONTENDS` lists in-tree frontends only; every other
frontend is built **out of tree** against an installed PaykanLang, as below.
The reference example is the Bison/Flex frontend,
[PaykanLang_Bison_Frontend](https://github.com/parsabee/PaykanLang_Bison_Frontend).

**Stability.** The plugin interfaces are not yet stable for out-of-tree
authors (see [writing-a-backend.md](writing-a-backend.md)): build a frontend
against the exact Paykan release it will be linked with.

## 1. The grammar is the contract

[`grammar.md`](grammar.md) is the specification of the concrete syntax, and
every frontend must follow it: accept and reject exactly the inputs the
recursive-descent frontend accepts and rejects, and build exactly the same
AST, node for node and location for location (`paykan --dump-ast` prints
it). Diagnostics are the exception: the messages `grammar.md` lists are
binding, but the wording of other syntax errors and the recovery after the
first error are up to each frontend (`grammar.md` section 9).

When the grammar changes, `grammar.md` changes first, then the
recursive-descent frontend, then every out-of-tree frontend. The test
support below makes the comparison mechanical.

## 2. The interface and registration

```cpp
#include "paykan/Frontend.h"

class MyFrontend : public paykan::frontend::Frontend {
public:
  std::string_view name() const override { return "mine"; }
  paykan::frontend::ParseResult
  parse(std::string_view filename, std::string_view source,
        paykan::ast::ASTContext &ctx, paykan::sema::DiagEngine &diag,
        const paykan::frontend::Options &opts) override;
};

static std::unique_ptr<paykan::frontend::Frontend> createMyFrontend() {
  return std::make_unique<MyFrontend>();
}
PAYKAN_REGISTER_FRONTEND(mine, "mine", &createMyFrontend);
```

`parse` allocates every node in `ctx`, reports every syntax error through
`diag` and counts it in the result, enforces the nesting limit
(`frontend::kMaxNesting`, `nesting too deep`), and lets nothing escape: a
frontend built with exceptions catches them all and turns them into
diagnostics. `Options` carries `--trace-parser` / `--trace-scanner` for a
frontend that has debug traces; `dumpTokens` (`--dump-tokens`) is optional.

## 3. Building it

Install PaykanLang, then build against the package:

```sh
cmake -B build                       # or any configuration
cmake --build build
cmake --install build --prefix /opt/paykan
```

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyPaykanFrontend LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(Paykan REQUIRED)             # -DCMAKE_PREFIX_PATH=/opt/paykan

add_library(paykan_frontend_mine STATIC MyFrontend.cpp)
target_link_libraries(paykan_frontend_mine PUBLIC Paykan::frontend)

# A `paykan` driver with every plugin of the installation plus this one:
# `paykan-mine --frontend=mine program.pkn`.
paykan_add_driver(paykan-mine PLUGINS paykan_frontend_mine)
```

The default frontend of that driver stays the installation's
(`recursive-descent`); users select yours with `--frontend=<name>`.

## 4. Testing it

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
samples corpus and fuzzes your frontend for crashes and hangs. Run the
samples through your driver too (`--frontend=mine` against the default,
comparing `--dump-ast` and program output), on a copy of
`PAYKAN_SAMPLES_DIR`, since the import samples write caches next to
themselves.

The suites are those of the installed PaykanLang release, so they check your
frontend against exactly the grammar that release implements. Run them
against every release you support, and against PaykanLang's `develop`
(nightly, for example) to learn about grammar changes early.
`-DPAYKAN_INSTALL_TEST_SUPPORT=OFF` builds an installation without them.
