# Standard library

The standard library is written in Paykan and ships with the compiler. It has five
modules: printing and files, errors as values, the containers, the storage the containers
are built on, and the C library's math functions. This page says how to import them, what
they have in common, and where the compiler finds them; each module has a page of its own
that documents every name it declares.

---

## Importing a Module

A module of the library is imported with a **system import**, a module path that starts
with `::` (see [System Imports](../language/15-modules.md#system-imports)), and what it
declares is named with the module's qualifier:

```pkn
import ::io;                    // io::println(...), io::open(...)
import ::error as err;          // err::Error, err::ErrorCode::NotFound
import ::result as res;         // res::Result<T, E>, res::ok(...), res::fail(...)
import ::collections;           // collections::List<int>()
import ::{memory, math};        // memory::Box(0), math::sqrt(2.0)
```

Only the builtins (`Obj`, `Str`, the primitive types and the conversions) need no import.
Printing is not one of them: a program that prints imports `::io`.

---

## Quick Summary

| Module | What it has |
|--------|-------------|
| [`::io`](io.md) | Printing any `Show` value (`print`, `println`, `printerr`, `printerrln`), files (`open` and the class `File`) and the standard input (`stdin()`) |
| [`::error`](error.md) | Errors as values: `ErrorCode`, the standard set of errors; `Error`, a code and a message; `none` and `fromErrno`, which make them |
| [`::result`](result.md) | `Result<T, E>`, the value an operation made (`Ok`) or the error saying why it could not (`Err`); `ok` and `fail`, which make them |
| [`::collections`](collections.md) | The containers `List<T>`, `LinkedList<T>`, `Stack<T>`, `Queue<T>`, `HashMap<K, V>`, `HashSet<K>`, `MinPriorityQueue<T>` and `MaxPriorityQueue<T>`, and `listOf` and `listToString` |
| [`::memory`](memory.md) | `Buffer<T>`, the growable run of slots the containers keep their elements in, and `Box<T: ValueType>`, one value in a shared object |
| [`::math`](math.md) | The C library's `<math.h>` functions on `float` (`sqrt`, `pow`, `exp`, `log`, `sin`, `atan2`, `floor`, `round`, `gamma`, ...), the constants `pi()`, `e()`, `inf()`, `nan()`, and `isNaN`, `isInf`, `isFinite` |

What the modules have in common:

- **Two ways to fail.** What can go wrong in the world (a file that is not there, a read
  at the end of the input, memory that runs out) is reported as a value: a library function
  that can fail returns a [`res::Result<T, err::Error>`](result.md), or an
  [`err::Error`](error.md#error) when it makes no value. A broken precondition, a mistake
  in the program (an index out of range, `pop` on an empty container, `get` of a key a map
  does not have), **panics**: it prints `paykan: <message>` to stderr and aborts the program
  (`paykan run` then exits with status 134; see
  [Panics](../language/04-strings-and-conversions.md#panics)). Each page lists
  which of its operations panic and with what message.
- **Private names.** The library's names that start with `_` are
  [private](../language/15-modules.md#private-to-the-module): a container's `__` fields and
  methods are reachable only from its own methods, and a module's `_` functions and classes
  (`collections::_Entry`, `memory::_new`) only from inside the module. A program that names
  one is rejected (`'__size' is private to 'HashMap<int, int>': it is reachable only from
  its own methods`, `'memory::_new' is private to module '::memory'`). These pages do not
  document them.
- **Type names are global** across an import graph, so a program that imports `::io` (which
  imports `::error` and `::result`) cannot declare its own `File`, `Error` or `ErrorCode`
  (`class 'File' conflicts with an imported type of the same name`, `enum 'ErrorCode' ...`).
  A generic type (`Result`, `List`, `HashMap`) is named through its module
  (`res::Result<T, E>`), so a program may declare its own `Result`. The library's function
  names are not reserved: a program may declare its own `fn print`.
- **Sized numeric types** ([`u8`, `i32`, `unsigned`, `f32`, ...](../language/02-basics.md#sized-numeric-types))
  are elements and keys like `int` and `float` (`List<u8>`, `HashMap<u32, Str>`,
  `Box<f32>`), and a literal passed to a container's method takes the element or key type.
  The `::math` functions take `float`: an `int` converts on its own, a sized float with
  `float(x)`.

---

## An Example

```pkn
import ::io;

import ::collections;

fn main() -> int {
  counts = collections::HashMap<Str, int>();
  words: Str[5] = ["to", "be", "or", "not", "to"];
  i: int = 0;
  while i < words.len() {
    counts.put(words[i], counts.getOr(words[i], 0) + 1);
    i = i + 1;
  }
  io::println("to: " + Str(counts.get("to")) + ", keys: " + Str(counts.len()));

  todo = collections::MinPriorityQueue<Str>();
  todo.push("write", 2);
  todo.push("plan", 1);
  todo.push("ship", 3);
  out = collections::List<Str>();
  while !todo.isEmpty() { out.push(todo.pop()); }
  io::println(collections::listToString(out));
  return 0;
}
```

Output:

```
to: 2, keys: 4
[plan, write, ship]
```

---

## Where the Library Is

A system import looks for `<name>.pkn` (or a prebuilt `<name>.pkm`) in the first of these
directories:

1. `$PAYKAN_STDLIB`, when it is set;
2. the installed library, `<prefix>/lib/paykan/stdlib/<version>/` next to the `paykan`
   executable's `<prefix>/bin/`, when that directory exists;
3. `stdlib/` under the source root (the directory of the main file).

The sources are in [`stdlib/`](../../stdlib) in the repository. The build compiles each into
a `.pkm` named `::<name>`, with the built `paykan` and against the `.pkm` files of the modules
it imports, in `lib/paykan/stdlib/<version>/` of the build tree, next to its `bin/paykan`.
Only the `.pkm` files are staged: they hold all an importer needs, the modules' generic
templates included ([Generics](../language/13-generics.md#modules)), so the sources are not
installed. `cmake --install` installs the `.pkm` files, and the release packages include
them. Configure with `-DPAYKAN_BUILD_STDLIB=OFF` to leave the library out.
It is off by default when cross-compiling, because the built `paykan` cannot run on the
build machine.

A program loads the library's modules from those files and compiles none of them. Each of
its own modules compiles a copy of the library's instantiations it uses
(`collections::List<int>`), kept in its
[compilation cache](../language/15-modules.md#the-compilation-cache) entry, and the program
keeps one copy of each.
