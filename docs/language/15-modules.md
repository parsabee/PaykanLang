# Modules and imports

Programs are split into **modules**, and in PaykanLang every `.pkn` source file is a
module. This chapter explains how imports work and what a module exposes, splits `tasks`
into a small multi-file project, and then describes the compiled module files (`.pkm`) and
the compilation cache.

## Files are modules

A module's name is its path relative to the **source root**, the directory that holds the
main file (the one you pass to `paykan`), with `/` written as `::` and the `.pkn` dropped.
That name is the module's canonical name. In this project:

```text
project/
├── main.pkn            the main file; project/ is the source root
└── geometry/
    ├── shapes.pkn      module geometry::shapes
    └── units.pkn       module geometry::units
```

`main.pkn` imports a module with `import geometry::shapes;` and then refers to what the
module declares through its last name segment: `shapes::Circle`, `shapes::area(...)`.
Here are the two files. The comment on the first line of each is not part of the language;
it only tells you (and the test suite) where the file lives:

```pkn
// file: geometry/shapes.pkn
class Circle {
  r: float;
}

impl Circle {
  fn __init__(r: float) { self.r = r; }
}

enum Kind { Round, Square }

fn area(c: Circle) -> float {
  return 3.14159 * c.r * c.r;
}
```

```pkn
// file: main.pkn
import ::io;
import geometry::shapes;

fn main() -> int {
  c: shapes::Circle = shapes::Circle(2.0);
  k = shapes::Kind::Round;
  io::println("area " + Str(shapes::area(c)));
  io::println(Str(k == shapes::Kind::Round));
  return 0;
}
```

Output:

```
area 12.5664
True
```

