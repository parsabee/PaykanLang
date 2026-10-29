# The language reference

This reference teaches PaykanLang and specifies it, a chapter per feature. Each chapter
introduces its feature with small programs, then gives the precise rules: the syntax, the
typing rules, the run-time behaviour, the limitations of the current release and the
compile-time errors the feature can report. It is written for a programmer who already
knows another language (C, Java, Python, Go, Swift or similar) but has never seen this one.
The modules a program imports, `::io`, `::error`, `::result`, `::collections`, `::memory`
and `::math`, are documented in [the standard library](../stdlib/index.md).

The chapters can be read in order. Along the way they build a real program: **`tasks`**, a
small to-do list manager. It starts as a few functions in [Basics](02-basics.md), gets a
`Task` class in [Classes](05-classes.md), priorities in [Match statements](08-match.md), a
list in [Arrays](09-arrays.md), and in [`::io`](../stdlib/io.md) it saves its tasks to a file
and takes commands from the command line.

## What is PaykanLang?

PaykanLang (source files end in `.pkn`) is a statically typed language. A program is
checked, lowered to a backend-neutral IR ([PIR](../pir.md)) and compiled by a backend: the
**`mlir` backend**, which takes it through MLIR to LLVM IR (and, with `--emit-c`, to C11).
`paykan run` (or just `paykan prog.pkn`) runs a program in process through LLVM's JIT;
`paykan build prog.pkn -o prog` writes a native executable. Memory lifetimes for objects are
managed by automatic reference counting: there is no garbage collector.

| Pillar | Description |
|--------|-------------|
| **Zero-cost abstractions** | Functions and arrays map directly to machine code with no overhead |
| **Classes with virtual dispatch** | vtable-based method dispatch, single inheritance, method overriding — class instances are heap-allocated with reference counting |
| **Run or build** | `paykan run` executes a program directly for rapid development; `paykan build` produces a standalone native executable |

## Chapters

1. [Getting started](01-getting-started.md): install PaykanLang, run and build your first
   program.
2. [Basics](02-basics.md): program structure, variables and type inference, the built-in
   types, operators, statements, control flow and scoping.
3. [Functions](03-functions.md): declarations, parameters and their modes, return values,
   native functions.
4. [Strings and conversions](04-strings-and-conversions.md): `Str`, `char`, printing, and
   converting between types.
5. [Classes and inheritance](05-classes.md): fields, methods, constructors, overriding,
   pure virtual methods and abstract classes, `toString` and `equals`, and the `Obj` root.
6. [Traits](06-traits.md): traits, `impl Trait for Type` and bounds on type parameters.
7. [Enums](07-enums.md): closed sets of values, variants and equality.
8. [Match statements](08-match.md): type, variant, value and optional mode.
9. [Arrays](09-arrays.md): `T[n]`, literals and inferred lengths, indexing, copying,
   nested arrays, fields and parameters, slices (`view T[]`).
10. [Tuples](10-tuples.md): returning several values, destructuring, tuples as values.
11. [Structs](11-structs.md): values with named fields, held in place like tuples, with
    `impl` blocks for their methods.
12. [Optionals and `None`](12-optionals.md): `T?`, values that may be absent, and
    unwrapping.
13. [Generics](13-generics.md): generic classes and functions, and inference.
14. [Function types and lambdas](14-function-types.md): `(int, float) -> bool`, lambdas,
    captures, and calling through a function value.
15. [Modules and imports](15-modules.md): import forms, path resolution, non-transitivity,
    module files and the compilation cache.
16. [Memory](16-memory.md): reference counting, destruction, `--track-heap`, reference
    cycles, weak references and ownership transfers.
17. [The `paykan` command](17-the-paykan-command.md): every option, the backends and the
    compilation cache.
18. [Appendix: a calculator](18-appendix-calculator.md): a complete multi-module program,
    walked through.

The [grammar](../grammar.md) is the specification of the concrete syntax that every
frontend implements.

## Conventions

Code is shown in blocks like this one, and a block labelled **Output** shows exactly what
the program prints:

```pkn
import ::io;

fn main() -> int {
  io::println("Hello from the reference");
  return 0;
}
```

Output:

```
Hello from the reference
```

Every program shown with its output is compiled and run by PaykanLang's test suite (the
`DocExamples` test), which checks that it prints exactly the output shown and leaks no
memory. A block labelled **Error** shows a program the compiler rejects, with the
diagnostic it prints. Shell commands are shown with a `$` prompt.

## Stability

Tuples ([Tuples](10-tuples.md)), optional types ([Optionals](12-optionals.md), optional
primitives and optional-mode `match` included) and generics ([Generics](13-generics.md))
are **stable**: the syntax and semantics this reference documents for them are covered by
the compatibility promise for the 0.2 series, so a program that follows the reference keeps
its meaning in every 0.2.x release. The limitations each chapter lists stay limitations. An
optional primitive is a value, not a box, and no optional converts to `Obj`
([#96](https://github.com/parsabee/PaykanLang/issues/96); see [Optionals](12-optionals.md)).
