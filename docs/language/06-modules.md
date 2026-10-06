# PaykanLang — Modules

Every `.pkn` source file is a **module**. The file path (relative to the source root, with `/`
replaced by `::` and the `.pkn` extension dropped) is the module's canonical name.

---

## Quick Summary

- `import modulePath;` — bring all public declarations into scope.
- `import modulePath as Alias;` — bring declarations in under `Alias::name`.
- `import base::{a, b as c};` — import several modules of one directory at once.
- `import ::modulePath;` — a system import, looked up in the standard-library directory
  instead of the source root (no standard library ships yet).
- Module paths use `::` as a separator, mirroring directory structure.
- There are no visibility modifiers. Everything declared at the top level of a module is
  importable.
- Imports are non-transitive: importing `A` does not automatically import what `A` imports.
  Values of a type that `A` only reached through its own imports can still flow through `A`'s
  signatures; naming that type requires importing its defining module (see
  [Non-Transitivity](#non-transitivity)).
- Type names are global across a program's import graph: two modules in the same graph cannot
  declare different classes or enums under one name.
- Function names are not: every module (the main file included) may define its own `tag`,
  `new` or `helper`, and `x::tag()`, `y::tag()` and a local `tag()` each call their own
  module's function. One qualifier names one module: two imports cannot both bind `util`.

---

## Import Forms

### Plain Import

```pkn
import utils::math;

result: int = math::add(3, 4);
```

All names from `utils/math.pkn` are accessible as `math::name`.

### Aliased Import

```pkn
import utils::math as M;

result: int = M::add(3, 4);
```

Useful for shortening long module paths or avoiding name collisions. A qualifier (an alias, a
plain import's last path segment, or a full module path) names exactly one module, so
`import a::util; import b::util;` is an error; write `import b::util as bu;` instead. Importing
the same module under a second qualifier is fine.

### Selective Import

```pkn
import lib::{foo, bar};
import ops::{add as plus, mul as times, sub};

x: int = foo::fooVal() + bar::barVal();
y: int = plus::add(3, 4) + times::mul(3, 4) + sub::sub(12, 7);
```

A brace list after a module path imports **several modules** that live under that path, one
per entry: `import lib::{foo, bar};` is exactly `import lib::foo; import lib::bar;`. Each entry
may take its own alias (`add as plus` binds `ops/add.pkn` as `plus`);
an entry without one is qualified by its own name. The entries name modules, not the
declarations inside them, so the module's functions, classes and enums are still reached
through the qualifier (`plus::add`). Every listed module must exist; a missing one is reported
as `module 'lib::bar' not found`. See `samples/imports/04_selective` and
`05_selective_alias`.

### Deeply Nested Module

```pkn
import graphics::shapes::polygon;

p: polygon::Polygon = polygon::Polygon(5, 10.0);
```

---

## Module Path Resolution

Given `import a::b::c`, the compiler looks for:

1. `<source-root>/a/b/c.pkn`, the module's source;
2. when there is no source, a prebuilt module file: `<source-root>/a/b/c.pkm`, then
   `a/b/c.pkm` under each `--module-path=<dir>` directory and each directory of
   `$PAYKAN_MODULE_PATH` (see [Module Files](#module-files)).

The source root is the directory containing the main file passed to the compiler driver. There
is no package registry, and an ordinary import never searches beyond the source root and the
module path.

### System Imports

A leading `::` marks a **system** import: `import ::io;` (or `import ::{io, fs};`) looks for
`io.pkn` in the standard-library directory instead of the source root. That directory is
`$PAYKAN_STDLIB` when the environment variable is set, and `<source-root>/stdlib` otherwise.
A system module is otherwise an ordinary module, and it is cached under
`.paykan_cache/@system/` (see [Module Files](#module-files)).

No standard library ships with PaykanLang yet (it is planned, #113), so a system import only
finds modules you provide yourself in that directory. The builtins (`println`, `Str`, `File`,
`open`, `Stdin`, ...) need no import.

---

## What Can Be Imported

A module exposes its top-level **function** (`fn`), **class**, and **enum** declarations. There
is no explicit `export` keyword and no visibility modifiers — everything declared at the top
level is importable under the chosen qualifier or alias.

An imported enum is named like any other qualified type. Use `mod::Enum` (or `Alias::Enum`, or
the full `path::to::mod::Enum`) wherever a type is expected, and `mod::Enum::Variant` to name a
variant:

```pkn
// file: pal/color.pkn
enum Color { Red, Green, Blue }
fn name(c: Color) -> Str { /* … */ }

// file: main.pkn
import pal::color;

fn main() -> int {
  c: color::Color = color::Color::Green;   // qualified type + variant
  println(color::name(c));                 // pass an enum to an imported function
  return 0;
}
```

---

## Non-Transitivity

If module `B` imports module `A`, and module `C` imports module `B`, then `C` does **not**
automatically have access to what `A` exported. `C` must import `A` directly if it needs
`A`'s declarations.

Precisely, importing `B` brings in **names** only for what `B` itself declares
(`B::fn`, `B::Class`, `B::Enum`). Nothing `B` imported is re-exported under `B`'s qualifier:
`B::A_fn` and `B::A_Class` are errors.

`B`'s declarations may still *mention* `A`'s types — a field, parameter, or return type of
`B`'s classes and functions can be `A::Thing`. Those signatures keep their real types across
the import, so a value obtained through `B` is a fully usable `Thing` (its fields and methods
are available, and it can be passed back into `B`) even though `C` never imported `A`.
What `C` cannot do without importing `A` is *spell* the type: `t = w.get()` is fine,
`t: B::Thing = w.get()` is not. Once `C` imports `A` (in any order relative to `B`),
`A::Thing` names the very same type that `B`'s signatures use.

Because each type is rebuilt once per importing file, keyed by its declared name, type names
are global across the import graph: if two modules reachable from one file both declare a
class (or enum) called `Node`, importing them is an error naming both modules. A file also
cannot declare a class with the same name as one reached through any of its imports.

```pkn
// file: geometry/point.pkn
class Point {
  x: int; y: int;
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

// file: geometry/segment.pkn
import geometry::point;
class Segment {
  a: point::Point; b: point::Point;
  fn __init__(a: point::Point, b: point::Point) { self.a = a; self.b = b; }
}

// file: main.pkn
import geometry::segment;
import geometry::point;   // must import point explicitly to NAME Point

p1: point::Point   = point::Point(0, 0);
p2: point::Point   = point::Point(3, 4);
s:  segment::Segment = segment::Segment(p1, p2);
a = s.a;                  // a Point even without `import geometry::point`
```

---

## Circular Imports

Circular imports (`A` imports `B` and `B` imports `A`) are **not supported** and will result
in a compilation error.

---

## Module Files

A compiled module is a **`.pkm` module file**: the module's interface (what the type checker
needs to check a module that imports it), its PIR (the backend-neutral intermediate
representation both backends generate code from), a symbol index, and a manifest recording
which `paykan` produced it, the source it was built from (size and SHA-256) and the interface
hash of every module it imports. The file carries no backend-specific code, no paths and no
timestamps: the same source always produces the same bytes, and one file serves the c
backend and the llvm backend alike. `paykan pkm dump <file>` prints a file,
`paykan pkm check <file>` says whether this `paykan` can use it, and `paykan --emit-pkm`
writes one ([The `paykan` command](../manual/13-the-paykan-command.md)).

Module files reach a program in two ways.

### The compilation cache

Compiling a program writes the `.pkm` of every module it imports to `.paykan_cache/` under
the source root, named by the module's canonical name: `a::b::c` is cached as
`.paykan_cache/a/b/c.pkm`, a system module under `.paykan_cache/@system/`. The location does
not depend on the directory the compiler is launched from, so `paykan proj/main.pkn` and
`cd proj && paykan main.pkn` share one cache. The main file is never cached.

- **Validity.** An entry is used only when it was written by this very `paykan` (version and
  format numbers), from exactly the current source (size and hash, never timestamps), and
  against the current interface of every module it imports. Editing a function body
  therefore rebuilds that module only; changing a class or a function signature also
  rebuilds the modules that import it, and theirs only if their own interface changed.
  `--verbose` prints, for every import, which file was used or why an entry was not.
- **Robustness.** Entries are written atomically (to a temporary file that is then renamed
  into place), and only after the whole program verified. An entry that is missing,
  truncated, corrupt or out of date is rebuilt. `--rebuild-modules` rebuilds every entry;
  `--no-module-cache` neither reads nor writes them. The directory is purely a cache:
  deleting it at any time is safe and only costs a recompile.
- **The backends' caches.** Each backend still keeps its own generated-code entries beside the
  module files (the llvm backend's `a/b/c.bc`; the c backend's `c.<hash>.c`, `.o` and `.key`,
  one set per key, and the main file's too), validated by their own keys and shared between
  concurrent builds as before. They are the native translation of the PIR a module file
  holds; a later release folds them into the module file.

### Prebuilt modules

Where an import's source is not found, a `.pkm` of the module is used instead: first where
the source would be (`a/b/c.pkm` for `a/b/c.pkn`), then under each `--module-path=<dir>`
directory and each `:`-separated directory of `$PAYKAN_MODULE_PATH`. A source file is always
authoritative when it exists. A prebuilt module is type-checked against from its interface
and compiled from its PIR, so the program builds and runs identically on either backend
without the source (see [`samples/imports/13_pkm`](../../samples/imports/13_pkm/README.md)).

A prebuilt file is used only when this `paykan` can: a file written for another PIR or
runtime ABI version, or built against another interface of a module it imports, is an error
naming what differs (`module 'a::b' (lib/a/b.pkm) was compiled by paykan 0.1.1 (pkm 1.0,
interface 1.0, PIR 1, runtime ABI 6); ... Rebuild it from source.`), never a silent mismatch.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Module not found | The `.pkn` file corresponding to the module path does not exist |
| Unresolved name | Qualified name `Mod::name` where `name` is not in `Mod` (including names `Mod` merely imported) |
| Circular import | Import graph contains a cycle |
| Conflicting qualifier | Two imports bind the same qualifier (alias, last path segment or full path) to different modules |
| Conflicting type name | Two modules in the import graph declare different classes/enums under one name, or a file declares a class named like an imported one |
| Unknown exported type | An import's export record names a type nothing provides (a compiler bug or corrupt cache — reported against the `import`, never silently degraded to `Obj`/`void`) |