- Everything declared at the top level of a module (functions, classes, structs and enums)
  can be imported, except the names starting with `_`, which are the module's own. There is
  no `export` keyword (see [What Can Be Imported](#what-can-be-imported)).
- Imported names are always qualified: a type is `shapes::Circle`, a variant is
  `shapes::Kind::Round`. Your own module's names stay unqualified.
- Import paths always start from the source root, also inside the modules in
  subdirectories: `geometry/units.pkn` imports its sibling as `import geometry::shapes;`.
- A leading `::` (`import ::io;`) imports a module of the standard library instead
  ([System Imports](#system-imports)).

## Import Forms

```text
import geometry::shapes;              // qualifier: shapes
import geometry::shapes as geo;       // qualifier: geo
import geometry::{shapes, units};     // two modules of one directory
import geometry::{shapes as s, units};
import ::collections;                 // a system import, from the standard library
```

### Plain Import

```pkn
import utils::math;

result: int = math::add(3, 4);
```

All names from `utils/math.pkn` are accessible as `math::name`, the module path's last
segment.

### Aliased Import

```pkn
import utils::math as M;

result: int = M::add(3, 4);
```

An **alias** replaces the qualifier. It is useful for shortening long module paths, and
required when two modules would otherwise share one: a qualifier (an alias, a plain
import's last path segment, or a full module path) names exactly one module, so
`import a::util; import b::util;` is an error (`import of module 'b::util': qualifier
'util' already names module 'a::util' (give one of them an alias with 'as')`, each module
by its canonical name, `::io` for a system module); write `import b::util as bu;` instead.
Importing the same module under a second qualifier is fine.

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

## Module Path Resolution

Given `import a::b::c`, the compiler looks for:

1. `<source-root>/a/b/c.pkn`, the module's source;
2. when there is no source, a prebuilt module file: `<source-root>/a/b/c.pkm`, then
   `a/b/c.pkm` under each `--module-path=<dir>` directory and each directory of
   `$PAYKAN_MODULE_PATH` (see [Module Files](#module-files)).

The source root is the directory containing the main file passed to the compiler driver. Imports
are resolved when the program is compiled; there is no package registry, and an ordinary import
never searches beyond the source root and the module path.

### System Imports

A leading `::` marks a **system** import: `import ::io;` (or `import ::{io, result};`) looks for
`io.pkn`, or the prebuilt `io.pkm` the installed library ships, in the standard-library
directory instead of the source root. That directory is
the first of `$PAYKAN_STDLIB` when the environment variable is set, the installed standard
library (`<prefix>/lib/paykan/stdlib/<version>/` next to the `paykan` executable's
`<prefix>/bin/`) when it exists, and `<source-root>/stdlib`. A system module is otherwise an
ordinary module, and it is cached under `.paykan_cache/@system/` (see
[Module Files](#module-files)).

The standard library ships with the compiler (see [the standard library](../stdlib/index.md)):
`::io` prints, reads and writes files; `::error` holds the errors that library functions
report and `::result` the results they return; `::collections` has lists, linked lists, stacks, queues, hash maps, hash sets and
priority queues; `::math` has the mathematical functions (`math::sqrt`, `math::pi()`, …);
and `::memory` has `Buffer<T>` and `Box<T>`. Printing is `::io`, so a program that prints
imports it (`import ::io;`, then `io::println(...)`). Only the builtins (`Obj`, `Str` and
its methods such as `s.toInt()`, the conversions such as `Str(n)` and `int(f)`, the
primitive types) need no import.

## What Can Be Imported

A module exposes its top-level **function** (`fn`), **class**, **struct** and **enum**
declarations. There is no explicit `export` keyword: everything declared at the top
level is importable under the chosen qualifier or alias, except the names that start with an
underscore, which are [private to the module](#private-to-the-module). A module's **traits**
are not exported yet: an importer cannot name `m::Trait` in a bound or an `impl ... for`
(`unknown trait 'm::Trait'`), and a template of `m` bounded by one, directly or through a
template it uses, cannot be instantiated there (`... another module's traits are not
supported yet (#147)`; see [Traits](06-traits.md#not-supported-yet)). A type's
conformance to a built-in trait is exported with it: `impl Hashable for Key` in the module
makes `m::Key` a `HashMap` key in its importers ([Traits](06-traits.md#hashable)).

An imported enum is named like any other qualified type. Use `mod::Enum` (or `Alias::Enum`, or
the full `path::to::mod::Enum`) wherever a type is expected, and `mod::Enum::Variant` to name a
variant:

```pkn
// file: pal/color.pkn
enum Color { Red, Green, Blue }
fn name(c: Color) -> Str { /* … */ }

// file: main.pkn
import ::io;
import pal::color;

fn main() -> int {
  c: color::Color = color::Color::Green;   // qualified type + variant
  io::println(color::name(c));                 // pass an enum to an imported function
  return 0;
}
```

**Function names are per module.** Every module (the main file included) may define its own
`tag`, `new` or `helper`, and `x::tag()`, `y::tag()` and a local `tag()` each call their own
module's function. Type names, on the other hand, are global across the program
([Non-Transitivity](#non-transitivity)).

**Generics.** A module's generic classes and functions are imported with the rest of it:
`shapes::Box<int>(7)`, `b: shapes::Box<Str>`, `shapes::first(xs)`. Their bodies use what
`shapes` declares and imports, whatever your module calls those names, and a `Box<int>` is
one class however many modules use it, so the values pass freely between them (see
[Generics](13-generics.md#modules)).

### Private to the module

A top-level function, class, struct, enum or trait whose name starts with one underscore
(`_bucket`, `_Entry`) is **private to its module**: inside the module it is an ordinary
declaration, used anywhere; an importer cannot name it. `m::_bucket(1, 2)`, a `m::_Entry`
annotation, `m::_Entry<int, Str>(...)`, `class C : m::_Base` and `impl m::_Trait for C` are
all rejected with `'m::_bucket' is private to module 'm'`. A field or method of a class or
struct whose name starts with one underscore is the module's own the same way: any code of
the module reaches it, an importer does not (`'_w' of 'Pub' is private to module 'm'`). An
initializer written `___init__` is the module's too: only the module's code constructs the
type, or extends it (`'m::T' cannot be constructed here: its initializer '___init__' is
private to module 'm'`, and `'T' cannot be extended here: ...` for a subclass's
`__super__(...)`); an importer makes one through the module's functions
([Classes](05-classes.md#private-fields-and-methods)). Two
underscores (`__count`) are for a member private to its own class or struct
([Classes](05-classes.md#private-fields-and-methods)), and a top-level declaration cannot
have them. A generic class or function the module exports may use its private names in its
bodies; an importer that instantiates it still runs that code, it just cannot write the
names.

Because an importer could not name what it received, a public declaration cannot expose a
private type: a free function's parameter or result, a public field, or a public method of a
public class or struct that names a private type is an error in the declaring module
(`'make' is public but its result names the private type '_Node'; make the function private
or the type public`). A private function, a private field or method, and the members of a
private type may name private types freely. The design, with the alternatives considered, is
in [`docs/design/visibility.md`](../design/visibility.md).

## Non-Transitivity

**Imports are not transitive.** If module `B` imports module `A`, and module `C` imports
module `B`, then `C` does **not** automatically have access to what `A` exported. `C` must
import `A` directly if it needs `A`'s declarations.

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

**Type names are global.** Because each type is rebuilt once per importing file, keyed by its
declared name, type names are global across the import graph (the main file and everything
it imports, directly or not): if two modules reachable from one file both declare a class
(or struct, or enum) called `Node`, importing them is an error naming both modules. A file
also cannot declare a class, struct, enum, trait or function with the same name as a type
reached through any of its imports (`enum 'Node' conflicts with an imported type of the same
name`). A generic type is not reached by its name: its instantiations are
(`Box<int>`), spelled through its module (`m::Box<int>`), so a file may declare its own `Box`.

```pkn
// file: geometry/point.pkn
class Point {
  x: int; y: int;
}

impl Point {
  fn __init__(x: int, y: int) { self.x = x; self.y = y; }
}

// file: geometry/segment.pkn
import geometry::point;
class Segment {
  a: point::Point; b: point::Point;
}

impl Segment {
  fn __init__(a: point::Point, b: point::Point) { self.a = a; self.b = b; }
}

// file: main.pkn
import geometry::segment;
import geometry::point;   // must import point explicitly to NAME Point

fn main() -> int {
  p1: point::Point   = point::Point(0, 0);
  p2: point::Point   = point::Point(3, 4);
  s:  segment::Segment = segment::Segment(p1, p2);
  a = s.a;                // a Point even without `import geometry::point`
  return a.x;
}
```

## Circular Imports

Circular imports are **not supported**: if `a` imports `b`, `b` cannot import `a`, directly
or through other modules, and such a cycle is a compile-time error. Move what both need into
a third module.

A chain of imports is at most 256 modules deep: the main file may import a module that
imports another, and so on, 256 modules down, and the import of a 257th is an error,
`imports nested too deep (more than 256 modules): 'c::m256' is imported 257 modules deep`,
whether the modules on the way are built from source or come from module files
([the grammar](../grammar.md#9-diagnostics-and-recovery)).

## Project: `tasks` as a multi-file program

`tasks` has outgrown a single file. It now has this layout:

```text
tasks/
├── main.pkn          command handling
└── tasks/
    ├── task.pkn      the Priority enum and the Task class
    ├── text.pkn      string helpers
    └── list.pkn      TaskList
```

`tasks/task.pkn` holds the data model:

```pkn
// file: tasks/task.pkn
enum Priority { Low, Normal, High }

fn priorityName(p: Priority) -> Str {
  match p {
    Low    { return "low"; }
    Normal { return "normal"; }
    High   { return "high"; }
  }
}

fn parsePriority(word: Str) -> Priority {
  match word {
    "low"  { return Priority::Low; }
    "high" { return Priority::High; }
    _      { return Priority::Normal; }
  }
}

class Task {
  title: Str;
  done: bool;
  priority: Priority;

  view fn toString() -> Str {
    box = if self.done then "[x] " else "[ ] ";
    marker = if self.priority == Priority::High then " !" else "";
    return box + self.title + marker;
  }
}

impl Task {
  fn __init__(title: Str, priority: Priority) {
    self.title = title;
    self.done = False;
    self.priority = priority;
  }
}
```

`tasks/text.pkn` collects the string helpers from earlier chapters:

```pkn
// file: tasks/text.pkn
fn isSpace(c: char) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

fn slice(s: Str, from: int, to: int) -> Str {
  out = "";
  i = from;
  while i < to {
    out = out + Str(s[i]);
    i = i + 1;
  }
  return out;
}

fn trim(s: Str) -> Str {
  start = 0;
  end = s.len();
  while start < end && isSpace(s[start]) { start = start + 1; }
  while end > start && isSpace(s[end - 1]) { end = end - 1; }
  return slice(s, start, end);
}

// Splits at the first `sep`: (s, "") when there is none.
fn splitOnce(s: Str, sep: char) -> (Str, Str) {
  i = 0;
  while i < s.len() && s[i] != sep { i = i + 1; }
  if i == s.len() { return (s, ""); }
  return (slice(s, 0, i), slice(s, i + 1, s.len()));
}
```

`tasks/list.pkn` imports `tasks::task`, so it names the class `task::Task`:

```pkn
// file: tasks/list.pkn
import ::io;
import tasks::task;

class TaskList {
  tasks: task::Task[8];   // room for eight tasks
  count: int;             // tasks[0] to tasks[count - 1] are in use

  // Adds a task; False if the list is full.
  fn add(title: Str, priority: task::Priority) -> bool {
    if self.count == self.tasks.len() { return False; }
    self.tasks[self.count] = task::Task(title, priority);
    self.count = self.count + 1;
    return True;
  }

  fn complete(n: int) -> bool {
    if n < 1 || n > self.count { return False; }
    self.tasks[n - 1].done = True;
    return True;
  }

  view fn pending() -> int {
    count = 0;
    i = 0;
    while i < self.count {
      if !self.tasks[i].done { count = count + 1; }
      i = i + 1;
    }
    return count;
  }

  view fn show() {
    i = 0;
    while i < self.count {
      io::println(Str(i + 1) + ". " + self.tasks[i].toString());
      i = i + 1;
    }
    io::println(Str(self.pending()) + " of " + Str(self.count) + " to do");
  }
}

impl TaskList {
  fn __init__() {
    self.tasks = [task::Task("", task::Priority::Normal); 8];
    self.count = 0;
  }
}
```

Finally `main.pkn` imports all three, the list module under the alias `tl`. It runs a few
commands of the form `add <title>|<priority>` and `done <number>`; in [`::io`](../stdlib/io.md) they
come from the command line and standard input instead:

```pkn
// file: main.pkn
import ::io;
import tasks::{task, text, list as tl};

fn run(tasks: tl::TaskList, line: Str) {
  command, rest = text::splitOnce(text::trim(line), ' ');
  match command {
    "add" {
      title, priority = text::splitOnce(rest, '|');
      if !tasks.add(title, task::parsePriority(priority)) { io::println("the list is full"); }
    }
    "done" {
      match rest.toInt() {
        n: int { if !tasks.complete(n) { io::println("no task " + rest); } }
        None   { io::println("not a task number: " + rest); }
      }
    }
    "list" { tasks.show(); }
    _      { io::println("unknown command: " + command); }
  }
}

fn main() -> int {
  tasks = tl::TaskList();
  script = ["add buy milk", "add fix the leak|high", "add call mum|low",
            "done 1", "done 9", "list"];
  i = 0;
  while i < script.len() {
    run(tasks, script[i]);
    i = i + 1;
  }
  return 0;
}
```

Output:

```
no task 9
1. [x] buy milk
2. [ ] fix the leak !
3. [ ] call mum
2 of 3 to do
```

Run it from anywhere by naming the main file: `paykan tasks/main.pkn`. The source root is
always the main file's directory, and the compiled modules are cached in
`tasks/.paykan_cache/`, so after a change only the modules affected by it are recompiled
([The compilation cache](#the-compilation-cache)).

## Module Files

A compiled module is a **`.pkm` module file**: the module's interface (what the type checker
needs to check a module that imports it), its templates (the generic classes, structs and
functions an importer instantiates), its code, a symbol index, and a manifest recording
which `paykan` produced it, the source it was built from (size and SHA-256) and the interface
hash of every module it imports. The file carries no paths and no timestamps: the same
source always produces the same bytes.

A module file keeps only the code the module and its importers can reach: the module's own
functions and classes, its copies of the instantiations its interface hands out (a
`Pair<Str, int>` a function returns, whole, since an importer may call any of its methods),
and of its other copies only what its own code uses. Its copies of instantiations stay in
PIR (the backend-neutral intermediate representation), where a program keeps one copy of
each and drops what its `main` does not use; the rest of its code is compiled by the
backend when the file is written (LLVM bitcode, for the mlir backend) and only declared in
its PIR. A program links just the part of that code it uses. Code compiled by one backend
(and one LLVM, for one target) is linked only by that backend: a backend plugin that
compiles nothing writes files whose PIR is all of their code. `paykan pkm dump <file>`
prints a file, `paykan pkm check <file>` says whether this `paykan` can use it,
`paykan pkm extract --object <file>` writes its compiled code as an object file, and
`paykan --emit-pkm` writes one ([The `paykan` command](17-the-paykan-command.md)).

Module files reach a program in two ways.

### The compilation cache

Compiling a program writes the `.pkm` of every module it imports to `.paykan_cache/` under
the source root, named by the module's canonical name: `a::b::c` is cached as
`.paykan_cache/a/b/c.pkm`, a system module under `.paykan_cache/@system/`. The location does
not depend on the directory the compiler is launched from, so `paykan proj/main.pkn` and
`cd proj && paykan main.pkn` share one cache. The main file is never cached. A module that
declares generic classes, structs or functions is cached with its templates, and a module that
uses another module's holds its own copies of the instantiations it uses
([Generics](13-generics.md#modules)).

- **Validity.** An entry is used only when it was written by this very `paykan` (its version,
  its build ID and the format numbers), from exactly the current source (size and hash, never
  timestamps), and
  against the current interface and templates of every module it imports, its code compiled
  by the backend in use (another backend's entry is rebuilt). Editing a function
  body therefore rebuilds that module only; changing a class or a function signature also
  rebuilds the modules that import it, and theirs only if their own interface changed.
  Editing the body of a generic class, struct or function rebuilds every module that imports
  its module, directly or not, since any of them may hold a copy of an instantiation of it.
  `--verbose` prints, for every import, which file was used or why an entry was not. The
  build ID is a hash of the compiler's sources, so two builds of one version from
  different sources do not share entries. A release has none: its version identifies it.
- **Robustness.** Entries are written atomically (to a temporary file that is then renamed
  into place), and only after the whole program verified. An entry that is missing,
  truncated, corrupt or out of date is rebuilt. `--rebuild-modules` rebuilds every entry;
  `--no-module-cache` neither reads nor writes them. The directory is purely a cache:
  deleting it at any time is safe and only costs a recompile.
- **The backend's cache.** The mlir backend keeps the LLVM IR of each imported module's PIR
  (its copies of instantiations) in the cache too, as bitcode (`a/b/c.bc`; the main file's is
  never cached), validated by its own key (the part of that PIR the program uses, the
  compiler and the runtime ABI) and by a hash of the bitcode, checked before the bitcode is
  read, and shared between concurrent builds. An entry whose hash does not match, or whose
  module does not verify, is compiled again (`--verbose` says why). A program compiles only
  the copies its `main` reaches, so two programs that use different ones of a module keep
  recompiling its entry; the module's other code is compiled once, in its module file. The
  cache options are summed up in
  [The `paykan` command](17-the-paykan-command.md#the-compilation-cache).

### Prebuilt modules

Where an import's source is not found, a `.pkm` of the module is used instead: first where
the source would be (`a/b/c.pkm` for `a/b/c.pkn`), then under each `--module-path=<dir>`
directory and each `:`-separated directory of `$PAYKAN_MODULE_PATH`. A source file is always
authoritative when it exists. A prebuilt module is type-checked against from its interface
and linked from its code, so the program builds and runs identically without the source
(see [`samples/imports/13_pkm`](../../samples/imports/13_pkm/README.md)); its compiled code
needs the backend that compiled it.

Whichever way a module file reaches a program, it is checked when it is loaded, before
anything is compiled: its PIR is decoded, verified and checked against its interface (each
class, with its superclass, fields and virtual methods, each struct method and each function
the interface names must be in it), and its compiled code is read and verified by the
backend that links it. A cache entry that fails is rebuilt, and a prebuilt file is an error
at the import (`module 'm' (m.pkm) has a bad CODE section: ...`, `... has a bad OBJ section:
the compiled code does not verify: ...`), its PIR under `--check-only` too. Past these checks
the compiled code is trusted once the file's hashes match. The hashes catch damage,
not a file made to pass them, and LLVM's bitcode reader is not hardened against such bytes,
so use prebuilt modules only from a source you trust.

A prebuilt file is used only when this `paykan` can: a file written by another release
(every format, PIR and the runtime ABI included, is versioned by the release), or built
against another interface of a module it imports, is an error naming what differs
(`module 'a::b' (lib/a/b.pkm) was written by another paykan release: format 0.3.0; this
paykan reads 0.2.0. Rebuild it from source.`), never a silent mismatch. A development build
of `paykan`, which has a build ID (see the cache's validity above), uses only the prebuilt
files it wrote itself: two development builds of one version may lay out what they compile
differently.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Module not found | Neither the `.pkn` file nor a prebuilt `.pkm` of the module path exists: `module 'a::b' not found (tried a/b.pkn, a/b.pkm)` |
| Unresolved name | Qualified name `Mod::name` where `name` is not in `Mod` (including names `Mod` merely imported) |
| Circular import | Import graph contains a cycle |
| Imports nested too deep | A chain of imports more than 256 modules deep: `imports nested too deep (more than 256 modules): 'c::m256' is imported 257 modules deep` |
| Conflicting qualifier | Two imports bind the same qualifier (alias, last path segment or full path) to different modules |
| Conflicting type name | Two modules in the import graph declare different classes/structs/enums under one name, or a file declares a type named like an imported one |
| Unknown exported type | An import's export record names a type nothing provides (a compiler bug or corrupt cache — reported against the `import`, never silently degraded to `Obj`/`void`) |
| Private name imported | `Mod::_name` for a function, class, struct, enum or trait whose name starts with `_`: `'Mod::_name' is private to module 'Mod'` |
| Module-private member imported | `x._f`, `x._m()` or `Mod::T::_s()` on a type of another module: `'_f' of 'T' is private to module 'Mod'` |
| Module-private initializer | `Mod::T(...)`, or `__super__(...)` from a subclass, where `T`'s initializer is `___init__`: `'Mod::T' cannot be constructed here: its initializer '___init__' is private to module 'Mod'` (`'T' cannot be extended here: ...` for `__super__`) |
| Private type exposed | A free function, a public field or a public method naming a `_`-prefixed type |
