# Writing a frontend plugin

A frontend is a lexer and parser for the Paykan language: it turns source text
into the AST and reports syntax errors. Everything after the AST (Sema, the
lowering to PIR, the backends) is shared, so frontends differ only in how they
parse. Every frontend must build exactly the same AST for the same input;
[`grammar.md`](grammar.md) is the grammar they implement. The built-in
`recursive-descent` frontend is the reference; the Bison frontend is the first
frontend shipped as a plugin of its own
([#60](https://github.com/parsabee/PaykanLang/issues/60)).

Frontends are plugins in the same way backends are, so most of
[`writing-a-backend.md`](writing-a-backend.md) applies as is: the installed
package, linking the plugin whole into a driver with `paykan_add_driver`, and
[plugin compatibility](writing-a-backend.md#7-plugin-compatibility). This page
covers what differs.

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
`scripts/diff_frontends.py` compares the ASTs of two frontends over the
samples corpus, which is the check a new frontend is expected to pass:

```sh
scripts/diff_frontends.py --paykan build-mine/paykan-mine \
  --frontends recursive-descent,mine
```
