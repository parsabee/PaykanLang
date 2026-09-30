# PaykanLang — Modules

Every `.pkn` source file is a **module**. The file path (relative to the source root, with `/`
replaced by `::` and the `.pkn` extension dropped) is the module's canonical name.

---

## Quick Summary

- `import modulePath;` — bring all public declarations into scope.
- `import modulePath as Alias;` — bring declarations in under `Alias::name`.
- Module paths use `::` as a separator, mirroring directory structure.
- There are no visibility modifiers. Everything declared at the top level of a module is
  importable.
- Imports are non-transitive: importing `A` does not automatically import what `A` imports.
  Values of a type that `A` only reached through its own imports can still flow through `A`'s
  signatures; naming that type requires importing its defining module (see
  [Non-Transitivity](#non-transitivity)).
- Type names are global across a program's import graph: two modules in the same graph cannot
  declare different classes or enums under one name.

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

Useful for shortening long module paths or avoiding name collisions.

### Deeply Nested Module

```pkn
import graphics::shapes::polygon;

p: polygon::Polygon = polygon::Polygon(5, 10.0);
```

---

## Module Path Resolution

Given `import a::b::c`, the compiler looks for:

1. `<source-root>/a/b/c.pkn`

The source root is the directory containing the main file passed to the compiler driver. There
is no package registry or search path beyond the source root.

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

## Compilation Cache

Imported modules are compiled to LLVM bitcode once and cached on disk, so a project whose
modules have not changed only re-generates code for the main file on the next run. The main
file itself is never cached.

- **Location.** The cache lives in `.paykan_cache/` **under the source root** (the main
  file's directory), mirroring the module layout: `a/b/c.pkn` is cached as
  `.paykan_cache/a/b/c.bc`. It does not depend on the directory the compiler is launched
  from, so `paykan proj/main.pkn` and `cd proj && paykan main.pkn` share one cache. A module
  resolved from outside the source root (for example a system module located through
  `PAYKAN_STDLIB`) is cached under the same directory, keyed by its full path.
- **Validity.** Every entry is stamped with a key derived from the module's **source
  text**, the keys of **every module it imports** (recursively), and the compiler and
  generated-code ABI versions. An entry is used only when it was written under exactly the
  key computed for the current compile. Editing a module therefore invalidates it *and*
  every module that imports it, directly or transitively — a changed class layout or
  function signature in `base` never leaks stale code into `mid` or `main`. Timestamps are
  not used.
- **Robustness.** Entries are written atomically (to a temporary file that is then renamed
  into place). An entry that is missing, truncated, corrupt, or produced by a different
  compiler build is ignored and regenerated. The directory is purely a cache: deleting it
  at any time is safe and only costs a recompile.

---

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Module not found | The `.pkn` file corresponding to the module path does not exist |
| Unresolved name | Qualified name `Mod::name` where `name` is not in `Mod` (including names `Mod` merely imported) |
| Circular import | Import graph contains a cycle |
| Conflicting type name | Two modules in the import graph declare different classes/enums under one name, or a file declares a class named like an imported one |
| Unknown exported type | An import's export record names a type nothing provides (a compiler bug or corrupt cache — reported against the `import`, never silently degraded to `Obj`/`void`) |
