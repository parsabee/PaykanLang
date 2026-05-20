# PaykanLang — Module System

This document covers how Paykan source files are organised into modules and how `import`,
`from … import`, and `as` work. PaykanLang is **strictly module-based**: every cross-file
reference is qualified with the module's name.

---

## Quick Summary

- **One module per file.** Each `.pkn` file *is* a module. There is no syntactic module
  declaration — the file is the module.
- **Module name = file basename**, lowercased (`lexer.pkn` → module `lexer`).
- Module names follow Python naming: `lower_snake_case`. The compiler rejects any other
  casing for source files used as modules.
- **All cross-module access is qualified.** After `import other::ast;`, every name
  defined in `ast` is referenced as `ast::Name` — there is no flat-namespace import.
- All `import` declarations must appear at the **top of the file**, before any function,
  struct, class, or enum declaration.
- Imports are **not transitive**. If A imports B and B imports C, A still has to import C
  itself to use C's names. Types only flow through transitively when they appear in a
  signature you call (so the call type-checks).
- Circular imports are detected at compile time and rejected.
- A module cache prevents repeated parsing of the same file in a single compilation.

---

## File Naming

| Filesystem | Module path | Module name in scope |
|---|---|---|
| `src/foo/bar.pkn` | `foo::bar` | `bar` |
| `examples/calc/lexer.pkn` | `examples::calc::lexer` | `lexer` |
| `lib/string.pkn` | `string` (system if shipped with stdlib) | `string` |

- The **last** path segment is what you write at use sites (`lexer::tokenize`).
- The **leading** segments only appear in `import` statements to locate the file.
- Two modules with the same final segment (`a::ast` and `b::ast`) cannot both be imported
  unaliased in the same file — see *Aliases* below.

---

## Import Forms

### 1. Plain import

```
import path::to::module;          // brings module `module` into scope
import std;                       // system module (stdlib search path)

x: int = module::someFn(1, 2);    // qualified access — required
```

The imported name is the **last** path segment. Every exported symbol in that module is
accessed as `module::Name`.

### 2. Selective multi-import (sibling modules)

```
import path::to::{module_a, module_b};
```

Imports several **sibling modules** (each a `.pkn` file in `path/to/`) at once. Each
imported module is brought into scope under its own name, with the same `module::Name`
access pattern.

```
import examples::calc::{ast, lexer, parser};

t: ast::Tok       = ast::Tok::Eof;
toks: ast::Tok[] = lexer::tokenize(src);
ast_root = parser::parse(toks);
```

This is purely a convenience for `import path::to::module_a; import path::to::module_b;` —
the imported names behave identically.

### 3. Directory import (parent-as-namespace)

```
import path::to::dir;
```

If the path resolves to a **directory** rather than a `.pkn` file, the directory becomes
the qualifier and you reach individual modules through it:

```
import examples::calc;       // 'calc' is a directory containing ast.pkn, lexer.pkn, ...

t: calc::ast::Tok = calc::ast::Tok::Eof;
toks: calc::ast::Tok[] = calc::lexer::tokenize(src);
```

This lets you keep a project-internal taxonomy without having to spell each sub-module on
its own `import` line. Resolution is unambiguous: a path is treated as a directory only
if no `.pkn` file with that name exists at that location.

### 4. Aliased import (`as`)

```
import path::to::module::module_a as a;
import other::path::module_a       as legacy_a;

my_a = a::MyType();   
old = legacy_a::Old();
```

The alias replaces the module's surface name in this file only. Aliases solve two problems:

1. **Name collisions** between modules with the same final segment.
2. **Local readability** — short aliases for deep paths (`import vendor::numerics::dense::matrix as mat;`).

---

## Same-Module Privacy ("module-friend access")

Within a single module file, **free functions** (top-level `fn`) declared **after** a class
declaration may freely access that class's `_private` members, subject to one rule:

> **The first parameter must be literally named `self` and typed as the class** (`self: ClassName`).

This is the language's only "friend" mechanism, keyed entirely on the module boundary —
no `friend` keyword, no annotation.

```
// file: counter.pkn
class Counter {
  _n: int;                                // private — only this class & this module
  fn __init__()              { self._n = 0; }
  fn get() -> int            { return self._n; }
  fn inc()                   { self._n = self._n + 1; }
}

// Free function in the SAME module — may read _n directly:
fn dump(self: Counter) {
  std::write(std::out(), "counter = " + (Str)self._n);   // OK: same module
}

fn reset(self: Counter) {
  self._n = 0;                                 
}
```

Calling the same fields from **another module** is rejected:

```
// file: app.pkn
import counter;

fn main() -> int {
  c = counter::Counter();
  c._n = 5;                                  // ERROR: _n is private to module 'counter'
  counter::dump(c);                          // OK
  return 0;
}
```

Rules:

- Privacy is enforced at the **module** level, not the class level.
- The receiver-rule applies: only first-parameter access counts.
- Subclasses in the *same* module also see `_private` fields; subclasses in *other*
  modules don't see anything.

This makes it easy to keep a class's surface area minimal — methods on the class itself
should be only those that genuinely need virtual dispatch; everything else lives next to
the class as free functions in the same file.

---

## Module Path Resolution

| Import form | Resolution |
|-------------|-----------|
| `import a::b::c;` | `<root>/a/b/c.pkn` (file) **or** `<root>/a/b/c/` (directory, if no `.pkn` exists) |
| `import ::std;` / `import std;` | stdlib search path |
| `import a::b::{c, d};` | `<root>/a/b/c.pkn` and `<root>/a/b/d.pkn` |
| `import a::b::c as x;` | as above; bound to local name `x` |

The project root is the directory passed to the compiler driver. Module path segments map
to filesystem path components; the final segment maps to either a `.pkn` file or to a
directory used for namespacing.

---

## Transitivity — Types Only

Imports are **not** name-transitive. If module A imports module B, and module B imports
module C, the names exported by C (functions, etc.) are **not** automatically visible in A.

The **only** transitivity is for **types used in exported function signatures**. If B
imports C and exports a function taking a `c::T`, importing B in A makes that type
visible in A as `c::T` — *only* enough to call B's function correctly. No other names
from C bleed through.

---

## What Can Be Exported

A module exports everything declared at its **top level**:

| Declaration | Exported as |
|-------------|------------|
| `fn` (free function) | `module::name` |
| `class` | `module::Name` |
| `enum` | `module::name_t` (variants: `module::name_t::Variant`) |

Local variables inside `fn` are never visible outside.

---

## `std` and Other System Modules

`import std;` makes the standard library available, all qualified:

| Symbol | Description |
|---|---|
| `std::write`, `std::read`, `std::open`, `std::close` | I/O |
| `std::args() -> Str[]`  | CLI args (after the script path) |
| `std::File` | File I/O |

---

## Semantic Checks (Modules)

| Error | Trigger |
|-------|---------|
| Module not found | Resolved file/directory path does not exist |
| Parse error in imported module | Syntax error in the imported `.pkn` file |
| Sema error in imported module | Semantic error in the imported module |
| Circular import | `a` imports `b` which (transitively) imports `a` |
| Import after declaration | `import` appears after the first function/struct/class/enum |
| Unqualified cross-module access | `Name` used without `module::` prefix when imported |
| Module name collision | Two `import`s land the same surface name; use `as` to disambiguate |
| Private member from foreign module | Reading `obj._field` outside the defining module |
