# PaykanLang — Generics (prototype)

Generic classes and functions are parameterised by types. This is a
**prototype**: the surface described here works end to end and is tested, but
constraints, variance and cross-module templates are not part of it yet. The
design, alternatives and open questions are in `proposals/generics.md`.

---

## Quick Summary

- Declare type parameters after the name: `class Box<T> { … }`,
  `class Pair<K, V> { … }`, `fn first<T>(xs: T[]) -> T { … }`.
- Use a generic class by giving it type arguments: `Box<int>`, `Pair<Str, int>`,
  `Box<int>[]`, `Box<Box<int>>`. Each distinct argument list is its own
  ordinary class named after it (`Box<int>` and `Box<Str>` are unrelated).
- Construct with the type arguments — `Box<int>(3)` — or let the compiler
  infer them from the `__init__` arguments: `Box(3)`.
- Call a generic function with explicit arguments, `first<int>(xs)`, or let
  them be inferred from the argument types, `first(xs)`.
- Type parameters name types only; `T` cannot be read as a value or constructed.
- Bodies are checked per instantiation: `Adder<int>` may be fine while
  `Adder<bool>` reports an error that names the instantiation.
- A module's generic declarations are not exported; `mod::Box<int>` is an error.

---

## Declaring a Generic Class

```pkn
class Box<T> {
  v: T;
  fn __init__(v: T) { self.v = v; }
  fn get() -> T      { return self.v; }
  fn set(v: T)       { self.v = v; }
}

class Pair<K, V> {
  k: K;
  v: V;
  fn __init__(k: K, v: V) { self.k = k; self.v = v; }
  fn key() -> K   { return self.k; }
  fn value() -> V { return self.v; }
}
```

- Type parameter names go in `<…>` right after the class name. Any identifier
  works; single capital letters are the convention.
- A parameter may not be repeated (`class P<T, T>`) and may not reuse the name
  of a type in scope (`class Bar<Foo>` when `Foo` is a class, `fn f<Str>`).
- A generic class may extend a **concrete** class: `class Wrap<T> : Base { … }`.
  The superclass itself cannot be generic in the prototype.
- A generic class may mention its own instantiation in signatures
  (`fn link(n: Node<T>)`).

## Declaring a Generic Function

```pkn
fn first<T>(xs: T[]) -> T { return xs[0]; }

fn unbox<T>(b: Box<T>) -> T { return b.get(); }

fn pick<A, B>(a: A, b: B) -> B { return b; }
```

Only free functions can be generic; methods of a non-generic class cannot
declare their own type parameters (a generic class's methods use the class's
parameters).

## Using Type Arguments

Type arguments are accepted wherever a type is written:

```pkn
b: Box<int> = Box<int>(3);            // variable annotation and constructor
p: Pair<Str, int> = Pair<Str, int>("age", 42);
nested: Box<Box<int>> = Box<Box<int>>(b);
arr: Box<int>[] = [Box<int>(1), Box<int>(2)];   // array of instantiations
ba: Box<int[]> = Box<int[]>([1, 2]);            // instantiation of an array

class Holder {
  b: Box<int>;                         // field
  fn __init__() { self.b = Box<int>(1); }
  fn get() -> Box<int> { return self.b; }        // return type
}

fn take(b: Box<Str>) -> int { … }      // parameter

match o {
  x: Box<int> { … }                    // match arm (with binding)
  Box<Str>    { … }
  _           { … }
}
```

`Box<int>` is a real class: it has fields, virtual methods, a constructor and
a destructor, and it takes part in `match`, `==`/`equals`, arrays, `mov` and
module export exactly like a hand-written class.

## Inference

The type arguments of a generic **function** are inferred from the argument
types when they are not written:

```pkn
first([10, 20]);          // T = int
first(["a", "b"]);        // T = Str
unbox(Box<int>(4));       // T = int, found through Box<T>
first<Str>(words);        // explicit — always allowed
```

Inference looks through arrays (`T[]`), tuples (`(A, B)`, element by element),
optionals (`T?` against a `U?` or a plain `U`) and instantiations (`Box<T>`,
also when the argument is a subclass of one). A bare `None` argument is typed
`Obj` and carries no information about `T`: `unwrapOr(None, Node(3))` infers
`T = Node` from the second argument, but `wrap(None)` alone cannot be inferred
and needs `wrap<Node>(None)`. It is exact: a
parameter that appears twice must be deduced as the same type, and no
promotion or subtyping is applied. When inference is impossible or ambiguous
the call is an error with a hint to write the arguments explicitly:

