# The language reference

The reference specifies PaykanLang feature by feature: the syntax, the typing rules, the
run-time behaviour, the limitations of the current release and the compile-time errors
each feature can report. To learn the language, start with [the manual](../manual/index.md)
instead; its chapters link here for the details.

1. [Language basics](01-language-basics.md): program structure, types, variables,
   operators, statements, control flow, scoping, the built-in functions, conversions and
   file I/O.
2. [Functions](02-functions-and-calling.md): declarations, parameters and return values.
3. [Enums](03-enums.md): declaration, variants, equality and matching.
4. [Classes](04-classes.md): fields, methods, constructors, inheritance and the `Obj` root.
5. [Arrays](05-arrays.md): `T[n]`, literals and inferred lengths, indexing, copying,
   nested arrays, fields and parameters, slices (`view T[]`).
6. [Modules](06-modules.md): import forms, path resolution, non-transitivity and the
   compilation cache.
7. [Match statements](07-match-statements.md): type, variant, value and optional mode.
8. [Memory model](08-memory-model.md): reference counting, destruction, weak references and
   ownership transfers.
9. [Tuples](09-tuples.md): tuple types, element access, destructuring, and tuples as values.
10. [Optional types](10-optionals.md): `T?`, `None` and unwrapping.
11. [Generics](11-generics.md): generic classes and functions, and inference.
12. [Traits](12-traits.md): traits, `impl Trait for Type` and bounds on type parameters.
13. [The standard library](13-standard-library.md): `::collections` (lists, linked lists,
    stacks, queues, hash maps and sets, priority queues) and `::memory`.
14. [Structs](14-structs.md): values with named fields, held in place like tuples, with
    `impl` blocks for their methods.
15. [Function types and lambdas](15-function-types.md): `(int, float) -> bool`, lambdas,
    captures, and calling through a function value.

The [grammar](../grammar.md) is the specification of the concrete syntax that every
frontend implements.

## Stability

Tuples, optional types and generics are stable in the 0.1 series: a program that follows
the reference keeps its meaning in every 0.1.x release. See
[Stability](01-language-basics.md#stability) for the limitations that stay.