```pkn
fn mk<T>() -> int { return 1; }
mk();                     // error: cannot infer type parameter 'T' of 'mk'
mk<int>();                // ok

fn same<T>(a: T, b: T) -> T { return a; }
same(1, 2.0);             // error: 'T' deduced as both 'int' and 'float'
same<float>(1, 2.0);      // ok (1 promotes to float)
```

A generic **class** may be constructed without type arguments when they can be
inferred from its `__init__` parameters: `Box(3)` is `Box<int>(3)`.

## Type Parameters Are Types

Inside a generic declaration `T` stands for the type argument and nothing
else. It can be used in annotations, `T[]`, `Box<T>`, tuple types `(T, int)`,
optional types `T?`, and `match` arms, but `y = T;` and `T()` are errors
("type parameter 'T' cannot be used as a value").

Tuples and optionals work inside templates and as type arguments:

```pkn
class Pair<A, B> {
  p: (A, B);
  fn __init__(a: A, b: B) { self.p = (a, b); }
  fn swap() -> (B, A) { return (self.p.1, self.p.0); }
}
fn orElse<T>(x: T?, d: T) -> T {
  match x {
    v: T { return v; }
    None { return d; }
  }
}
b = Box<(int, Str)>((1, "a"));     // tuple type argument
m = ident<Node?>(n);               // optional type argument
```

Because each instantiation is checked separately, a `T?` in a template is
rejected for the instantiations where it is not allowed — `Slot<int>`
(`int?`: optional primitives are not supported) or `Slot<(int, Str)>`
(optional tuples are not supported) — with the instantiation named in the
diagnostic.

## How Errors Are Reported

A generic body is checked once per instantiation. An error is reported at the
template's source and says which instantiation was being checked:

```
class Adder<T> {
  v: T;
  fn __init__(v: T) { self.v = v; }
  fn twice() -> T { return self.v + self.v; }
}
a = Adder<int>(1);        // fine
b = Adder<bool>(True);    // error: operator '+' is not defined for types 'bool' and 'bool'
                          // note: in instantiation of 'Adder<bool>' requested here
```

A generic declaration that is never instantiated is only parsed, not checked.
Nested instantiation is bounded: a class that instantiates itself with an
ever-growing argument (`inner: Bad<Bad<T>>`) stops with "exceeds the maximum
instantiation depth".

## Modules

- Generic classes and functions are **not exported**. From an importer,
  `gen::Box<int>`, `gen::Box<int>(1)` and `gen::first<int>(xs)` are errors:
  "generic types cannot be imported yet".
- Concrete instantiations a module makes for itself *are* exported as
  ordinary classes, so a value returned by the module can be used:

```pkn
// lib/gen.pkn
class Box<T> { … }
fn mk() -> Box<int> { return Box<int>(41); }

// main.pkn
import lib::gen;
b = gen::mk();            // b: Box<int>, a class exported by the module
println(StrInt(b.get()));
```

  Declaring your own `class Box<T>` and instantiating `Box<int>` in a module
  that also imports such an exported `Box<int>` is an error: type names are
  global across the import graph.

## Syntax Notes

- `a < b` stays a comparison everywhere; `f<int>(x)` is a generic call because
  the `>` is followed by `(`. The one ambiguous shape is `g(a < b, c > (d))`,
  which reads as a generic call — parenthesise a comparison to get the other
  meaning.
- `>=` is a single token: write `x: Box<int> = …`, not `x: Box<int>=…`.

## Semantic Checks

| Error | Trigger |
|-------|---------|
| Wrong number of type arguments | `Box<int, Str>` for a one-parameter class, `f<int, int>(…)` |
| Unknown generic | `Nope<int>` with no such class |
| Type arguments on a non-generic | `f<int>(1)` where `f` is ordinary; `Str<int>` |
| Missing type arguments | `b: Box = …` for a generic `Box` |
| Type parameter used as a value | `y = T;`, `T()` inside a generic body |
| Duplicate / shadowing type parameter | `class P<T, T>`, `class Bar<Foo>` when `Foo` is a type |
| Name reuse | a class or function named like a generic declaration |
| Error inside an instantiation | any ordinary error in a body, reported with the instantiation note |
| Inference failure / ambiguity | see *Inference* |
| Instantiation depth exceeded | self-instantiation with a growing argument |
| Imported generic | `mod::Box<int>` and friends |

## Not in the Prototype

Constraints/bounds (`T: Comparable`) and interfaces, variance, default type
arguments, explicit specialisation, generic methods on non-generic classes,
generic superclasses, and exporting templates across modules. See
`proposals/generics.md`.
